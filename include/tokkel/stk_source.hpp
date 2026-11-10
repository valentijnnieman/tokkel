#pragma once

#include "tokkel/tokkel.hpp"

#include <Stk.h>

#include <utility>

namespace tokkel {

// Plays any mono STK object with a `tick()` that returns one sample
// (generators such as stk::SineWave or stk::Noise, and instruments such as
// stk::Plucked), copying it to every output channel.
//
//   auto sine = std::make_shared<tokkel::StkSource<stk::SineWave>>();
//   sine->unit.setFrequency(440.0);
//   engine.play(sine);
//
// `unit` is read on the audio thread: changing it while it plays is only as
// safe as the STK class makes it (a parameter set is usually a plain write).
template <typename Unit> class StkSource : public Source {
public:
  template <typename... Args>
  explicit StkSource(Args &&...args) : unit(std::forward<Args>(args)...) {}

  void render(float *out, std::uint32_t frames, std::uint32_t channels) override {
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
      const float sample = static_cast<float>(unit.tick());
      for (std::uint32_t channel = 0; channel < channels; ++channel)
        *out++ = sample;
    }
  }

  Unit unit;
};

} // namespace tokkel
