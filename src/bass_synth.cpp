#include "tokkel/bass_synth.hpp"

#include <algorithm>
#include <cmath>

namespace tokkel {

namespace {

float sample_rate() { return static_cast<float>(stk::Stk::sampleRate()); }

// Smooths the jump of a square or saw wave over a sample either side of it
// (PolyBLEP), so its overtones don't fold back above half the sample rate
// as off-key noise. `t` is the phase (0 to 1), `step` the phase per sample.
float blep(float t, float step) {
  if (t < step) {
    t /= step;
    return t + t - t * t - 1.0f;
  }
  if (t > 1.0f - step) {
    t = (t - 1.0f) / step;
    return t * t + t + t + 1.0f;
  }
  return 0.0f;
}

} // namespace

BassSynth::BassSynth() {
  envelope.setAllTimes(0.003, 0.9, 0.0, 0.0);
}

void BassSynth::note_on(float frequency_, float amplitude, float sustain) {
  if (frequency_ < 20.0f || frequency_ > sample_rate() / 4.0f)
    return;
  // Of notes started together, the lowest is the one the bass plays.
  if (started_this_tick && frequency_ >= frequency)
    return;
  started_this_tick = true;

  frequency = frequency_;
  level = std::clamp(amplitude, 0.0f, 1.0f);
  hold = static_cast<int>(std::max(sustain, 0.05f) * sample_rate());
  age = 0;
  retune = 0;
  // From wherever the envelope is, so taking over from a note doesn't click.
  envelope.keyOn();
}

void BassSynth::set_parameter(const std::string &name, float value) {
  if (name == "Waveform")
    waveform.store(std::clamp(static_cast<int>(value), static_cast<int>(Sine),
                              static_cast<int>(Saw)));
  else
    Synth::set_parameter(name, value);
}

float BassSynth::oscillator() {
  const float step = frequency / sample_rate();
  phase += step;
  if (phase >= 1.0f)
    phase -= 1.0f;

  switch (waveform.load(std::memory_order_relaxed)) {
  case Sine:
    return std::sin(6.28318531f * phase);
  case Triangle:
    // Naive is fine: a triangle's overtones fall off fast.
    return 4.0f * std::fabs(phase - 0.5f) - 1.0f;
  case Saw:
    return 2.0f * phase - 1.0f - blep(phase, step);
  case Square:
  default: {
    float square = phase < 0.5f ? 1.0f : -1.0f;
    square += blep(phase, step);
    float half = phase + 0.5f;
    if (half >= 1.0f)
      half -= 1.0f;
    square -= blep(half, step);
    return square;
  }
  }
}

void BassSynth::tick(float &left, float &right) {
  started_this_tick = false;
  if (frequency <= 0.0f) {
    left = right = 0.0f; // nothing played yet
    return;
  }
  if (hold >= 0 && hold-- == 0)
    envelope.keyOff();

  // The filter opens with the note and closes over about 0.15 s, to a few
  // overtones above the note; harder notes open it further. Worked out every
  // 32 samples: often enough to sound smooth, cheap enough not to matter.
  if (retune-- <= 0) {
    retune = 32;
    const float t = static_cast<float>(age) / sample_rate();
    const float base = std::min(frequency * 2.0f + 50.0f, 2000.0f);
    const float sweep = (600.0f + 200.0f * level) * std::exp(-t / 0.15f);
    filter.setLowPass(std::min(base + sweep, 0.4f * sample_rate()), 2.0);
  }
  ++age;

  const float shaped = static_cast<float>(filter.tick(oscillator()));
  // Gain set so a note sits at about the level of the other synths.
  const float out = std::tanh(0.5f * level * static_cast<float>(envelope.tick()) * shaped);
  left = out;
  right = out;
}

} // namespace tokkel
