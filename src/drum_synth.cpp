#include "tokkel/drum_synth.hpp"

#include <algorithm>
#include <cmath>

namespace tokkel {

namespace {

constexpr float two_pi = 6.28318531f;

float sample_rate() { return static_cast<float>(stk::Stk::sampleRate()); }

// The MIDI note number of `frequency` (A4, 440 Hz, is 69).
int midi_note(float frequency) {
  return static_cast<int>(std::lround(69.0f + 12.0f * std::log2(frequency / 440.0f)));
}

// How long each drum sounds, after which its hit is idle again.
constexpr float kick_length = 1.2f, snare_length = 1.0f; // seconds
constexpr float closed_hat_length = 0.2f, open_hat_length = 1.2f;
// How fast the hi-hat fades: the time it takes to drop to about a third.
constexpr float closed_hat_decay = 0.025f, open_hat_decay = 0.2f;

// The 808's hi-hat oscillators, in Hz: close enough together to beat
// against each other, far enough apart not to sound like a chord.
constexpr float hat_frequencies[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
// The hi-hat's share of the mix, set so it sits under the snare.
constexpr float hat_mix = 0.5f;

} // namespace

DrumSynth::DrumSynth() {
  for (int side = 0; side < 2; ++side) {
    wires_high[side].setHighPass(1500.0);
    wires_low[side].setLowPass(9000.0);
    hat_high[side].setHighPass(7000.0);
  }
}

void DrumSynth::note_on(float frequency, float amplitude, float) {
  if (frequency <= 0.0f)
    return;
  const float level = std::clamp(amplitude, 0.0f, 1.0f) * random_f(0.9f, 1.0f);
  Hit *hit = nullptr;
  switch (midi_note(frequency)) {
  case 35:
  case 36:
    hit = &kicks[next_kick];
    next_kick = (next_kick + 1) % kicks.size();
    break;
  case 37:
  case 38:
  case 40:
    hit = &snares[next_snare];
    next_snare = (next_snare + 1) % snares.size();
    break;
  case 42:
  case 44:
  case 46:
    hat_open = midi_note(frequency) == 46;
    hat = Hit{0, level * (midi_note(frequency) == 44 ? 0.7f : 1.0f), 0.0f, 0.0f};
    return;
  default:
    return; // not a drum of this kit
  }
  *hit = Hit{0, level, 0.0f, 0.0f};
}

float DrumSynth::kick(Hit &hit) {
  const float t = static_cast<float>(hit.age) / sample_rate();
  // The pitch drops fast from about 150 Hz to the drum's own 48 Hz: that
  // drop is most of what makes it sound like a kick.
  const float frequency = 48.0f + 100.0f * std::exp(-t / 0.035f);
  hit.phase += two_pi * frequency / sample_rate();
  if (hit.phase > two_pi)
    hit.phase -= two_pi;
  float out = std::sin(hit.phase) * std::exp(-t / 0.2f);
  // The beater: a few ms of noise.
  if (t < 0.004f)
    out += 0.4f * random_f(-1.0f, 1.0f) * (1.0f - t / 0.004f);
  return hit.level * out;
}

float DrumSynth::snare_tone(Hit &hit) {
  const float t = static_cast<float>(hit.age) / sample_rate();
  // The head's two lowest modes, dropping a little in pitch as they fade.
  const float drop = 1.0f + 0.08f * std::exp(-t / 0.02f);
  hit.phase += two_pi * 180.0f * drop / sample_rate();
  hit.second_phase += two_pi * 210.0f * drop / sample_rate();
  if (hit.phase > two_pi)
    hit.phase -= two_pi;
  if (hit.second_phase > two_pi)
    hit.second_phase -= two_pi;
  const float tone = 0.7f * std::sin(hit.phase) + 0.35f * std::sin(hit.second_phase);
  return hit.level * tone * std::exp(-t / 0.06f);
}

float DrumSynth::hat_metal() {
  float metal = 0.0f;
  for (std::size_t i = 0; i < hat_phases.size(); ++i) {
    hat_phases[i] += hat_frequencies[i] / sample_rate();
    if (hat_phases[i] >= 1.0f)
      hat_phases[i] -= 1.0f;
    metal += hat_phases[i] < 0.5f ? 1.0f : -1.0f;
  }
  return metal / static_cast<float>(hat_phases.size());
}

void DrumSynth::tick(float &left, float &right) {
  float kick_click = 0.0f;
  for (Hit &hit : kicks) {
    if (hit.age < 0)
      continue;
    kick_click += kick(hit);
    if (++hit.age > static_cast<int>(kick_length * sample_rate()))
      hit.age = -1;
  }
  // Round the beater's noise off a little, so it's a thump more than a tick.
  click += 0.5f * (kick_click - click);

  float tone = 0.0f;
  float wires = 0.0f; // how loud the snare wires are right now
  for (Hit &hit : snares) {
    if (hit.age < 0)
      continue;
    tone += snare_tone(hit);
    const float t = static_cast<float>(hit.age) / sample_rate();
    // Harder hits make the wires a bigger part of the sound.
    wires += hit.level * (0.6f + 0.4f * hit.level) * std::exp(-t / 0.03f);
    if (++hit.age > static_cast<int>(snare_length * sample_rate()))
      hit.age = -1;
  }
  float noise[2] = {0.0f, 0.0f};
  for (int side = 0; side < 2; ++side) {
    // Filtered always, also when silent, so the filters never jump.
    const float white = wires > 0.0f ? wires * random_f(-1.0f, 1.0f) : 0.0f;
    noise[side] = static_cast<float>(wires_low[side].tick(wires_high[side].tick(white)));
  }

  // The hi-hat: its metal with a little noise of its own per side.
  const float metal = hat_metal();
  float hat_level = 0.0f;
  if (hat.age >= 0) {
    const float t = static_cast<float>(hat.age) / sample_rate();
    hat_level = hat.level * std::exp(-t / (hat_open ? open_hat_decay : closed_hat_decay));
    if (++hat.age > static_cast<int>((hat_open ? open_hat_length : closed_hat_length) * sample_rate()))
      hat.age = -1;
  }
  float sizzle[2] = {0.0f, 0.0f};
  for (int side = 0; side < 2; ++side) {
    const float raw = hat_level > 0.0f
                          ? hat_level * (0.7f * metal + 0.3f * random_f(-1.0f, 1.0f))
                          : 0.0f;
    sizzle[side] = static_cast<float>(hat_high[side].tick(raw));
  }

  // Levels set so a hit is about as loud (RMS over its first 100 ms) as the
  // kick and snare samples were; tanh rounds off two hits landing together.
  const float centre = 0.45f * click + 0.25f * tone;
  left = std::tanh(centre + 0.3f * noise[0] + hat_mix * sizzle[0]);
  right = std::tanh(centre + 0.3f * noise[1] + hat_mix * sizzle[1]);
}

} // namespace tokkel
