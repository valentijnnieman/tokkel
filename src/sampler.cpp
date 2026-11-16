#include "tokkel/sampler.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <cmath>

namespace tokkel {

// Room for the waiting sounds up front, so trigger() never allocates.
Sampler::Sampler(std::uint32_t sample_rate, std::uint32_t channels)
    : sample_rate(sample_rate), sample_channels(std::max<std::uint32_t>(channels, 1)) {
  waiting.reserve(64);
  stopping.reserve(16);
}

bool Sampler::add_file(const std::string &event, const std::string &path) {
  // The decoder converts to the sampler's format: float, its channels and rate.
  ma_decoder_config config = ma_decoder_config_init(ma_format_f32, sample_channels, sample_rate);
  ma_uint64 frame_count = 0;
  void *frames = nullptr;
  if (ma_decode_file(path.c_str(), &config, &frame_count, &frames) != MA_SUCCESS)
    return false;
  const bool added = add_decoded(event, static_cast<const float *>(frames), frame_count);
  ma_free(frames, nullptr);
  return added;
}

bool Sampler::add(const std::string &event, const void *data, std::size_t size) {
  if (data == nullptr || size == 0)
    return false;
  ma_decoder_config config = ma_decoder_config_init(ma_format_f32, sample_channels, sample_rate);
  ma_uint64 frame_count = 0;
  void *frames = nullptr;
  if (ma_decode_memory(data, size, &config, &frame_count, &frames) != MA_SUCCESS)
    return false;
  const bool added = add_decoded(event, static_cast<const float *>(frames), frame_count);
  ma_free(frames, nullptr);
  return added;
}

bool Sampler::add_decoded(const std::string &event, const float *frames,
                          std::uint64_t frame_count) {
  if (frames == nullptr || frame_count == 0)
    return false;
  const std::size_t total = static_cast<std::size_t>(frame_count);

  // Skip the silence before the sound starts, so it is heard the moment it is
  // triggered: some files have up to 0.1 s of it, which makes a quickly
  // repeated sound uneven and lets a restart cut it off before it is heard.
  // "Silence" is below 1% of the file's peak; 1 ms before the sound is kept
  // so its attack isn't clipped.
  float peak = 0.0f;
  for (std::size_t i = 0; i < total * sample_channels; ++i)
    peak = std::max(peak, std::abs(frames[i]));
  std::size_t first = 0;
  while (first < total) {
    bool sounds = false;
    for (std::uint32_t channel = 0; channel < sample_channels; ++channel)
      sounds = sounds || std::abs(frames[first * sample_channels + channel]) > 0.01f * peak;
    if (sounds)
      break;
    ++first;
  }
  if (first == total)
    first = 0; // all silent: keep it as it is
  first -= std::min<std::size_t>(first, sample_rate / 1000);

  auto sample = std::make_shared<Sample>();
  sample->frame_count = total - first;
  sample->frames.assign(frames + first * sample_channels, frames + total * sample_channels);

  std::lock_guard<std::mutex> lock(events_mutex);
  Event &added_to = events[event];
  if (added_to.id == 0)
    added_to.id = next_event_id++;
  added_to.variations.push_back(std::move(sample));
  // So that the first trigger can pick any variation, the first one included.
  added_to.last = added_to.variations.size();
  return true;
}

bool Sampler::trigger(const std::string &event, float volume, bool restart, float delay) {
  Playing sound;
  sound.volume = volume;
  sound.restart = restart;
  sound.start_in = static_cast<std::size_t>(std::max(delay, 0.0f) * static_cast<float>(sample_rate));
  return start(std::move(sound), event);
}

bool Sampler::trigger_at(const std::string &event, const Vec3 &position, float volume,
                         bool restart, float delay) {
  Playing sound;
  sound.volume = volume;
  sound.restart = restart;
  sound.start_in = static_cast<std::size_t>(std::max(delay, 0.0f) * static_cast<float>(sample_rate));
  sound.placed = true;
  sound.at = position;
  return start(std::move(sound), event);
}

// Pick the variation `start` plays of `event`, and hand it to the audio thread.
bool Sampler::start(Playing start, const std::string &event) {
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    auto found = events.find(event);
    if (found == events.end() || found->second.variations.empty())
      return false;
    Event &triggered = found->second;

    const std::size_t count = triggered.variations.size();
    std::size_t pick = 0;
    if (count > 1 && triggered.last < count) {
      // Any but the last one: pick among the others, then step over it.
      pick = std::uniform_int_distribution<std::size_t>(0, count - 2)(random);
      if (pick >= triggered.last)
        ++pick;
    } else if (count > 1) {
      pick = std::uniform_int_distribution<std::size_t>(0, count - 1)(random);
    }
    triggered.last = pick;
    start.sample = triggered.variations[pick];
    start.event = triggered.id;
  }

  std::lock_guard<std::mutex> lock(waiting_mutex);
  // Restarting the same event at the same moment more than once, before the
  // audio thread got to it, is one restart: the latest one. Otherwise they
  // would all start together and cut each other off at once.
  if (start.restart) {
    for (Playing &waits : waiting) {
      if (waits.restart && waits.event == start.event && waits.start_in == start.start_in) {
        waits = std::move(start);
        return true;
      }
    }
  }
  if (waiting.size() < waiting.capacity()) // else dropped: far too many at once
    waiting.push_back(std::move(start));
  return true;
}

void Sampler::stop(const std::string &event) {
  std::uint32_t id = 0;
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    auto found = events.find(event);
    if (found == events.end())
      return;
    id = found->second.id;
  }
  std::lock_guard<std::mutex> lock(waiting_mutex);
  waiting.erase(std::remove_if(waiting.begin(), waiting.end(),
                               [id](const Playing &waits) { return waits.event == id; }),
                waiting.end());
  stopping.push_back(id); // here, never on the audio thread, it may allocate
}

void Sampler::stop_all() {
  std::lock_guard<std::mutex> lock(waiting_mutex);
  waiting.clear();
  stopping.push_back(0);
}

void Sampler::set_listener(const Listener &listener) {
  std::lock_guard<std::mutex> lock(listener_mutex);
  listener_shared = listener;
}

void Sampler::placed_gains(const Listener &listener, const Vec3 &at, float &left,
                           float &right) {
  // A gentle pan: the far side keeps 40%, so a voice off to one side is
  // still heard in both ears, as in a room.
  const float gain = listener.gain(at);
  const float pan = listener.pan(at);
  left = gain * (pan > 0.0f ? 1.0f - 0.6f * pan : 1.0f);
  right = gain * (pan < 0.0f ? 1.0f + 0.6f * pan : 1.0f);
}

void Sampler::render(float *out, std::uint32_t frames, std::uint32_t channels) {
  // try_lock, like waiting_mutex below: a listener being set right now is
  // heard from the next block.
  if (listener_mutex.try_lock()) {
    listener_audio = listener_shared;
    listener_mutex.unlock();
  }

  // try_lock, because the audio thread must never wait: if a sound is being
  // triggered right now, the waiting ones start with the next block instead.
  if (waiting_mutex.try_lock()) {
    // Stops first: what was triggered after a stop() isn't cut off by it.
    for (std::uint32_t id : stopping) {
      for (Playing &voice : voices) {
        if (!voice.sample || (id != 0 && voice.event != id))
          continue;
        if (voice.start_in > 0)
          voice.sample.reset(); // not started yet: never will
        else
          voice.cut = true;
      }
    }
    stopping.clear();
    for (Playing &start : waiting) {
      // A free voice, or else the one that has the least left to play.
      Playing *voice = &voices[0];
      std::size_t least_left = static_cast<std::size_t>(-1);
      for (Playing &candidate : voices) {
        if (!candidate.sample) {
          voice = &candidate;
          break;
        }
        const std::size_t left = candidate.sample->frame_count - candidate.position;
        if (left < least_left) {
          least_left = left;
          voice = &candidate;
        }
      }
      *voice = std::move(start);
    }
    waiting.clear();
    waiting_mutex.unlock();
  }

  std::fill(out, out + static_cast<std::size_t>(frames) * channels, 0.0f);

  // A sound starting in this block with `restart` cuts off what its event is
  // already playing.
  for (Playing &voice : voices) {
    if (!voice.sample || !voice.restart || voice.start_in >= frames)
      continue;
    for (Playing &playing : voices)
      if (&playing != &voice && playing.sample && playing.event == voice.event &&
          playing.start_in == 0)
        playing.cut = true;
    voice.restart = false;
  }

  // A sound that is cut off fades out over 5 ms: stopping it dead would click.
  const float fade_step = 1.0f / (0.005f * static_cast<float>(sample_rate));

  for (Playing &voice : voices) {
    if (!voice.sample)
      continue;
    // A delayed sound waits, then starts partway into a block.
    std::size_t offset = 0;
    if (voice.start_in > 0) {
      if (voice.start_in >= frames) {
        voice.start_in -= frames;
        continue;
      }
      offset = voice.start_in;
      voice.start_in = 0;
    }
    const Sample &sample = *voice.sample;
    const std::size_t count =
        std::min<std::size_t>(frames - offset, sample.frame_count - voice.position);
    // A placed sound glides from the gains it ended the last block on to
    // this block's, so a moving listener doesn't make it click.
    float left = 1.0f, right = 1.0f;
    if (voice.placed)
      placed_gains(listener_audio, voice.at, left, right);
    if (!voice.gains_set) {
      voice.left = left;
      voice.right = right;
      voice.gains_set = true;
    }
    const float left_step = count > 0 ? (left - voice.left) / static_cast<float>(count) : 0.0f;
    const float right_step = count > 0 ? (right - voice.right) / static_cast<float>(count) : 0.0f;
    const float *in = sample.frames.data() + voice.position * sample_channels;
    float *mixed = out + offset * channels;
    std::size_t frame = 0;
    for (; frame < count && voice.fade > 0.0f; ++frame) {
      const float gain = voice.volume * voice.fade;
      const float left_gain = voice.left + left_step * static_cast<float>(frame);
      const float right_gain = voice.right + right_step * static_cast<float>(frame);
      // Normally the same channels on both sides; if the engine asks for
      // more, the extra ones repeat the sample's last channel. Left and
      // right are the first two; any others (and a mono mix) get both.
      for (std::uint32_t channel = 0; channel < channels; ++channel) {
        const float side = channels < 2       ? 0.5f * (left_gain + right_gain)
                           : channel == 0     ? left_gain
                           : channel == 1     ? right_gain
                                              : 0.5f * (left_gain + right_gain);
        mixed[channel] += gain * side * in[std::min(channel, sample_channels - 1)];
      }
      mixed += channels;
      in += sample_channels;
      if (voice.cut)
        voice.fade -= fade_step;
    }
    voice.left = left;
    voice.right = right;
    voice.position += frame;
    if (voice.position >= sample.frame_count || voice.fade <= 0.0f)
      voice.sample.reset(); // free again; the event still holds the sample
  }
}

} // namespace tokkel
