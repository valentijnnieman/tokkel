#include "tokkel/tokkel.hpp"

#include "tokkel/sampler.hpp"
#include "tokkel/synth.hpp"

#include <FreeVerb.h>
#include <Stk.h>
#include <memory>
#include <miniaudio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace tokkel {

namespace {

// A Instrument seen by miniaudio as an endless, unseekable data source.
struct Voice {
  ma_data_source_base base{}; // must stay first: miniaudio casts Voice* to it
  ma_sound sound{};
  std::shared_ptr<Instrument> instrument;
  ma_uint32 channels = 0;
  ma_uint32 sample_rate = 0;
  bool sound_ready = false;
  bool base_ready = false;

  ~Voice() {
    // The sound first: uninit detaches it from the graph, after which the
    // audio thread no longer reads from this voice.
    if (sound_ready)
      ma_sound_uninit(&sound);
    if (base_ready)
      ma_data_source_uninit(&base);
  }
};

ma_result voice_read(ma_data_source *data_source, void *frames_out, ma_uint64 frame_count,
                     ma_uint64 *frames_read) {
  auto *voice = static_cast<Voice *>(data_source);
  if (frames_out != nullptr)
    voice->instrument->render(static_cast<float *>(frames_out), static_cast<std::uint32_t>(frame_count),
                          voice->channels);
  if (frames_read != nullptr)
    *frames_read = frame_count;
  return MA_SUCCESS;
}

ma_result voice_seek(ma_data_source *, ma_uint64) { return MA_NOT_IMPLEMENTED; }

ma_result voice_format(ma_data_source *data_source, ma_format *format, ma_uint32 *channels,
                       ma_uint32 *sample_rate, ma_channel *channel_map, size_t channel_map_cap) {
  auto *voice = static_cast<Voice *>(data_source);
  if (format != nullptr)
    *format = ma_format_f32;
  if (channels != nullptr)
    *channels = voice->channels;
  if (sample_rate != nullptr)
    *sample_rate = voice->sample_rate;
  if (channel_map != nullptr)
    ma_channel_map_init_standard(ma_standard_channel_map_default, channel_map, channel_map_cap,
                                 voice->channels);
  return MA_SUCCESS;
}

ma_data_source_vtable voice_vtable = {
    voice_read, voice_seek, voice_format, nullptr, nullptr, nullptr, 0,
};

// The main reverb, on the whole mix: a stereo FreeVerb added to the dry
// sound. Its settings are changed from any thread through `amount`, and
// picked up by process() on the audio thread.
struct MainReverb {
  std::atomic<float> amount{0.0f}; // 0 (none) to 1
  std::unique_ptr<stk::FreeVerb> verb;
  ma_uint32 channels = 2;
  float glide = 0.001f; // per frame: how fast `wet` follows the amount
  float wet = 0.0f;     // how much of the reverb is heard now
  float room = -1.0f;   // the room size the verb was last given

  void process(const float *in, float *out, ma_uint32 frames) {
    const float wanted = std::clamp(amount.load(std::memory_order_relaxed), 0.0f, 1.0f);
    // More reverb is a bigger room as well as more of it.
    const float wanted_room = 0.6f + 0.35f * wanted;
    if (wanted > 0.0f && std::fabs(wanted_room - room) > 0.001f) {
      verb->setRoomSize(wanted_room);
      room = wanted_room;
    }
    const float wanted_wet = 0.5f * wanted;
    if (wanted_wet == 0.0f && wet < 0.0001f) {
      // Off: pass the mix through, and start clean when it comes back.
      if (wet > 0.0f) {
        wet = 0.0f;
        verb->clear();
      }
      std::copy(in, in + static_cast<std::size_t>(frames) * channels, out);
      return;
    }
    for (ma_uint32 frame = 0; frame < frames; ++frame) {
      // Glide to the new amount, so a jump in it doesn't click.
      wet += glide * (wanted_wet - wet);
      const float *from = in + static_cast<std::size_t>(frame) * channels;
      float *to = out + static_cast<std::size_t>(frame) * channels;
      const float left = from[0];
      const float right = channels > 1 ? from[1] : left;
      verb->tick(left, right);
      to[0] = left + wet * static_cast<float>(verb->lastOut(0));
      if (channels > 1)
        to[1] = right + wet * static_cast<float>(verb->lastOut(1));
      for (ma_uint32 channel = 2; channel < channels; ++channel)
        to[channel] = from[channel];
    }
  }
};

// A miniaudio node running the main reverb, between the mix and the output.
struct MainNode {
  ma_node_base base{}; // must stay first: miniaudio casts MainNode* to it
  MainReverb *reverb = nullptr;
};

void main_node_process(ma_node *node, const float **frames_in, ma_uint32 *,
                       float **frames_out, ma_uint32 *frame_count_out) {
  auto *main = static_cast<MainNode *>(node);
  main->reverb->process(frames_in[0], frames_out[0], *frame_count_out);
}

ma_node_vtable main_node_vtable = {main_node_process, nullptr, 1, 1, 0};

} // namespace

struct Engine::Impl {
  ma_engine engine{};
  bool ok = false;
  bool offline = false;
  std::mutex voices_mutex;
  std::unordered_map<VoiceId, std::unique_ptr<Voice>> voices;
  VoiceId next_voice = 1;

  // Everything play() starts goes into `mix`, which goes through `main`
  // (the main reverb) to the output. Without them (if they failed to start)
  // sounds go to the output directly.
  ma_sound_group mix{};
  MainNode main;
  MainReverb main_reverb;
  bool main_ready = false, mix_ready = false;

  struct Named {
    std::shared_ptr<Instrument> instrument;
    VoiceId voice = 0;
  };
  mutable std::mutex named_mutex;
  std::unordered_map<std::string, Named> named;

  // set_listener()'s, which add_sampler() gives a new sampler too. Under
  // named_mutex.
  Listener listener;

  // Play `source` on `engine` and keep it under `name`, stopping what had
  // the name.
  bool add_named(Engine &engine, const std::string &name, std::shared_ptr<Instrument> source,
                 Named added) {
    added.voice = engine.play(std::move(source));
    if (added.voice == 0)
      return false;
    VoiceId replaced = 0;
    {
      std::lock_guard<std::mutex> lock(named_mutex);
      Named &slot = named[name];
      replaced = slot.voice;
      slot = std::move(added);
    }
    if (replaced != 0)
      engine.stop(replaced);
    return true;
  }
};

Engine::Engine(const EngineConfig &config) : impl(std::make_unique<Impl>()) {
  ma_engine_config engine_config = ma_engine_config_init();
  engine_config.sampleRate = config.sample_rate;
  engine_config.channels = config.channels;
  engine_config.noDevice = config.offline ? MA_TRUE : MA_FALSE;
  impl->offline = config.offline;
  impl->ok = ma_engine_init(&engine_config, &impl->engine) == MA_SUCCESS;
  if (!impl->ok)
    return;
  stk::Stk::setSampleRate(static_cast<stk::StkFloat>(sample_rate()));

  // The main reverb, between the mix and the output.
  MainReverb &reverb = impl->main_reverb;
  reverb.verb = std::make_unique<stk::FreeVerb>(); // now that STK knows the rate
  reverb.verb->setEffectMix(1.0); // only the reverb: process() adds the dry mix
  reverb.verb->setDamping(0.5);
  reverb.channels = channels();
  reverb.glide = 1.0f / (0.05f * static_cast<float>(sample_rate())); // ~50 ms
  impl->main.reverb = &reverb;
  const ma_uint32 node_channels = channels();
  ma_node_config node_config = ma_node_config_init();
  node_config.vtable = &main_node_vtable;
  node_config.pInputChannels = &node_channels;
  node_config.pOutputChannels = &node_channels;
  impl->main_ready =
      ma_node_init(ma_engine_get_node_graph(&impl->engine), &node_config, nullptr,
                   &impl->main) == MA_SUCCESS &&
      ma_node_attach_output_bus(&impl->main, 0, ma_engine_get_endpoint(&impl->engine), 0) ==
          MA_SUCCESS;
  if (impl->main_ready) {
    // The mix as it is: not placed in space, not repitched.
    impl->mix_ready = ma_sound_group_init(&impl->engine,
                                          MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH,
                                          nullptr, &impl->mix) == MA_SUCCESS;
    if (impl->mix_ready &&
        ma_node_attach_output_bus(&impl->mix, 0, &impl->main, 0) != MA_SUCCESS) {
      ma_sound_group_uninit(&impl->mix);
      impl->mix_ready = false;
    }
  }
}

Engine::~Engine() {
  if (!impl->ok)
    return;
  impl->named.clear();
  impl->voices.clear(); // sounds must go before their group and engine
  if (impl->mix_ready)
    ma_sound_group_uninit(&impl->mix);
  if (impl->main_ready)
    ma_node_uninit(&impl->main, nullptr);
  ma_engine_uninit(&impl->engine);
}

bool Engine::ok() const { return impl->ok; }

std::uint32_t Engine::sample_rate() const {
  return impl->ok ? ma_engine_get_sample_rate(&impl->engine) : 0;
}

std::uint32_t Engine::channels() const {
  return impl->ok ? ma_engine_get_channels(&impl->engine) : 0;
}

void Engine::set_master_volume(float volume) {
  if (impl->ok)
    ma_engine_set_volume(&impl->engine, volume);
}

bool Engine::play_file(const std::string &path) {
  return impl->ok && ma_engine_play_sound(&impl->engine, path.c_str(), nullptr) == MA_SUCCESS;
}

VoiceId Engine::play(std::shared_ptr<Instrument> instrument, float volume) {
  if (!impl->ok || !instrument)
    return 0;

  auto voice = std::make_unique<Voice>();
  voice->instrument = std::move(instrument);
  voice->channels = channels();
  voice->sample_rate = sample_rate();

  ma_data_source_config source_config = ma_data_source_config_init();
  source_config.vtable = &voice_vtable;
  if (ma_data_source_init(&source_config, &voice->base) != MA_SUCCESS)
    return 0;
  voice->base_ready = true;

  // No spatialization or pitch: a Instrument is already at the engine's format.
  const ma_uint32 flags = MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH;
  ma_sound_group *mix = impl->mix_ready ? &impl->mix : nullptr;
  if (ma_sound_init_from_data_source(&impl->engine, &voice->base, flags, mix, &voice->sound) !=
      MA_SUCCESS)
    return 0;
  voice->sound_ready = true;

  ma_sound_set_volume(&voice->sound, volume);
  if (ma_sound_start(&voice->sound) != MA_SUCCESS)
    return 0;

  std::lock_guard<std::mutex> lock(impl->voices_mutex);
  const VoiceId id = impl->next_voice++;
  impl->voices.emplace(id, std::move(voice));
  return id;
}

void Engine::set_volume(VoiceId voice, float volume) {
  std::lock_guard<std::mutex> lock(impl->voices_mutex);
  auto found = impl->voices.find(voice);
  if (found != impl->voices.end())
    ma_sound_set_volume(&found->second->sound, volume);
}

void Engine::stop(VoiceId voice) {
  std::unique_ptr<Voice> stopped;
  {
    std::lock_guard<std::mutex> lock(impl->voices_mutex);
    auto found = impl->voices.find(voice);
    if (found == impl->voices.end())
      return;
    stopped = std::move(found->second);
    impl->voices.erase(found);
  }
  // `stopped` is torn down here, outside the lock.
}

void Engine::set_reverb(float amount) {
  impl->main_reverb.amount.store(std::clamp(amount, 0.0f, 1.0f), std::memory_order_relaxed);
}

bool Engine::add_instrument(const std::string &name, std::shared_ptr<Instrument> instrument) {
  if (!impl->ok || !instrument)
    return false;
  Impl::Named added;
  added.instrument = instrument;
  return impl->add_named(*this, name, std::move(instrument), std::move(added));
}

std::shared_ptr<Instrument> Engine::instrument(const std::string &name) const {
  std::lock_guard<std::mutex> lock(impl->named_mutex);
  auto found = impl->named.find(name);
  return found == impl->named.end() ? nullptr : found->second.instrument;
}

std::vector<std::string> Engine::instruments() const {
  std::vector<std::string> names;
  std::lock_guard<std::mutex> lock(impl->named_mutex);
  for (const auto &[name, named] : impl->named)
    if (named.instrument)
      names.push_back(name);
  return names;
}

bool Engine::set_parameter(const std::string &instrument_name, const std::string &name,
                           float value) {
  std::shared_ptr<Synth> synth = std::dynamic_pointer_cast<Synth>(instrument(instrument_name));
  if (!synth)
    return false;
  synth->set_parameter(name, value);
  return true;
}


bool Engine::play_note(const std::string &name, float frequency, float amplitude, float sustain,
                       float delay, float speed) {
  std::shared_ptr<Synth> synth = std::dynamic_pointer_cast<Synth>(instrument(name));
  if (!synth)
    return false;
  synth->play_note(frequency, amplitude, sustain, delay);

  if(synth->tokkel) {
    for(std::size_t i = 1; i < synth->tokkel_amt+1; i++) {
      double t_delay = (i / 16.0) * speed;

      synth->play_note(frequency * (i * 1.5), amplitude, sustain, i * t_delay);
    }
  }
  return true;
}

std::shared_ptr<Sampler> Engine::add_sampler(const std::string &name) {
  if (!impl->ok)
    return nullptr;
  auto sampler = std::make_shared<Sampler>(sample_rate(), channels());
  sampler->set_listener(listener());
  Impl::Named added;
  added.instrument = sampler;
  if (!impl->add_named(*this, name, sampler, std::move(added)))
    return nullptr;
  return sampler;
}

std::shared_ptr<Sampler> Engine::sampler(const std::string &name) const {
  std::lock_guard<std::mutex> lock(impl->named_mutex);
  auto found = impl->named.find(name);
  return found == impl->named.end() ? nullptr : std::dynamic_pointer_cast<Sampler>(found->second.instrument);
}

bool Engine::trigger(const std::string &name, const std::string &event, float volume,
                     bool restart, float delay) {
  std::shared_ptr<Sampler> found = sampler(name);
  return found && found->trigger(event, volume, restart, delay);
}

bool Engine::trigger_at(const std::string &name, const std::string &event,
                        const Vec3 &position, float volume, bool restart, float delay) {
  std::shared_ptr<Sampler> found = sampler(name);
  return found && found->trigger_at(event, position, volume, restart, delay);
}

bool Engine::stop_event(const std::string &name, const std::string &event) {
  std::shared_ptr<Sampler> found = sampler(name);
  if (found)
    found->stop(event);
  return found != nullptr;
}

bool Engine::stop_events(const std::string &name) {
  std::shared_ptr<Sampler> found = sampler(name);
  if (found)
    found->stop_all();
  return found != nullptr;
}

bool Engine::play_note_at(const std::string &name, const Vec3 &position, float frequency,
                          float amplitude, float sustain, float delay) {
  std::shared_ptr<Synth> synth = std::dynamic_pointer_cast<Synth>(instrument(name));
  if (!synth)
    return false;
  const Listener heard_from = listener();
  const float gain = heard_from.gain(position);
  if (gain > 0.0f)
    synth->play_note(frequency, amplitude * gain, sustain, delay, heard_from.pan(position));
  return true;
}

void Engine::set_listener(const Listener &listener) {
  std::vector<std::shared_ptr<Sampler>> samplers;
  {
    std::lock_guard<std::mutex> lock(impl->named_mutex);
    impl->listener = listener;
    for (const auto &[name, added] : impl->named) {
      std::shared_ptr<Sampler> smplr = std::dynamic_pointer_cast<Sampler>(added.instrument);
      if (smplr != nullptr) {
        samplers.push_back(smplr);
      }
    }
  }
  for (const auto &sampler : samplers)
    sampler->set_listener(listener);
}

Listener Engine::listener() const {
  std::lock_guard<std::mutex> lock(impl->named_mutex);
  return impl->listener;
}

float Listener::gain(const Vec3 &at) const {
  const float dx = at.x - position.x, dy = at.y - position.y, dz = at.z - position.z;
  const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (distance <= min_distance)
    return 1.0f;
  if (distance >= max_distance)
    return 0.0f;
  // Squared, so it falls off quickly at first, then trails into silence.
  const float out = 1.0f - (distance - min_distance) / (max_distance - min_distance);
  return out * out;
}

float Listener::pan(const Vec3 &at) const {
  const float length = std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
  if (length <= 0.0f || max_distance <= 0.0f)
    return 0.0f;
  const float along = ((at.x - position.x) * right.x + (at.y - position.y) * right.y +
                       (at.z - position.z) * right.z) /
                      length;
  return std::clamp(along / max_distance, -1.0f, 1.0f);
}

std::uint32_t Engine::render(float *out, std::uint32_t frames) {
  if (!impl->ok || !impl->offline)
    return 0;
  // miniaudio reports only the frames its nodes produced (none when nothing
  // is playing) but always fills the rest of `out` with silence.
  if (ma_engine_read_pcm_frames(&impl->engine, out, frames, nullptr) != MA_SUCCESS)
    return 0;
  return frames;
}

} // namespace tokkel
