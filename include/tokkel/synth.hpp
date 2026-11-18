#pragma once

#include "tokkel/tokkel.hpp"
#include "tokkel/ring.hpp"

#include <Chorus.h>
#include <StifKarp.h>
#include <ADSR.h>
#include <BiQuad.h>
#include <JCRev.h>

#include <array>
#include <cstddef>
#include <mutex>
#include <random>
#include <string>
#include <vector>
#include <atomic>

namespace tokkel {

// "A4", "Bb3", "C#5" -> frequency in Hz (A4 = 440). 0 when `name` isn't a
// note name: a letter A-G, an optional # or b, and one octave digit.
float note_frequency(const std::string &name);

// A Instrument that plays notes. To make a synth, subclass it and implement
// note_on() and tick(); start it once with Engine::play(), then call
// play_note() whenever a note should sound. Nothing has to be stopped.
//
//   auto synth = std::make_shared<tokkel::StringSynth>();
//   engine.play(synth);
//   synth->play_note(tokkel::note_frequency("G2"));
class Synth : public Instrument {
public:
  Synth();

  // Play a note: `frequency` in Hz, `amplitude` in [0, 1], starting `delay`
  // seconds from the next block of audio (0: at its start), placed at `pan`
  // (-1 left, 1 right; see note_pan()). Callable from any thread.
  void play_note(float frequency, float amplitude = 1.0f, float sustain = 0.95f,
                 float delay = 0.0f, float pan = 0.0f);

  void render(float *out, std::uint32_t frames, std::uint32_t channels) final;

  // The synth's own reverb, on everything it plays: how long it rings is
  // reverb / 20 seconds (20: one second), heard 30% under 70% of the dry
  // sound. 0: none. Settable from any thread, any time.
  std::atomic<float> reverb{0.0f};
  // How loud it is, as a gain: 1 as it is, 0 silent, above 1 louder.
  std::atomic<float> volume{1.0f};
  // Where it sits between the speakers: -1 left, 0 the middle, 1 right. A
  // side is turned down as it goes to the other (balance).
  std::atomic<float> pan{0.0f};
  // wether to enable "picking" (or tokkeling :-)) extra notes
  std::atomic<bool> tokkel{false};
  // how many extra notes to tokkel, if enabled
  std::atomic<std::size_t> tokkel_amt{2};

  // Set a parameter by name, from any thread. Every synth knows "Reverb",
  // "Volume" and "Pan" (the atomics above); changes to volume and pan glide
  // in over about 20 ms, so they don't click. A synth that has more overrides this, handles its
  // own names, and passes the rest on to Synth::set_parameter(). Names a
  // synth doesn't know are ignored, so one parameter can be sent to every
  // instrument (a "Tick", say) and only the ones that use it change.
  // Overrides only store the value (in an atomic): tick() picks it up.
  virtual void set_parameter(const std::string &name, float value);

protected:
  // Both are called on the audio thread only.

  // Start a note.
  virtual void note_on(float frequency, float amplitude, float sustain) = 0;
  // The pan of the note being started, read in note_on(): -1 left, 1 right.
  // A synth that places its notes keeps it with the note's voice (see
  // pan_gains()); one that doesn't ignores it, and only the synth's own
  // `pan` places it.
  float note_pan() const { return starting_pan; }
  // A note's two sides at `pan`, the samples' gentle pan
  // (Sampler::placed_gains()): the far side keeps 40%, so a sound off to one
  // side is still heard in both ears, as in a room.
  static void pan_gains(float pan, float &left, float &right) {
    left = pan > 0.0f ? 1.0f - 0.6f * pan : 1.0f;
    right = pan < 0.0f ? 1.0f + 0.6f * pan : 1.0f;
  }
  // The next sample of the synth's stereo output. On a mono engine the two
  // are averaged.
  virtual void tick(float &left, float &right) = 0;

  float random_f(float min, float max);

private:
  struct Note {
    float frequency;
    float amplitude;
    float sustain;
    std::uint64_t delay; // in frames
    float pan;
  };

  void start(const Note &note);
  float starting_pan = 0.0f;
  struct Scheduled {
    Note note;
    std::uint64_t start; // the frame (counted by `now`) it starts on
  };

  void start_due_notes();

  // Notes played but not started yet: play_note() adds them, render() starts
  // them on the audio thread.
  std::mutex mutex;
  tokkel::Ring<Note, 64> waiting;
  std::minstd_rand random;

  // Audio thread only: notes with a delay, the frames rendered so far, and
  // the earliest start among the scheduled notes.
  std::vector<Scheduled> scheduled;
  std::uint64_t now = 0;
  std::uint64_t next_start = UINT64_MAX;

  // The reverb, and the `reverb` it was last set to.
  stk::JCRev rev;
  float current_reverb = 0.0f;
  // The volume and pan heard now, gliding to the atomics' values.
  float current_volume = 1.0f, current_pan = 0.0f;
};

// Plucked strings (Karplus-Strong, STK's Plucked). Each note plucks the next
// of a fixed set of strings, so when all of them are ringing a new note takes
// over the one plucked longest ago. Create it after the Engine (it needs
// STK's sample rate).
class StringSynth : public Synth {
public:
  StringSynth();

protected:
  void note_on(float frequency, float amplitude, float sustain) override;
  void tick(float &left, float &right) override;

private:
  struct StringSynthVoice {
    stk::StifKarp main, detuned, low; 
    stk::BiQuad body;
    stk::ADSR envelope;

    int start_in = -1; // -1 means don't play this note - the default until it's set
    float frequency = 0.0f, amplitude = 0.0f, sustain = 0.0f;
  };

  std::array<StringSynthVoice, 16> voices;
  int strum_pos = 0; // notes started since last tick()
  stk::Chorus chorus;
  stk::BiQuad lp_filter;
  stk::BiQuad body_air;
  stk::BiQuad body_top;

  std::size_t next_voice = 0;
};

} // namespace tokkel
