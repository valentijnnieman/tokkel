#include "tokkel/guitar_synth.hpp"

#include <algorithm>
#include <cmath>

namespace tokkel {

namespace {

constexpr float lowest_note = 30.0f; // Hz; lower notes aren't played

// The loop gain that makes a Twang string at `frequency` fall 60 dB in
// `seconds` (Twang adds frequency * 0.000005 to it itself).
double loop_gain(float frequency, float seconds) {
  const double gain = std::pow(0.001, 1.0 / (static_cast<double>(frequency) * seconds));
  return std::clamp(gain - frequency * 0.000005, 0.0, 0.99998);
}

// How long an undamped string rings: low strings for seconds, high ones
// much shorter.
float ring_time(float frequency) {
  return 8.0f * std::sqrt(82.0f / std::max(frequency, 82.0f));
}

float sample_rate() { return static_cast<float>(stk::Stk::sampleRate()); }

// How long a damped string must stay silent before it's done: longer than a
// period of the lowest note, which can pass zero.
constexpr float done_after = 0.1f; // seconds

// Twang's loop feeds back the delay line's previous output, a sample more
// than setFrequency() counts, which plays it flat (31 cents at A5). The
// frequency to ask for so that `frequency` is what sounds.
float in_tune(float frequency) { return frequency / (1.0f - frequency / sample_rate()); }

} // namespace

GuitarSynth::GuitarSynth() {
  for (Voice &voice : voices) {
    for (stk::Twang *string : {&voice.vertical, &voice.horizontal}) {
      string->setLowestFrequency(lowest_note);
      // The loop's low-pass: a two-point average, the classic plucked string,
      // which lets each overtone die faster the higher it is.
      string->setLoopFilter({0.5, 0.5});
    }
  }

  // The body: Helmholtz air resonance, the top plate's main modes, and a bit
  // of upper-mid "wood". Frequency, radius (closer to 1 = narrower), gain.
  // The right side's are slightly off and with less air, as a second
  // microphone further from the soundhole would hear it.
  const float left[5][3] = {
      {98.0f, 0.997f, 0.9f}, {204.0f, 0.996f, 0.7f}, {390.0f, 0.993f, 0.35f},
      {560.0f, 0.99f, 0.25f}, {1050.0f, 0.985f, 0.15f}};
  const float right[5][3] = {
      {101.0f, 0.997f, 0.55f}, {214.0f, 0.996f, 0.6f}, {372.0f, 0.993f, 0.35f},
      {610.0f, 0.99f, 0.3f}, {1180.0f, 0.985f, 0.2f}};
  for (std::size_t i = 0; i < 5; ++i) {
    left_body.resonances[i].setResonance(left[i][0], left[i][1], true);
    left_body.gains[i] = left[i][2];
    right_body.resonances[i].setResonance(right[i][0], right[i][1], true);
    right_body.gains[i] = right[i][2];
  }

  // low pass to make it sound more nylon
  lp_filter.setLowPass(4000.0, 0.5);

  reverb = 20.0f; // a second, as it had before the reverb moved to Synth
}

float GuitarSynth::Body::tick(float in) {
  float out = in;
  for (std::size_t i = 0; i < resonances.size(); ++i)
    out += gains[i] * static_cast<float>(resonances[i].tick(in));
  return out;
}

void GuitarSynth::note_on(float frequency, float amplitude, float sustain) {
  if (frequency < lowest_note || frequency > sample_rate() / 4.0f)
    return;
  Voice &voice = voices[next_voice];
  next_voice = (next_voice + 1) % voices.size();

  const float velocity = std::clamp(amplitude, 0.0f, 1.0f) * random_f(0.5f, 1.0f);
  voice.frequency = frequency;
  voice.level = velocity;
  voice.sounding = true;
  voice.quiet = 0;

  // Where it is plucked, varying a little as a hand does: nearer the bridge
  // (lower) is twangier. Set before the frequency, which applies it.
  const float position = random_f(0.22f, 0.32f);
  const float seconds = ring_time(frequency);
  voice.vertical.setPluckPosition(position);
  voice.horizontal.setPluckPosition(position);
  voice.vertical.setFrequency(in_tune(frequency));
  voice.horizontal.setFrequency(in_tune(frequency * 1.0004f)); // under a cent apart
  // The vertical motion pushes on the bridge and dies quickly; the
  // horizontal one rings on.
  voice.vertical.setLoopGain(loop_gain(frequency, seconds * 0.35f));
  voice.horizontal.setLoopGain(loop_gain(frequency, seconds));

  // The pluck: a period of noise, low-passed by how hard it is played.
  voice.pick.setPole(0.92 - 0.55 * velocity);
  voice.excite_length = std::max(2, static_cast<int>(sample_rate() / frequency));

  // Strummed: each note started at the same moment comes a bit after the
  // one before.
  const float ms = sample_rate() / 1000.0f;
  voice.start_in = static_cast<int>((static_cast<float>(strum) * 12.0f + random_f(0.0f, 3.0f)) * ms);
  ++strum;

  voice.hold = static_cast<int>(std::max(sustain, 0.05f) * sample_rate()) + voice.start_in;
}

void GuitarSynth::damp(Voice &voice) {
  // A finger on the string: what's left dies away in a quarter second.
  voice.vertical.setLoopGain(loop_gain(voice.frequency, 0.25f));
  voice.horizontal.setLoopGain(loop_gain(voice.frequency, 0.25f));
}

bool GuitarSynth::holding_notes() const {
  for (const Voice &voice : voices)
    if (voice.start_in >= 0)
      return true;
  return false;
}

void GuitarSynth::tick(float &left, float &right) {
  strum = 0;
  float strings = 0.0f;
  for (Voice &voice : voices) {
    if (!voice.sounding)
      continue;
    if (voice.start_in >= 0 && voice.start_in-- == 0) {
      voice.excite_left = voice.excite_length;
      pick_noise = std::max(pick_noise, voice.level);
    }

    float in = 0.0f;
    if (voice.excite_left > 0) {
      // A smooth window over the burst, so the pluck has no hard edges.
      const float phase = 1.0f - static_cast<float>(voice.excite_left) /
                                     static_cast<float>(voice.excite_length);
      const float window = std::sin(3.14159265f * phase);
      in = voice.level * static_cast<float>(voice.pick.tick(random_f(-1.0f, 1.0f) * window));
      --voice.excite_left;
    }

    if (voice.hold >= 0 && voice.hold-- == 0)
      damp(voice);

    const float out = static_cast<float>(voice.vertical.tick(in) + 0.6 * voice.horizontal.tick(in));
    strings += out;

    // Plucked, damped and gone quiet: done, until it's played again (cleared,
    // so it starts clean).
    if (voice.start_in >= 0 || voice.excite_left > 0 || voice.hold >= 0)
      continue;
    if (std::fabs(out) >= silent)
      voice.quiet = 0;
    else if (++voice.quiet > static_cast<int>(done_after * sample_rate())) {
      voice.sounding = false;
      voice.vertical.clear();
      voice.horizontal.clear();
      voice.pick.clear();
    }
  }

  // The pick against the string: a few ms of quiet, bright noise.
  float noise = 0.0f;
  if (pick_noise > 0.0001f) {
    const float white = random_f(-1.0f, 1.0f);
    noise = 0.04f * pick_noise * (white - last_noise); // high-passed
    last_noise = white;
    pick_noise *= 0.996f; // gone in about 5 ms
  }

  // The gain puts a single note at about the level of the other synths;
  // tanh rounds off the peaks of a full chord instead of clipping them.
  // 0.55 before the reverb moved to Synth, which keeps 70% of the dry sound.
  const float gain = 0.7f;
  float body_left = gain * left_body.tick(strings);
  float body_right = gain * right_body.tick(strings);                                  

  body_left = lp_filter.tick(body_left);
  body_right = lp_filter.tick(body_right);

  // The reverb comes after, in Synth::render().
  left = std::tanh(body_left + noise);
  right = std::tanh(body_right + noise);
}

} // namespace tokkel
