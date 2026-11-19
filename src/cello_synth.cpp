#include "tokkel/cello_synth.hpp"

#include <algorithm>
#include <cmath>

namespace tokkel {

namespace {

float sample_rate() { return static_cast<float>(stk::Stk::sampleRate()); }

// The most vibrato: how far the pitch swings, as a fraction of the string's
// length (0.004 is about 7 cents either way), and when it has fully set in.
constexpr float vibrato_depth = 0.004f;
constexpr float vibrato_delay = 0.3f, vibrato_rise = 0.5f; // seconds

// How long a string must stay silent, once the bow has lifted, before it's
// done: longer than a period of the lowest note, which can pass zero.
constexpr float done_after = 0.1f; // seconds

} // namespace

CelloSynth::CelloSynth() {
  for (Voice &voice : voices) {
    // Vibrato at about 5.5 Hz (controller 11 is its rate: 128 is 12 Hz).
    voice.string.controlChange(11, 128.0 * 5.5 / 12.0);
    voice.string.setVibrato(0.0);
  }
  // The cello's air and wood resonances, well below a violin's.
  air.setResonance(98.0, 0.995, true);
  wood.setResonance(210.0, 0.99, true);
  tone.setLowPass(1500.0, 0.7);
}

void CelloSynth::note_on(float frequency, float amplitude, float sustain) {
  if (frequency < 30.0f || frequency > 2000.0f)
    return;
  Voice &voice = voices[next_voice];
  next_voice = (next_voice + 1) % voices.size();

  const float velocity = std::clamp(amplitude, 0.0f, 1.0f);
  voice.string.setFrequency(frequency);
  voice.string.setVibrato(0.0);
  // The bow: from 150 ms to set in for a soft note down to 80 ms for a hard
  // one (the rate is per sample).
  const float attack = 0.15f - 0.07f * velocity;
  voice.string.startBowing(velocity, 1.0 / (attack * sample_rate()));
  voice.age = 0;
  voice.quiet = 0;
  voice.hold = static_cast<int>(std::max(sustain, 0.05f) * sample_rate());
}

void CelloSynth::tick(float &left, float &right) {
  float strings = 0.0f;
  for (Voice &voice : voices) {
    if (voice.age < 0)
      continue;
    // Vibrato sets in after a moment, over half a second. Changed every 64
    // samples: smooth enough, and Bowed retunes its delay line on each.
    if (voice.age % 64 == 0) {
      const float t = static_cast<float>(voice.age) / sample_rate() - vibrato_delay;
      voice.string.setVibrato(vibrato_depth * std::clamp(t / vibrato_rise, 0.0f, 1.0f));
    }
    ++voice.age;
    if (voice.hold >= 0 && voice.hold-- == 0)
      voice.string.stopBowing(1.0 / (0.25 * sample_rate())); // the bow lifts: 0.25 s
    const float out = static_cast<float>(voice.string.tick());
    strings += out;
    if (voice.hold >= 0)
      continue;
    // The bow is off and the string has gone quiet: done, until it's played
    // again (cleared, so it starts clean).
    if (std::fabs(out) >= silent)
      voice.quiet = 0;
    else if (++voice.quiet > static_cast<int>(done_after * sample_rate())) {
      voice.age = -1;
      voice.string.clear();
    }
  }

  float out = strings;
  out += 0.6f * static_cast<float>(air.tick(out)) + 0.4f * static_cast<float>(wood.tick(out));
  out = std::tanh(static_cast<float>(tone.tick(out)));
  left = out;
  right = out;
}

} // namespace tokkel
