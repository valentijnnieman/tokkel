#pragma once

#include "tokkel/synth.hpp"

#include <ADSR.h>
#include <BiQuad.h>
#include <StifKarp.h>

#include <array>
#include <cstddef>

namespace tokkel {

// A piano-like tone without sample files: each note is a pair of strings
// (STK's StifKarp) a few cents apart, as a piano has more than one string per
// key, behind a low-pass that stands in for the soft hammer.
// Create it after the Engine (it needs STK's sample rate).
//
//   auto piano = std::make_shared<tokkel::PianoSynth>();
//   engine.play(piano);
//   piano->play_note(tokkel::note_frequency("D4"));
class PianoSynth : public Synth {
public:
  PianoSynth();

protected:
  // `sustain` scales how long the note rings: 1 is about three seconds for a
  // low note, shorter the higher it is.
  void note_on(float frequency, float amplitude, float sustain) override;
  void tick(float &left, float &right) override;

private:
  struct Voice {
    stk::StifKarp first, second;
    stk::BiQuad hammer; // low-pass: how soft the attack is
    stk::ADSR envelope;
    bool sounding = false; // struck and not yet silent: only then is it computed
  };

  // When all are ringing, a new note takes over the one played longest ago.
  std::array<Voice, 8> voices;
  std::size_t next_voice = 0;
};

} // namespace tokkel
