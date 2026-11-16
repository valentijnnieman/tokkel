#pragma once

#include "tokkel/synth.hpp"

#include <array>
#include <cstddef>

namespace tokkel {

// A simple bell: a square wave that starts at once and dies away, like a
// chiptune chime. Create it after the Engine (it needs STK's sample rate).
//
//   auto bell = std::make_shared<tokkel::BellSynth>();
//   engine.play(bell);
//   bell->play_note(tokkel::note_frequency("D5"));
class BellSynth : public Synth {
public:
  BellSynth() = default;

protected:
  // `sustain` scales how long the note rings: 1 is about a second and a half.
  void note_on(float frequency, float amplitude, float sustain) override;
  void tick(float &left, float &right) override;

private:
  struct Voice {
    double phase = 0.0;     // through the cycle, 0 to 1
    double step = 0.0;      // phase per sample
    double level = 0.0;     // falls by `decay` every sample
    double decay = 0.0;
    float left = 1.0f, right = 1.0f; // where it sits (Synth::pan_gains())
  };

  // When all are ringing, a new note takes over the one struck longest ago.
  std::array<Voice, 8> voices;
  std::size_t next_voice = 0;
};

} // namespace tokkel
