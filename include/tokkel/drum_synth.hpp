#pragma once

#include "tokkel/synth.hpp"

#include <BiQuad.h>

#include <array>
#include <cstddef>

namespace tokkel {

// A drum kit without sample files. The note picks the drum, by its General
// MIDI drum number (as on MIDI channel 10):
//
//   C2 (36) and B1 (35): kick, a sine that drops from about 150 Hz to 48 Hz,
//                        with a short click on top
//   D2 (38), E2 (40), C#2 (37): snare, two drum-head tones under a burst of
//                        high-passed noise (the snare wires)
//   F#2 (42), G#2 (44): closed (and pedal) hi-hat, a short metallic tick
//   A#2 (46):            open hi-hat, the same metal ringing on; a closed
//                        hit chokes it, as the pedal does on a real kit
//
// The hi-hat's metal is the 808's: six square waves at clashing pitches,
// with some noise, high-passed so only the sizzle is left.
//
// Other notes are silent. `amplitude` is how hard it is hit; `sustain` is
// ignored, a drum rings as long as it rings.
// Create it after the Engine (it needs STK's sample rate).
//
//   auto drums = std::make_shared<tokkel::DrumSynth>();
//   engine.play(drums);
//   drums->play_note(tokkel::note_frequency("C2")); // kick
class DrumSynth : public Synth {
public:
  DrumSynth();

protected:
  void note_on(float frequency, float amplitude, float sustain) override;
  void tick(float &left, float &right) override;

private:
  // A hit that's sounding: `age` in frames since it was hit, -1 when idle.
  struct Hit {
    int age = -1;
    float level = 0.0f;
    float phase = 0.0f, second_phase = 0.0f;
  };

  float kick(Hit &hit);
  float snare_tone(Hit &hit);
  float hat_metal();

  // Two of each, so a quick second hit doesn't cut the first off mid-sound.
  std::array<Hit, 2> kicks, snares;
  std::size_t next_kick = 0, next_snare = 0;
  float click = 0.0f; // the kick's click, low-passed

  // The snare wires: noise through a high-pass and a gentle low-pass, with
  // its own noise per side for a little width.
  stk::BiQuad wires_high[2], wires_low[2];

  // One hi-hat: a new hit cuts off the last (a closed one chokes an open
  // one). `hat_open` is how the last was hit. The six squares' phases (0 to
  // 1) run on between hits, so no two hits start alike.
  Hit hat;
  bool hat_open = false;
  std::array<float, 6> hat_phases{};
  stk::BiQuad hat_high[2];
};

} // namespace tokkel
