#pragma once

#include "tokkel/synth.hpp"

#include <BiQuad.h>
#include <Bowed.h>

#include <array>
#include <cstddef>

namespace tokkel {

// A cello: STK's Bowed, a bow dragging a string (the stick-slip friction that
// gives a bowed string its sustained, slightly raspy tone), with:
//
// - a slow bow attack, faster the harder the note, and a soft release when
//   the bow lifts after `sustain` seconds;
// - vibrato that sets in a moment after the note starts, as a cellist's does;
// - a cello's body on top of Bowed's own (a violin's): low air and wood
//   resonances, and a darker top end.
//
// Four voices, one per string: notes can sound together (double stops), and
// a fifth takes over the one bowed longest ago.
// Create it after the Engine (it needs STK's sample rate).
//
//   auto cello = std::make_shared<tokkel::CelloSynth>();
//   engine.play(cello);
//   cello->play_note(tokkel::note_frequency("C2"), 0.8f, 2.0f);
class CelloSynth : public Synth {
public:
  CelloSynth();

protected:
  // `sustain`: seconds the note is bowed before the bow lifts.
  void note_on(float frequency, float amplitude, float sustain) override;
  void tick(float &left, float &right) override;

private:
  struct Voice {
    stk::Bowed string{20.0}; // its lowest note, Hz
    int age = -1;  // frames since the note started; -1: never played
    int hold = -1; // frames until the bow lifts; -1: lifted
    int quiet = 0; // frames it has been silent since the bow lifted
  };

  std::array<Voice, 4> voices;
  std::size_t next_voice = 0;
  stk::BiQuad air, wood, tone; // the cello's body, and its darker top
};

} // namespace tokkel
