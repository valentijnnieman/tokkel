#include "tokkel/piano_synth.hpp"

#include <algorithm>
#include <cmath>

namespace tokkel {

namespace {

// The loop gain that makes a StifKarp string at `frequency` fall 60 dB in
// `seconds`. StifKarp adds frequency * 0.000005 to the base gain itself.
double base_loop_gain(float frequency, float seconds) {
  const double gain = std::pow(0.001, 1.0 / (static_cast<double>(frequency) * seconds));
  return std::min(gain, 0.99999) - frequency * 0.000005;
}

// StifKarp's loop is about one sample longer than the note asks for, which
// plays it 13 to 22 cents flat (more the higher the note). The frequency to
// ask for so that `frequency` is what sounds.
float in_tune(float frequency) {
  const float extra_delay = 1.1f; // samples, measured
  return frequency /
         (1.0f - extra_delay * frequency / static_cast<float>(stk::Stk::sampleRate()));
}

} // namespace

PianoSynth::PianoSynth() {
  for (Voice &voice : voices) {
    // The stiffness (setStretch) stays at STK's default: lowering it moves
    // the note itself, by more than half a semitone at 0.97.
    for (stk::StifKarp *string : {&voice.first, &voice.second})
      string->setPickupPosition(0.25); // keeps the low overtones
    voice.hammer.setLowPass(3000.0, 0.7);
    // Silent until the first note: an ADSR starts idle at 0.
  }
}

void PianoSynth::note_on(float frequency, float amplitude, float sustain) {
  if (frequency < 20.0f)
    return;
  Voice &voice = voices[next_voice];
  next_voice = (next_voice + 1) % voices.size();

  const float level = std::clamp(amplitude, 0.0f, 1.0f);
  // Higher strings ring shorter, as on a real piano.
  const float seconds =
      std::max(sustain, 0.05f) * 3.0f * std::sqrt(220.0f / std::max(frequency, 220.0f));
  const float detuned = frequency * 1.0015f; // about 2.5 cents

  voice.first.setBaseLoopGain(base_loop_gain(frequency, seconds));
  voice.second.setBaseLoopGain(base_loop_gain(detuned, seconds));
  voice.first.noteOn(in_tune(frequency), level);
  voice.second.noteOn(in_tune(detuned), level);

  // The higher the note, the more of its overtones the hammer lets through.
  voice.hammer.setLowPass(std::clamp(frequency * 6.0f, 1500.0f, 6000.0f), 0.7);

  // The strings decay by themselves; the envelope only softens the start and
  // makes sure the voice ends.
  voice.envelope.setAllTimes(0.004, seconds * 2.0f, 0.0, 0.1);
  voice.envelope.keyOn();
}

void PianoSynth::tick(float &left, float &right) {
  double mix = 0.0;
  for (Voice &voice : voices) {
    const double strings = voice.first.tick() + voice.second.tick();
    mix += voice.envelope.tick() * voice.hammer.tick(strings);
  }
  // The gain puts a single note at about the level of a StringSynth note;
  // tanh then rounds off the peaks of a full chord instead of clipping them.
  const float sample = std::tanh(static_cast<float>(mix) * 1.5f);
  left = sample;
  right = sample;
}

} // namespace tokkel
