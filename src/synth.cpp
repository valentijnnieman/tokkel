#include "tokkel/synth.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace tokkel {

float note_frequency(const std::string &name) {
  static const int semitones[] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
  if (name.size() < 2 || name[0] < 'A' || name[0] > 'G')
    return 0.0f;

  int semitone = semitones[name[0] - 'A'];
  std::size_t index = 1;
  if (name[index] == '#') {
    ++semitone;
    ++index;
  } else if (name[index] == 'b') {
    --semitone;
    ++index;
  }

  // Exactly one octave digit must be left.
  if (index + 1 != name.size() || name[index] < '0' || name[index] > '9')
    return 0.0f;
  const int octave = name[index] - '0';

  const int midi = 12 * (octave + 1) + semitone; // A4 = 69
  return 440.0f * std::pow(2.0f, static_cast<float>(midi - 69) / 12.0f);
}

// Room for the waiting and scheduled notes up front, so neither play_note()
// nor the audio thread ever allocates.
Synth::Synth() {
  scheduled.reserve(256);
  rev.setEffectMix(1.0); // only the reverb: render() mixes it with the dry sound
}

void Synth::play_note(float frequency, float amplitude, float sustain, float delay, float pan) {
  const auto delay_frames =
      static_cast<std::uint64_t>(std::max(delay, 0.0f) * stk::Stk::sampleRate());
  std::lock_guard<std::mutex> lock(mutex);
  waiting.push_to_ring(Note{frequency, amplitude, sustain, delay_frames, std::clamp(pan, -1.0f, 1.0f)});
}

void Synth::start(const Note &note) {
  starting_pan = note.pan;
  note_on(note.frequency, note.amplitude, note.sustain);
  starting_pan = 0.0f;
}

void Synth::start_due_notes() {
  next_start = UINT64_MAX;
  for (std::size_t i = 0; i < scheduled.size();) {
    if (scheduled[i].start <= now) {
      start(scheduled[i].note);
      scheduled[i] = scheduled.back(); // order doesn't matter: swap and pop
      scheduled.pop_back();
    } else {
      next_start = std::min(next_start, scheduled[i].start);
      ++i;
    }
  }
}

void Synth::render(float *out, std::uint32_t frames, std::uint32_t channels) {
  Note *notes;
  std::size_t count;
  while((count = waiting.peek_ring(&notes)) > 0) {
    for(std::size_t i = 0; i < count; i++) {
      const Note &note = notes[i];
      if (note.delay == 0) {
        start(note);
      } else if (scheduled.size() < scheduled.capacity()) { // else dropped
        scheduled.push_back(Scheduled{note, now + note.delay});
        next_start = std::min(next_start, now + note.delay);
      }
    }
    waiting.advance_ring(count);
  }

  // A changed reverb takes effect from this block on.
  const float wanted = reverb.load(std::memory_order_relaxed);
  if (wanted != current_reverb) {
    if (wanted <= 0.0f)
      rev.clear(); // off: no old tail when it comes back
    else
      rev.setT60(std::max(wanted / 20.0f, 0.05f));
    current_reverb = wanted;
  }

  // Volume and pan glide to their new values over about 20 ms.
  const float wanted_volume = std::max(volume.load(std::memory_order_relaxed), 0.0f);
  const float wanted_pan = std::clamp(pan.load(std::memory_order_relaxed), -1.0f, 1.0f);
  const float glide = 1.0f / (0.02f * static_cast<float>(stk::Stk::sampleRate()));

  for (std::uint32_t frame = 0; frame < frames; ++frame, ++now) {
    if (now >= next_start)
      start_due_notes();
    float left = 0.0f, right = 0.0f;
    tick(left, right);
    if (current_reverb > 0.0f) {
      rev.tick(0.5f * (left + right)); // mono in, stereo out
      // tanh: a loud chord plus its reverb mustn't go past 1.
      left = std::tanh(0.7f * left + 0.3f * static_cast<float>(rev.lastOut(0)));
      right = std::tanh(0.7f * right + 0.3f * static_cast<float>(rev.lastOut(1)));
    }
    current_volume += glide * (wanted_volume - current_volume);
    current_pan += glide * (wanted_pan - current_pan);
    left *= current_volume * std::min(1.0f, 1.0f - current_pan);
    right *= current_volume * std::min(1.0f, 1.0f + current_pan);
    if (channels == 1) {
      *out++ = 0.5f * (left + right);
      continue;
    }
    *out++ = left;
    *out++ = right;
    for (std::uint32_t channel = 2; channel < channels; ++channel)
      *out++ = 0.0f; // anything past stereo stays silent
  }
}

void Synth::set_parameter(const std::string &name, float value) {
  if (name == "Reverb")
    reverb.store(value, std::memory_order_relaxed);
  else if (name == "Volume")
    volume.store(std::max(value, 0.0f), std::memory_order_relaxed);
  else if (name == "Pan")
    pan.store(std::clamp(value, -1.0f, 1.0f), std::memory_order_relaxed);
  else if (name == "Tokkel")
    tokkel.store(value, std::memory_order_relaxed);
  else if (name == "Tokkel_amount")
    tokkel_amt.store(value, std::memory_order_relaxed);
}

float Synth::random_f(float min, float max) {
  return std::uniform_real_distribution<float>(min, max)(random);
}

StringSynth::StringSynth() { 
  reverb = 20.0f; // a second, as it had before the reverb moved to Synth

  chorus.setModDepth(0.02); 
  chorus.setModFrequency(0.3); 
  chorus.setEffectMix(0.3); 

  lp_filter.setLowPass(2400.0, 0.7);
  body_air.setResonance(100.0, 0.995, true);
  body_top.setResonance(200.0, 0.99, true);

  for(auto& v : voices) {
    v.main.setPickupPosition(0.4);
    v.detuned.setPickupPosition(0.6);
    v.low.setPickupPosition(0.8);
  }
}

void StringSynth::note_on(float frequency, float amplitude, float sustain) {
  if (frequency < 20.0f)
    return;
  auto &v = voices[next_voice];

  const float ms = static_cast<float>(stk::Stk::sampleRate()) / 1000.0f;
  const float jitter = random_f(0.0f, 4.0f);
  v.start_in = static_cast<int>((strum_pos * 32.0f + jitter) * ms); // 32 ms per string 
  ++strum_pos;  

  v.frequency = frequency;
  v.amplitude = std::clamp(amplitude, 0.0f, 1.0f) * random_f(0.8f, 1.0f);
  v.sustain = sustain;

  next_voice = (next_voice + 1) % voices.size();
}

void StringSynth::tick(float &left, float &right) {
  strum_pos = 0;
  float sample = 0.0f;
  for (auto &v : voices) {

    if (v.start_in >= 0 && v.start_in-- == 0) {
      v.envelope.setAllTimes(0.005, v.sustain, 0.0, 0.1);
      v.envelope.keyOn();

      v.body.setResonance(v.frequency * 2.0f, 0.99, true);

      v.main.noteOn(v.frequency, v.amplitude);
      v.detuned.noteOn(v.frequency * 1.004f, v.amplitude * 0.5f);
      v.low.noteOn(v.frequency * 0.5, v.amplitude * 0.4f);
    }

    const double out = v.main.tick() + v.detuned.tick() + v.low.tick();
    const double voiced = out + 0.5 * v.body.tick(out);
    sample += static_cast<float>(v.envelope.tick() * voiced);
  }
  sample += static_cast<float>(0.5 * body_air.tick(sample) + 0.4 * body_top.tick(sample));
  // sample = chorus.tick(sample);
  sample = lp_filter.tick(sample);
  sample *= 0.5f; // headroom for chords

  // tanh for some soft limiting; the reverb comes after, in Synth::render()
  left = std::tanh(sample);
  right = left;
}

} // namespace tokkel
