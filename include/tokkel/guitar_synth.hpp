#pragma once

#include "tokkel/synth.hpp"

#include <BiQuad.h>
#include <OnePole.h>
#include <Twang.h>

#include <array>
#include <cstddef>

namespace tokkel {

// An acoustic guitar without sample files. Each note is a string (STK's
// Twang, a plucked string whose pluck we make ourselves) that:
//
// - is plucked by a short burst of noise, darker the softer the note: a
//   harder pluck is brighter as well as louder;
// - vibrates in two directions at once, slightly apart in pitch and decaying
//   at different rates, as a real string does: a loud start that falls off
//   quickly, then a quieter tail that rings on;
// - loses its high overtones faster than its low ones, so it starts bright
//   and mellows as it rings;
// - rings for `sustain` seconds, then is damped, like lifting the finger.
//
// All strings then sound through one fixed guitar body (its air and top-plate
// resonances), slightly different per channel, like two microphones.
// Notes started together are strummed, a few ms apart, low to high in the
// order they were played.
// Create it after the Engine (it needs STK's sample rate).
//
//   auto guitar = std::make_shared<tokkel::GuitarSynth>();
//   engine.play(guitar);
//   guitar->play_note(tokkel::note_frequency("E2"));
class GuitarSynth : public Synth {
public:
  GuitarSynth();

protected:
  // `sustain`: seconds the note is held before it is damped.
  void note_on(float frequency, float amplitude, float sustain) override;
  void tick(float &left, float &right) override;
  bool holding_notes() const override;

private:
  struct Voice {
    stk::Twang vertical, horizontal; // the string's two directions
    stk::OnePole pick;               // the pluck's brightness
    float frequency = 0.0f;
    float level = 0.0f;
    int start_in = -1;   // frames until the pluck; -1: nothing to pluck
    int excite_left = 0; // frames of the pluck still to feed into the string
    int excite_length = 0;
    int hold = -1;       // frames until it is damped; -1: already damped
    bool sounding = false; // played and not yet silent: only then is it computed
    int quiet = 0;       // frames it has been silent since it was damped
  };
  // One side's body: parallel resonances added to the strings.
  struct Body {
    std::array<stk::BiQuad, 5> resonances;
    std::array<float, 5> gains{};
    float tick(float in);
  };

  void damp(Voice &voice);

  // When all are ringing, a new note takes over the one played longest ago.
  std::array<Voice, 12> voices;
  std::size_t next_voice = 0;
  int strum = 0; // notes started since the last tick()
  Body left_body, right_body;
  float pick_noise = 0.0f; // the pick's own click, fading out
  float last_noise = 0.0f;

  stk::BiQuad lp_filter;
};

} // namespace tokkel
