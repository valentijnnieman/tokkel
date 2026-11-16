#pragma once

#include "tokkel/synth.hpp"

#include <ADSR.h>
#include <BiQuad.h>

#include <atomic>

namespace tokkel {

// A synth bass: one oscillator through a resonant low-pass whose cutoff
// sweeps down after every note (the "bwow"), an amp envelope, and a little
// drive, whose overtones also let low notes be heard on small speakers.
//
// It plays one note at a time, as a bass line does: a new note takes over
// from the one before (without a click: the oscillator runs on). Of notes
// started together, the lowest plays, the chord's root.
//
// `amplitude` is how hard it's played: louder, and the filter opens further.
// `sustain` is how long the note is held before it is released.
// Create it after the Engine (it needs STK's sample rate).
//
//   auto bass = std::make_shared<tokkel::BassSynth>();
//   engine.play(bass);
//   bass->waveform = tokkel::BassSynth::Square;
//   bass->play_note(tokkel::note_frequency("E1"));
class BassSynth : public Synth {
public:
  enum Waveform { Sine, Triangle, Square, Saw };

  BassSynth();

  // The oscillator's shape; settable from any thread, any time.
  std::atomic<int> waveform{Triangle};

  // "Waveform": 0 sine, 1 triangle, 2 square, 3 saw; and Synth's own.
  void set_parameter(const std::string &name, float value) override;

protected:
  void note_on(float frequency, float amplitude, float sustain) override;
  void tick(float &left, float &right) override;

private:
  float oscillator();

  float frequency = 0.0f;
  float level = 0.0f;
  float phase = 0.0f; // 0 to 1
  float triangle = 0.0f;
  int hold = -1;  // frames until it is released; -1: released
  int age = 0;    // frames since the note started, for the filter sweep
  int retune = 0; // frames until the filter's cutoff is worked out again
  bool started_this_tick = false;

  stk::ADSR envelope;
  stk::BiQuad filter;
};

} // namespace tokkel
