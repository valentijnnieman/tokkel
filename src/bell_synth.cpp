#include "tokkel/bell_synth.hpp"

#include <Stk.h>

#include <algorithm>
#include <cmath>

namespace tokkel {

void BellSynth::note_on(float frequency, float amplitude, float sustain) {
  const double rate = stk::Stk::sampleRate();
  if (frequency < 20.0f || frequency > rate / 2.0)
    return;
  Voice &voice = voices[next_voice];
  next_voice = (next_voice + 1) % voices.size();

  // Falls 60 dB over `seconds`.
  const double seconds = std::max(sustain, 0.05f) * 1.5;
  voice.phase = 0.0;
  voice.step = frequency / rate;
  voice.level = std::clamp(amplitude, 0.0f, 1.0f);
  voice.decay = std::pow(0.001, 1.0 / (seconds * rate));
  pan_gains(note_pan(), voice.left, voice.right);
}

void BellSynth::tick(float &left, float &right) {
  double mix_left = 0.0, mix_right = 0.0;
  for (Voice &voice : voices) {
    if (voice.level < 1e-5)
      continue;
    const double wave = voice.phase < 0.5 ? voice.level : -voice.level;
    mix_left += wave * voice.left;
    mix_right += wave * voice.right;
    voice.phase += voice.step;
    if (voice.phase >= 1.0)
      voice.phase -= 1.0;
    voice.level *= voice.decay;
  }
  // A square is loud for its peak: 0.25 puts a note near the other synths'
  // level, and tanh rounds off a full chord instead of clipping it.
  left = std::tanh(static_cast<float>(mix_left) * 0.25f);
  right = std::tanh(static_cast<float>(mix_right) * 0.25f);
}

} // namespace tokkel
