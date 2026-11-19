// Renders an STK sine through an offline engine (no audio device) and checks
// that the mix carries it at the expected level and pitch, then goes silent
// once the voice is stopped. Then plays notes on the engine's string synth.
// Last, that a synth gone silent rests (isn't computed) and wakes for a note.
#include "tokkel/bass_synth.hpp"
#include "tokkel/bell_synth.hpp"
#include "tokkel/cello_synth.hpp"
#include "tokkel/drum_synth.hpp"
#include "tokkel/guitar_synth.hpp"
#include "tokkel/piano_synth.hpp"
#include "tokkel/sampler.hpp"
#include "tokkel/stk_instrument.hpp"
#include "tokkel/synth.hpp"
#include "tokkel/tokkel.hpp"

#include <SineWave.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int failures = 0;

void check(bool passed, const char *what) {
  std::printf("%s: %s\n", passed ? "ok  " : "FAIL", what);
  if (!passed)
    ++failures;
}

float peak(const std::vector<float> &samples) {
  float highest = 0.0f;
  for (float sample : samples)
    highest = std::fmax(highest, std::fabs(sample));
  return highest;
}

// The pitch in `mix` (stereo, 48 kHz; its left channel, 0.05 to 0.5 s) near
// `expected` Hz: the lag within 5% of the expected period at which the signal
// best matches itself, refined between samples.
double pitch_of(const std::vector<float> &mix, float expected) {
  const int from = static_cast<int>(48000.0f / expected * 0.95f);
  const int to = static_cast<int>(48000.0f / expected * 1.05f) + 1;
  std::vector<double> match(to + 2, 0.0);
  for (int lag = from - 1; lag <= to + 1; ++lag)
    for (std::uint32_t frame = 2400; frame + lag < 24000; ++frame)
      match[lag] += mix[frame * 2] * mix[(frame + lag) * 2];
  int best = from;
  for (int lag = from; lag <= to; ++lag)
    if (match[lag] > match[best])
      best = lag;
  const double a = match[best - 1], b = match[best], c = match[best + 1];
  const double shift = 0.5 * (a - c) / (a - 2.0 * b + c);
  return 48000.0 / (best + shift);
}

double cents(double heard, double wanted) { return 1200.0 * std::log2(heard / wanted); }

// A 16-bit mono wav file in memory: `frames` frames of a constant `level`.
std::vector<unsigned char> constant_wav(std::uint32_t sample_rate, std::uint32_t frames,
                                        std::int16_t level) {
  std::vector<unsigned char> wav(44 + frames * 2);
  auto put32 = [&](std::size_t at, std::uint32_t value) { std::memcpy(&wav[at], &value, 4); };
  auto put16 = [&](std::size_t at, std::uint16_t value) { std::memcpy(&wav[at], &value, 2); };
  std::memcpy(&wav[0], "RIFF", 4);
  put32(4, 36 + frames * 2);
  std::memcpy(&wav[8], "WAVEfmt ", 8);
  put32(16, 16);              // size of the format chunk
  put16(20, 1);               // PCM
  put16(22, 1);               // mono
  put32(24, sample_rate);
  put32(28, sample_rate * 2); // bytes per second
  put16(32, 2);               // bytes per frame
  put16(34, 16);              // bits per sample
  std::memcpy(&wav[36], "data", 4);
  put32(40, frames * 2);
  for (std::uint32_t frame = 0; frame < frames; ++frame)
    std::memcpy(&wav[44 + frame * 2], &level, 2);
  return wav;
}

// A synth that counts its ticks, to see when Synth::render() stops computing
// it: a note is a level that fades by 1% a frame (silent after about 1150).
class CountingSynth : public tokkel::Synth {
public:
  long ticks = 0;
  bool hold = false; // say it's holding a note back

protected:
  void note_on(float, float amplitude, float) override { level = amplitude; }
  void tick(float &left, float &right) override {
    ++ticks;
    left = right = level;
    level *= 0.99f;
  }
  bool holding_notes() const override { return hold; }

private:
  float level = 0.0f;
};

// Renders `seconds` of `synth` on its own, in blocks as an audio device would.
std::vector<float> render_alone(tokkel::Synth &synth, float seconds) {
  const std::uint32_t block = 480;
  std::vector<float> out(static_cast<std::size_t>(seconds * 48000.0f) * 2);
  for (std::size_t frame = 0; frame < out.size() / 2; frame += block)
    synth.render(out.data() + frame * 2,
                 static_cast<std::uint32_t>(std::min<std::size_t>(block, out.size() / 2 - frame)), 2);
  return out;
}

} // namespace

int main() {
  tokkel::EngineConfig config;
  config.offline = true;
  tokkel::Engine engine(config);
  check(engine.ok(), "offline engine starts");
  check(engine.sample_rate() == 48000 && engine.channels() == 2, "engine uses the requested format");
  check(stk::Stk::sampleRate() == 48000.0, "STK follows the engine's sample rate");

  const std::uint32_t frames = 48000;
  std::vector<float> mix(frames * engine.channels());

  check(engine.render(mix.data(), frames) == frames, "render returns the frames asked for");
  check(peak(mix) == 0.0f, "an empty engine is silent");

  auto sine = std::make_shared<tokkel::StkInstrument<stk::SineWave>>();
  sine->unit.setFrequency(440.0);
  const tokkel::VoiceId voice = engine.play(sine, 0.5f);
  check(voice != 0, "play returns a voice");

  engine.render(mix.data(), frames);
  const float level = peak(mix);
  check(level > 0.45f && level < 0.55f, "the sine comes through at its volume");

  // One second of a 440 Hz sine crosses zero upwards 440 times.
  int cycles = 0;
  for (std::uint32_t frame = 1; frame < frames; ++frame)
    if (mix[(frame - 1) * 2] <= 0.0f && mix[frame * 2] > 0.0f)
      ++cycles;
  check(std::abs(cycles - 440) <= 1, "the sine is at 440 Hz");

  bool channels_match = true;
  for (std::uint32_t frame = 0; frame < frames; ++frame)
    channels_match = channels_match && mix[frame * 2] == mix[frame * 2 + 1];
  check(channels_match, "both channels carry the same signal");

  engine.stop(voice);
  engine.render(mix.data(), frames);
  check(peak(mix) == 0.0f, "a stopped voice is silent");

  check(std::fabs(tokkel::note_frequency("A4") - 440.0f) < 0.01f, "A4 is 440 Hz");
  check(std::fabs(tokkel::note_frequency("G2") - 97.999f) < 0.01f, "G2 is 98 Hz");
  check(std::fabs(tokkel::note_frequency("Bb3") - tokkel::note_frequency("A#3")) < 0.01f,
        "Bb3 and A#3 are the same note");
  check(tokkel::note_frequency("Kick") == 0.0f && tokkel::note_frequency("Bloop") == 0.0f &&
            tokkel::note_frequency("") == 0.0f && tokkel::note_frequency("A") == 0.0f,
        "names that aren't notes have no frequency");

  // 240 Hz: a period of exactly 200 frames.
  auto strings = std::make_shared<tokkel::StringSynth>();
  engine.play(strings);
  strings->play_note(240.0f);
  engine.render(mix.data(), frames);
  const float note_level = peak(mix);
  check(note_level > 0.01f && note_level < 1.0f, "a played note sounds");


  // More notes at once than the synth has strings, and than it lets wait.
  // The synth strums a chord, a few tens of ms per string, so with 100 notes
  // the ones that keep a string start seconds later: listen for 4 seconds.
  for (int note = 0; note < 100; ++note)
    strings->play_note(110.0f + 10.0f * static_cast<float>(note));
  float many_level = 0.0f;
  for (int second = 0; second < 4; ++second) {
    engine.render(mix.data(), frames);
    many_level = std::fmax(many_level, peak(mix));
  }
  check(many_level > 0.01f, "many notes at once still sound");

  // Let the strings ring out before listening to anything else.
  for (int second = 0; second < 20; ++second)
    engine.render(mix.data(), frames);

  // A sampler with one event of two variations: 0.1 s at a quarter and at
  // half of full level. Mono at 24 kHz, so it is converted on the way in.
  auto sampler = std::make_shared<tokkel::Sampler>(engine.sample_rate(), engine.channels());
  const std::vector<unsigned char> quiet = constant_wav(24000, 2400, 8192);
  const std::vector<unsigned char> loud = constant_wav(24000, 2400, 16384);
  check(sampler->add("Click", quiet.data(), quiet.size()) &&
            sampler->add("Click", loud.data(), loud.size()),
        "the sampler decodes wav files");
  check(!sampler->add("Click", "not a wav", 9), "the sampler refuses what isn't audio");
  check(!sampler->trigger("Nothing"), "an unknown event doesn't play");
  engine.play(sampler);
  engine.render(mix.data(), frames);
  const float rest = peak(mix); // what's left of the strings
  check(rest < 0.01f, "a sampler that wasn't triggered is silent");

  // With two variations the sampler never repeats one: they alternate, at a
  // quarter and a half.
  bool alternates = true, right_levels = true, right_length = true;
  float previous = 0.0f;
  for (int round = 0; round < 6; ++round) {
    check(sampler->trigger("Click"), round == 0 ? "a known event plays" : "and again");
    engine.render(mix.data(), frames);
    const float level = mix[2400 * 2]; // in the middle of the sound
    right_levels = right_levels && (std::fabs(level - 0.25f) < 0.02f || std::fabs(level - 0.5f) < 0.02f);
    alternates = alternates && (round == 0 || std::fabs(level - previous) > 0.1f);
    previous = level;
    // 0.1 s of sound, then nothing.
    right_length = right_length && std::fabs(mix[9000 * 2]) < rest + 0.001f;
  }
  check(right_levels, "the sampler plays a sample at its own level");
  check(alternates, "the sampler doesn't repeat a variation");
  check(right_length, "a sample ends");

  // Many more at once than the sampler has voices.
  for (int sound = 0; sound < 100; ++sound)
    sampler->trigger("Click", 0.01f);
  engine.render(mix.data(), frames);
  check(peak(mix) > 0.05f, "many sounds at once still sound");

  // One variation of a full second at a quarter level: two triggers overlap
  // and add up, unless the second one restarts the event.
  const std::vector<unsigned char> held = constant_wav(48000, 48000, 8192);
  sampler->add("Hold", held.data(), held.size());
  engine.render(mix.data(), frames); // the clicks above end
  sampler->trigger("Hold");
  engine.render(mix.data(), frames / 2);
  sampler->trigger("Hold");
  engine.render(mix.data(), frames / 4);
  check(std::fabs(mix[4800 * 2] - 0.5f) < 0.02f, "triggers of one event overlap");
  for (int second = 0; second < 2; ++second) // the event ends
    engine.render(mix.data(), frames);
  sampler->trigger("Hold");
  engine.render(mix.data(), frames / 2);
  sampler->trigger("Hold", 1.0f, true);
  engine.render(mix.data(), frames / 4);
  check(std::fabs(mix[4800 * 2] - 0.25f) < 0.02f, "a restart cuts the event's previous sound off");
  for (int second = 0; second < 2; ++second) // the event ends
    engine.render(mix.data(), frames);
  // Several restarts at once (several lines of text typing in the same
  // frame) play one sound, not several that cut each other off.
  for (int line = 0; line < 4; ++line)
    sampler->trigger("Hold", 1.0f, true);
  engine.render(mix.data(), frames / 4);
  check(std::fabs(mix[4800 * 2] - 0.25f) < 0.02f, "restarts at once play one sound");
  for (int second = 0; second < 2; ++second)
    engine.render(mix.data(), frames);

  // Stopping: what an event plays fades out, and a trigger stopped before the
  // audio thread got to it never starts; one after the stop does.
  sampler->trigger("Hold");
  engine.render(mix.data(), frames / 4);
  sampler->stop("Hold");
  engine.render(mix.data(), frames / 4);
  check(peak(mix) < 0.26f && std::fabs(mix[4800 * 2]) < 0.001f, "a stopped sound goes quiet");
  sampler->trigger("Hold");
  sampler->stop("Hold");
  engine.render(mix.data(), frames / 4);
  check(peak(mix) < 0.001f, "a sound stopped before it started never plays");
  sampler->stop("Hold");
  sampler->trigger("Hold");
  engine.render(mix.data(), frames / 4);
  check(std::fabs(mix[4800 * 2] - 0.25f) < 0.02f, "a trigger after a stop plays");
  sampler->stop_all();
  engine.render(mix.data(), frames / 4);
  check(std::fabs(mix[4800 * 2]) < 0.001f, "stop_all() stops every event");

  // Placed sounds. A listener at the origin, panning along x, hearing fully
  // up to 10 away and nothing from 100 on.
  tokkel::Listener ear;
  ear.min_distance = 10.0f;
  ear.max_distance = 100.0f;
  check(ear.gain({5.0f, 0.0f, 0.0f}) == 1.0f && ear.gain({0.0f, 0.0f, 100.0f}) == 0.0f &&
            std::fabs(ear.gain({0.0f, 55.0f, 0.0f}) - 0.25f) < 0.001f,
        "a listener hears less the further off a sound is");
  check(ear.pan({0.0f, 50.0f, 0.0f}) == 0.0f && std::fabs(ear.pan({50.0f, 0.0f, 0.0f}) - 0.5f) < 0.001f &&
            ear.pan({-500.0f, 0.0f, 0.0f}) == -1.0f,
        "a listener pans by how far right a sound is");
  sampler->set_listener(ear);
  sampler->trigger_at("Hold", {0.0f, 55.0f, 0.0f});
  engine.render(mix.data(), frames / 4);
  check(std::fabs(mix[4800 * 2] - 0.0625f) < 0.005f && mix[4800 * 2] == mix[4800 * 2 + 1],
        "a placed sound in front is quieter far off, on both sides");
  sampler->stop("Hold");
  engine.render(mix.data(), frames / 4);
  // 50 to the right: gain (1 - 40/90)^2, the left side at 40% of the pan's half.
  const float side_gain = 0.25f * (50.0f / 90.0f) * (50.0f / 90.0f);
  sampler->trigger_at("Hold", {50.0f, 0.0f, 0.0f});
  engine.render(mix.data(), frames / 4);
  check(std::fabs(mix[4800 * 2 + 1] - side_gain) < 0.005f &&
            std::fabs(mix[4800 * 2] - 0.7f * side_gain) < 0.005f,
        "a placed sound to the right is louder on the right");
  // The listener moves onto it: it comes in at full level, centred.
  ear.position = {50.0f, 0.0f, 0.0f};
  sampler->set_listener(ear);
  engine.render(mix.data(), frames / 4);
  check(std::fabs(mix[11000 * 2] - 0.25f) < 0.005f && std::fabs(mix[11000 * 2 + 1] - 0.25f) < 0.005f,
        "a placed sound follows the listener as it moves");
  sampler->trigger_at("Hold", {500.0f, 0.0f, 0.0f}, 1.0f, true);
  engine.render(mix.data(), frames / 4);
  check(peak(mix) < 0.26f && std::fabs(mix[4800 * 2]) < 0.001f, "a placed sound out of hearing is silent");
  sampler->stop_all();
  engine.render(mix.data(), frames / 4);

  // The engine's listener reaches its samplers, also ones added after it.
  engine.set_listener(ear);
  check(engine.listener().max_distance == 100.0f && engine.listener().position.x == 50.0f,
        "the engine keeps its listener");
  auto placed = engine.add_sampler("Placed");
  placed->add("Hold", held.data(), held.size());
  engine.trigger_at("Placed", "Hold", {50.0f, 55.0f, 0.0f});
  engine.render(mix.data(), frames / 4);
  check(std::fabs(mix[4800 * 2] - 0.0625f) < 0.005f, "a sampler the engine adds hears its listener");
  check(engine.stop_event("Placed", "Hold") && !engine.stop_event("Nothing", "Hold"),
        "the engine stops a sampler's event");
  engine.render(mix.data(), frames / 4);
  check(std::fabs(mix[4800 * 2]) < 0.001f, "and it goes quiet");

  // Delays: a sample and a note asked for 0.5 s from now are silent until then.
  engine.render(mix.data(), frames);
  auto first_sound = [&](float threshold) {
    for (std::uint32_t frame = 0; frame < frames; ++frame)
      if (std::fabs(mix[frame * 2]) > threshold)
        return static_cast<int>(frame);
    return -1;
  };
  sampler->trigger("Hold", 1.0f, false, 0.5f);
  engine.render(mix.data(), frames);
  const int sample_start = first_sound(0.1f);
  check(std::abs(sample_start - 24000) <= 600, "a delayed sample starts on time");
  for (int second = 0; second < 2; ++second)
    engine.render(mix.data(), frames);
  // A delayed restart only cuts the event's sound off when it starts.
  sampler->trigger("Hold");
  sampler->trigger("Hold", 1.0f, true, 0.5f);
  engine.render(mix.data(), frames);
  check(std::fabs(mix[12000 * 2] - 0.25f) < 0.02f && std::fabs(mix[36000 * 2] - 0.25f) < 0.02f,
        "a delayed restart plays the old sound until the new one starts");
  for (int second = 0; second < 2; ++second)
    engine.render(mix.data(), frames);

  // The bell: a square wave at the note's pitch that dies away.
  auto bell = std::make_shared<tokkel::BellSynth>();
  engine.play(bell);
  bell->play_note(tokkel::note_frequency("A4"));
  engine.render(mix.data(), frames);
  int bell_crossings = 0;
  for (std::uint32_t frame = 4800; frame < 9600; ++frame)
    bell_crossings += (mix[frame * 2] < 0.0f) != (mix[(frame + 1) * 2] < 0.0f);
  const float bell_start = std::fabs(mix[100 * 2]), bell_later = std::fabs(mix[24000 * 2]);
  std::printf("      bell: %.0f Hz, %.2f at the start, %.3f after 0.5 s\n",
              bell_crossings / 2.0f / 0.1f, bell_start, bell_later);
  check(std::abs(bell_crossings / 2.0f / 0.1f - 440.0f) < 15.0f, "a bell note is in tune");
  check(bell_start > 0.15f && bell_start < 0.5f && bell_later < bell_start / 2.0f,
        "a bell note dies away");
  for (int second = 0; second < 2; ++second)
    engine.render(mix.data(), frames);
  check(peak(mix) < 0.001f, "a bell note ends");

  // A bell note placed to the right: the left keeps 40% (Synth::pan_gains()).
  bell->play_note(tokkel::note_frequency("A4"), 1.0f, 0.95f, 0.0f, 1.0f);
  engine.render(mix.data(), frames);
  float bell_left = 0.0f, bell_right = 0.0f;
  for (std::uint32_t frame = 0; frame < 4800; ++frame) {
    bell_left = std::fmax(bell_left, std::fabs(mix[frame * 2]));
    bell_right = std::fmax(bell_right, std::fabs(mix[frame * 2 + 1]));
  }
  std::printf("      bell on the right: left %.3f, right %.3f\n", bell_left, bell_right);
  check(bell_right > 0.15f && bell_left < bell_right * 0.6f && bell_left > bell_right * 0.2f,
        "a bell note pans");
  // Placed through the listener: to its right, so heard on the right.
  tokkel::Listener ears;
  ears.max_distance = 10.0f;
  engine.set_listener(ears);
  for (int second = 0; second < 2; ++second)
    engine.render(mix.data(), frames);
  engine.add_instrument("Bell", std::make_shared<tokkel::BellSynth>());
  engine.play_note_at("Bell", tokkel::Vec3{8.0f, 0.0f, 0.0f}, tokkel::note_frequency("A4"));
  engine.render(mix.data(), frames);
  float placed_left = 0.0f, placed_right = 0.0f;
  for (std::uint32_t frame = 0; frame < 4800; ++frame) {
    placed_left = std::fmax(placed_left, std::fabs(mix[frame * 2]));
    placed_right = std::fmax(placed_right, std::fabs(mix[frame * 2 + 1]));
  }
  std::printf("      bell placed right of the listener: left %.3f, right %.3f\n", placed_left,
              placed_right);
  check(placed_right > 0.01f && placed_right > placed_left * 1.5f,
        "play_note_at() pans by the listener");
  engine.set_listener(tokkel::Listener{});
  for (int second = 0; second < 2; ++second)
    engine.render(mix.data(), frames);

  auto delayed = std::make_shared<tokkel::PianoSynth>();
  engine.play(delayed);
  delayed->play_note(tokkel::note_frequency("A4"), 1.0f, 0.95f, 0.5f);
  engine.render(mix.data(), frames);
  const int note_start = first_sound(0.01f);
  check(std::abs(note_start - 24000) <= 600, "a delayed note starts on time");
  for (int second = 0; second < 6; ++second)
    engine.render(mix.data(), frames);

  auto piano = std::make_shared<tokkel::PianoSynth>();
  engine.play(piano);
  piano->play_note(tokkel::note_frequency("D4"));
  engine.render(mix.data(), frames);
  const float piano_level = peak(mix);
  check(piano_level > 0.05f && piano_level < 1.0f, "a piano note sounds");
  std::printf("      piano D4 peak %.2f\n", piano_level);
  for (const char *note : {"D4", "F4", "A4", "C#5", "D5", "G4", "Bb4", "E4"})
    piano->play_note(tokkel::note_frequency(note));
  engine.render(mix.data(), frames);
  check(peak(mix) > 0.05f && peak(mix) < 1.0f, "a full piano chord doesn't clip");
  for (int second = 0; second < 8; ++second) // the piano rings out
    engine.render(mix.data(), frames);

  // The pitch in `mix` (left channel) near `expected` Hz: the lag at which the
  // signal looks most like itself, refined between samples.
  auto pitch = [&](float expected) { return pitch_of(mix, expected); };

  auto guitar = std::make_shared<tokkel::GuitarSynth>();
  engine.play(guitar);
  bool in_tune = true;
  for (const char *note : {"E2", "A2", "G3", "E4", "A5"}) {
    const float wanted = tokkel::note_frequency(note);
    guitar->play_note(wanted, 1.0f, 3.0f);
    engine.render(mix.data(), frames);
    const double off = cents(pitch(wanted), wanted);
    std::printf("      guitar %s: %+.1f cents, peak %.2f\n", note, off, peak(mix));
    in_tune = in_tune && std::fabs(off) < 5.0;
    for (int second = 0; second < 8; ++second)
      engine.render(mix.data(), frames);
  }
  check(in_tune, "the guitar is in tune");

  guitar->play_note(tokkel::note_frequency("A2"), 0.8f);
  engine.render(mix.data(), frames);
  const float guitar_level = peak(mix);
  check(guitar_level > 0.05f && guitar_level < 1.0f, "a guitar note sounds");
  for (int second = 0; second < 8; ++second)
    engine.render(mix.data(), frames);

  // Held for 3 s, the note still rings at 2 s; held for 0.2 s, it's damped.
  auto level_at_2s = [&](float sustain) {
    guitar->play_note(tokkel::note_frequency("A2"), 1.0f, sustain);
    engine.render(mix.data(), frames);
    engine.render(mix.data(), frames);
    const float level = peak(mix);
    for (int second = 0; second < 8; ++second)
      engine.render(mix.data(), frames);
    return level;
  };
  const float rings = level_at_2s(3.0f), damped = level_at_2s(0.2f);
  std::printf("      guitar at 2 s: held %.4f, damped %.6f\n", rings, damped);
  check(rings > 0.02f * guitar_level && damped < rings * 0.05f, "a guitar note is damped after its sustain");

  for (const char *note : {"E2", "B2", "E3", "G#3", "B3", "E4"})
    guitar->play_note(tokkel::note_frequency(note));
  engine.render(mix.data(), frames);
  std::printf("      guitar chord peak %.2f\n", peak(mix));
  check(peak(mix) > 0.05f && peak(mix) < 1.0f, "a full guitar chord doesn't clip");
  // Strummed: the top string starts tens of ms after the bottom one.
  check(first_sound(0.001f) < 480, "a strum starts at once");
  for (int second = 0; second < 10; ++second) // the chord rings out
    engine.render(mix.data(), frames);

  // Instruments by name, owned by the engine.
  check(!engine.play_note("Nothing", 220.0f) && engine.instrument("Nothing") == nullptr,
        "an unknown instrument doesn't play");
  check(engine.add_instrument("Keys", std::make_shared<tokkel::PianoSynth>()) &&
            engine.instrument("Keys") != nullptr,
        "an instrument is added by name");
  check(engine.play_note("Keys", tokkel::note_frequency("D4")), "a note is played by name");
  engine.render(mix.data(), frames / 4);
  check(peak(mix) > 0.05f, "a note on a named instrument sounds");
  auto first_keys = engine.instrument("Keys");
  engine.add_instrument("Keys", std::make_shared<tokkel::PianoSynth>());
  engine.render(mix.data(), frames / 4);
  check(engine.instrument("Keys") != first_keys && peak(mix) < 0.0001f,
        "a new instrument under a name replaces and stops the old one");

  // A sampler by name, made by the engine in its format.
  for (int second = 0; second < 8; ++second)
    engine.render(mix.data(), frames);
  auto sounds = engine.add_sampler("Sounds");
  check(sounds != nullptr && engine.sampler("Sounds") == sounds && engine.instrument("Sounds") == sounds,
        "a sampler is added by name");
  check(sounds->add("Hold", held.data(), held.size()), "a named sampler takes sounds");
  check(!engine.trigger("Nothing", "Hold") && !engine.trigger("Sounds", "Nothing"),
        "an unknown sampler or event doesn't play");
  check(engine.trigger("Sounds", "Hold"), "a sound is triggered by name");
  engine.render(mix.data(), frames / 4);
  check(std::fabs(mix[4800 * 2] - 0.25f) < 0.02f, "a sound on a named sampler plays");
  check(engine.add_instrument("Sounds", std::make_shared<tokkel::PianoSynth>()) &&
            engine.sampler("Sounds") == nullptr,
        "an instrument can take a sampler's name");

  // Drums: a kick and a snare at the level the samples had.
  for (int second = 0; second < 8; ++second)
    engine.render(mix.data(), frames);
  auto drums = std::make_shared<tokkel::DrumSynth>();
  engine.play(drums);
  auto rms = [&](std::uint32_t from, std::uint32_t to) {
    double sum = 0.0;
    for (std::uint32_t frame = from; frame < to; ++frame)
      sum += mix[frame * 2] * mix[frame * 2];
    return std::sqrt(sum / (to - from));
  };
  drums->play_note(tokkel::note_frequency("C2"));
  engine.render(mix.data(), frames);
  const float kick_peak = peak(mix);
  const double kick_rms = rms(0, 4800);
  // Its pitch once the drop is over: count the crossings from 0.2 to 0.4 s.
  int crossings = 0;
  for (std::uint32_t frame = 9600; frame < 19200; ++frame)
    crossings += (mix[frame * 2] < 0.0f) != (mix[(frame + 1) * 2] < 0.0f);
  const float kick_pitch = crossings / 2.0f / 0.2f;
  std::printf("      kick: peak %.2f, rms %.3f, %.0f Hz\n", kick_peak, kick_rms, kick_pitch);
  check(kick_rms > 0.12 && kick_rms < 0.25, "a kick sounds as loud as the sample");
  check(kick_pitch > 40.0f && kick_pitch < 65.0f, "a kick drops to a low pitch");
  engine.render(mix.data(), frames);

  drums->play_note(tokkel::note_frequency("D2"));
  engine.render(mix.data(), frames);
  const float snare_peak = peak(mix);
  const double snare_rms = rms(0, 4800);
  bool wide = false;
  for (std::uint32_t frame = 0; frame < 4800; ++frame)
    wide = wide || std::fabs(mix[frame * 2] - mix[frame * 2 + 1]) > 0.01f;
  std::printf("      snare: peak %.2f, rms %.3f\n", snare_peak, snare_rms);
  check(snare_rms > 0.06 && snare_rms < 0.12, "a snare sounds as loud as the sample");
  check(wide, "the snare wires are a little wide");
  engine.render(mix.data(), frames);
  check(peak(mix) < 0.001f, "the drums end");

  // Hi-hats: a closed one is a short tick under the snare, an open one rings
  // on until a closed one chokes it.
  drums->play_note(tokkel::note_frequency("F#2"));
  engine.render(mix.data(), frames);
  const double closed_rms = rms(0, 4800);
  const double closed_tail = rms(9600, 19200);
  std::printf("      closed hi-hat: rms %.3f\n", closed_rms);
  check(closed_rms > 0.01 && closed_rms < snare_rms, "a closed hi-hat sounds, softer than the snare");
  check(closed_tail < 0.001, "a closed hi-hat is short");
  drums->play_note(tokkel::note_frequency("A#2"));
  engine.render(mix.data(), frames);
  const double open_tail = rms(9600, 19200);
  std::printf("      open hi-hat: rms %.3f, %.3f at 0.2-0.4 s\n", rms(0, 4800), open_tail);
  check(open_tail > 10 * closed_tail && open_tail > 0.005, "an open hi-hat rings on");
  drums->play_note(tokkel::note_frequency("A#2"));
  engine.render(mix.data(), 2400);
  drums->play_note(tokkel::note_frequency("F#2"));
  engine.render(mix.data(), frames);
  check(rms(9600, 19200) < 0.001, "a closed hi-hat chokes an open one");
  engine.render(mix.data(), frames);
  check(peak(mix) < 0.001f, "the hi-hats end");

  drums->play_note(tokkel::note_frequency("A4"));
  engine.render(mix.data(), frames);
  check(peak(mix) < 0.0001f, "a note that's no drum is silent");

  // Bass: every waveform in tune, from E1 up; one note at a time.
  for (int second = 0; second < 4; ++second)
    engine.render(mix.data(), frames);
  auto bass = std::make_shared<tokkel::BassSynth>();
  engine.play(bass);
  const char *waveform_names[] = {"sine", "triangle", "square", "saw"};
  bool bass_in_tune = true, bass_levels = true;
  for (int waveform : {tokkel::BassSynth::Sine, tokkel::BassSynth::Triangle,
                       tokkel::BassSynth::Square, tokkel::BassSynth::Saw}) {
    bass->waveform = waveform;
    for (const char *note : {"E1", "A1", "E2", "G3"}) {
      const float wanted = tokkel::note_frequency(note);
      bass->play_note(wanted, 1.0f, 3.0f);
      engine.render(mix.data(), frames);
      const double off = cents(pitch(wanted), wanted);
      std::printf("      bass %-8s %s: %+.1f cents, peak %.2f\n", waveform_names[waveform], note,
                  off, peak(mix));
      // 8 cents, not 5: while the filter closes after a note, its growing
      // delay bends the square and saw down, up to 6.5 cents on the lowest
      // notes.
      bass_in_tune = bass_in_tune && std::fabs(off) < 8.0;
      bass_levels = bass_levels && peak(mix) > 0.15f && peak(mix) < 0.8f;
      bass->play_note(wanted, 0.0f); // a silent note ends it
      engine.render(mix.data(), frames / 2);
    }
  }
  check(bass_in_tune, "every bass waveform is in tune");
  check(bass_levels, "every bass waveform is about as loud as the other synths");
  bass->waveform = tokkel::BassSynth::Square;

  auto bass_at_2s = [&](float sustain, bool another) {
    bass->play_note(tokkel::note_frequency("A1"), 0.8f, sustain);
    engine.render(mix.data(), frames / 2);
    if (another)
      bass->play_note(tokkel::note_frequency("A1"), 0.0f); // a silent note takes over
    engine.render(mix.data(), frames / 2);
    const float first = peak(mix);
    engine.render(mix.data(), frames);
    const float level = peak(mix);
    engine.render(mix.data(), frames);
    return std::make_pair(first, level);
  };
  const float bass_damped = bass_at_2s(0.2f, false).second;
  const float bass_stopped = bass_at_2s(10.0f, true).second;
  check(bass_damped < 0.001f, "a bass note is released after its sustain");
  check(bass_stopped < 0.001f, "a new bass note takes over from the one before");

  // A chord: only its lowest note plays.
  bass->play_note(tokkel::note_frequency("E3"), 1.0f, 3.0f);
  bass->play_note(tokkel::note_frequency("A1"), 1.0f, 3.0f);
  bass->play_note(tokkel::note_frequency("C#3"), 1.0f, 3.0f);
  engine.render(mix.data(), frames);
  check(std::fabs(cents(pitch(tokkel::note_frequency("A1")), tokkel::note_frequency("A1"))) < 5.0,
        "of a chord, the bass plays the lowest note");
  bass->play_note(tokkel::note_frequency("A1"), 0.0f);
  engine.render(mix.data(), frames);

  // An instrument's own reverb: a snare (silent after a second) rings on.
  for (int second = 0; second < 2; ++second)
    engine.render(mix.data(), frames);
  drums->reverb = 60.0f; // three seconds
  drums->play_note(tokkel::note_frequency("D2"));
  engine.render(mix.data(), frames);
  engine.render(mix.data(), frames / 4);
  const float instrument_tail = peak(mix);
  drums->reverb = 0.0f;
  // miniaudio reads a source a little ahead: the change is heard an engine
  // buffer (a few ms) later.
  engine.render(mix.data(), frames / 20);
  std::fill(mix.begin(), mix.end(), 0.0f); // peak() reads it all; only a quarter is rendered
  engine.render(mix.data(), frames / 4);
  std::printf("      snare reverb tail %.4f\n", instrument_tail);
  check(instrument_tail > 0.001f, "an instrument's reverb rings after it");
  check(peak(mix) < 0.0001f, "turning an instrument's reverb off ends it");

  // The main reverb: a sine that stopped rings on, on top of everything.
  engine.set_reverb(1.0f);
  auto ping = std::make_shared<tokkel::StkInstrument<stk::SineWave>>();
  ping->unit.setFrequency(440.0);
  engine.render(mix.data(), frames / 10); // the reverb glides in
  const tokkel::VoiceId pinged = engine.play(ping, 0.3f);
  engine.render(mix.data(), frames / 5);
  const float dry_and_wet = peak(mix);
  engine.stop(pinged);
  engine.render(mix.data(), frames / 4);
  const float main_tail = peak(mix);
  std::printf("      main reverb: %.3f with the sine, tail %.4f\n", dry_and_wet, main_tail);
  check(main_tail > 0.01f, "the main reverb rings after a sound stops");
  check(dry_and_wet < 1.0f, "the main reverb doesn't clip a sound");
  engine.set_reverb(0.0f);
  engine.render(mix.data(), frames); // it glides out
  std::fill(mix.begin(), mix.end(), 0.0f);
  engine.render(mix.data(), frames / 4);
  check(peak(mix) < 0.0001f, "turning the main reverb off ends it");

  // Parameters by name, through the engine.
  engine.add_instrument("Bass", std::make_shared<tokkel::BassSynth>());
  auto named_bass = std::dynamic_pointer_cast<tokkel::BassSynth>(engine.instrument("Bass"));
  check(engine.set_parameter("Bass", "Reverb", 30.0f) && named_bass->reverb == 30.0f,
        "every synth knows Reverb");
  check(engine.set_parameter("Bass", "Waveform", 3.0f) &&
            named_bass->waveform == tokkel::BassSynth::Saw,
        "a synth knows its own parameters");
  check(engine.set_parameter("Bass", "Tick", 12.0f), "a synth ignores parameters it doesn't know");
  check(!engine.set_parameter("Nothing", "Reverb", 1.0f), "no parameters for an unknown instrument");

  // Volume and pan: a held bass note, measured left and right.
  for (int second = 0; second < 2; ++second)
    engine.render(mix.data(), frames);
  named_bass->reverb = 0.0f;
  // A fresh note for each, measured at the same point in it; a silent
  // note after (the bass plays one note at a time) ends it.
  auto sides = [&]() {
    engine.render(mix.data(), frames / 10); // a change glides in
    engine.play_note("Bass", tokkel::note_frequency("A2"), 0.8f, 10.0f);
    std::fill(mix.begin(), mix.end(), 0.0f);
    engine.render(mix.data(), frames / 4);
    float left = 0.0f, right = 0.0f;
    for (std::uint32_t frame = frames / 8; frame < frames / 4; ++frame) {
      left = std::fmax(left, std::fabs(mix[frame * 2]));
      right = std::fmax(right, std::fabs(mix[frame * 2 + 1]));
    }
    engine.play_note("Bass", tokkel::note_frequency("A2"), 0.0f);
    engine.render(mix.data(), frames / 2);
    return std::make_pair(left, right);
  };
  const auto [middle_left, middle_right] = sides();
  engine.set_parameter("Bass", "Pan", -1.0f);
  const auto [panned_left, panned_right] = sides();
  engine.set_parameter("Bass", "Pan", 0.0f);
  engine.set_parameter("Bass", "Volume", 0.5f);
  const auto [half_left, half_right] = sides();
  std::printf("      bass L/R: middle %.3f/%.3f, panned left %.3f/%.3f, half %.3f/%.3f\n",
              middle_left, middle_right, panned_left, panned_right, half_left, half_right);
  check(middle_left > 0.05f && std::fabs(middle_left - middle_right) < 0.001f,
        "an instrument plays in the middle");
  check(std::fabs(panned_left - middle_left) < 0.05f * middle_left && panned_right < 0.001f,
        "panned left, only the left side plays");
  check(std::fabs(half_left - 0.5f * middle_left) < 0.05f * middle_left,
        "at volume 0.5, it is half as loud");
  engine.set_parameter("Bass", "Volume", 0.0f);
  engine.render(mix.data(), frames);

  // Cello: in tune over its range, sustained while bowed, silent after.
  for (int second = 0; second < 2; ++second)
    engine.render(mix.data(), frames);
  auto cello = std::make_shared<tokkel::CelloSynth>();
  engine.play(cello);
  bool cello_in_tune = true, cello_level = true;
  for (const char *note : {"C2", "G2", "D3", "A3", "A4"}) {
    const float wanted = tokkel::note_frequency(note);
    cello->play_note(wanted, 0.8f, 1.5f);
    engine.render(mix.data(), frames);
    const double off = cents(pitch(wanted), wanted);
    std::printf("      cello %s: %+.1f cents, peak %.2f\n", note, off, peak(mix));
    cello_in_tune = cello_in_tune && std::fabs(off) < 10.0;
    cello_level = cello_level && peak(mix) > 0.1f && peak(mix) < 0.9f;
    for (int second = 0; second < 2; ++second)
      engine.render(mix.data(), frames);
  }
  check(cello_in_tune, "the cello is in tune");
  check(cello_level, "the cello is about as loud as the other synths");
  cello->play_note(tokkel::note_frequency("D3"), 0.8f, 2.5f);
  engine.render(mix.data(), frames);
  const float bowing = peak(mix);
  engine.render(mix.data(), frames);
  const float still_bowing = peak(mix);
  for (int second = 0; second < 2; ++second)
    engine.render(mix.data(), frames);
  std::printf("      cello bowed: %.3f, a second on %.3f, after %.6f\n", bowing, still_bowing, peak(mix));
  check(still_bowing > 0.7f * bowing, "a bowed note keeps sounding");
  check(peak(mix) < 0.02f * bowing, "the cello fades once the bow lifts");

  // Resting: a synth gone silent isn't computed until its next note.
  CountingSynth counting;
  render_alone(counting, 1.0f);
  const long never_played = counting.ticks;
  counting.play_note(440.0f);
  std::vector<float> rested = render_alone(counting, 1.0f);
  const long after_note = counting.ticks - never_played;
  std::printf("      ticks in a second: never played %ld, after a note %ld\n", never_played,
              after_note);
  check(never_played < 12000, "a synth never played rests after a moment");
  check(rested[0] == 1.0f && after_note > 10000 && after_note < 13000,
        "a note wakes a resting synth, which rests again once it's silent");
  counting.hold = true;
  const long before_hold = counting.ticks;
  render_alone(counting, 1.0f);
  check(counting.ticks - before_hold == 48000, "a synth holding a note back doesn't rest");
  counting.hold = false;
  render_alone(counting, 1.0f);
  counting.volume = 0.5f;
  counting.play_note(440.0f, 1.0f, 0.95f, 0.5f);
  rested = render_alone(counting, 1.0f);
  int counted_start = -1;
  for (int frame = 0; frame < 48000 && counted_start < 0; ++frame)
    if (rested[frame * 2] > 0.01f) // the last note's fade never quite reaches 0
      counted_start = frame;
  check(counted_start == 24000, "a delayed note on a resting synth starts on time");
  check(std::fabs(rested[24000 * 2] - 0.5f) < 0.001f,
        "a resting synth's volume is already where it was going");

  // Every synth falls silent after its note, and plays the next one as loud.
  auto wakes = [](tokkel::Synth &synth, const char *name, float frequency) {
    const float first = peak(render_alone(synth, 1.0f));
    render_alone(synth, 4.0f);
    const float after = peak(render_alone(synth, 1.0f));
    synth.play_note(frequency, 1.0f, 0.5f);
    const float again = peak(render_alone(synth, 1.0f));
    std::printf("      %s: peak %.3f, after %g, again %.3f\n", name, first, after, again);
    return after == 0.0f && again > 0.4f * first && again < 2.5f * first;
  };
  bool all_wake = true;
  {
    tokkel::StringSynth synth;
    synth.play_note(220.0f, 1.0f, 0.5f);
    all_wake = wakes(synth, "strings", 220.0f) && all_wake;
  }
  {
    tokkel::PianoSynth synth;
    synth.play_note(220.0f, 1.0f, 0.5f);
    all_wake = wakes(synth, "piano", 220.0f) && all_wake;
  }
  {
    tokkel::GuitarSynth synth;
    synth.play_note(220.0f, 1.0f, 0.5f);
    all_wake = wakes(synth, "guitar", 220.0f) && all_wake;
  }
  {
    tokkel::CelloSynth synth;
    synth.play_note(220.0f, 1.0f, 0.5f);
    all_wake = wakes(synth, "cello", 220.0f) && all_wake;
  }
  {
    tokkel::BassSynth synth;
    synth.play_note(110.0f, 1.0f, 0.5f);
    all_wake = wakes(synth, "bass", 110.0f) && all_wake;
  }
  {
    tokkel::BellSynth synth;
    synth.play_note(880.0f, 1.0f, 0.5f);
    all_wake = wakes(synth, "bell", 880.0f) && all_wake;
  }
  {
    tokkel::DrumSynth synth;
    const float kick = tokkel::note_frequency("C2");
    synth.play_note(kick, 1.0f, 0.5f);
    all_wake = wakes(synth, "drums", kick) && all_wake;
  }
  check(all_wake, "every synth goes silent after a note, and wakes for the next");

  return failures == 0 ? 0 : 1;
}
