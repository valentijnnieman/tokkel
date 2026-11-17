// Plays a two-second A4 through the default audio device.
#include "tokkel/stk_instrument.hpp"
#include "tokkel/tokkel.hpp"

#include <SineWave.h>

#include <chrono>
#include <cstdio>
#include <thread>

int main() {
  tokkel::Engine engine;
  if (!engine.ok()) {
    std::fprintf(stderr, "tokkel: could not open an audio device\n");
    return 1;
  }

  auto sine = std::make_shared<tokkel::StkInstrument<stk::SineWave>>();
  sine->unit.setFrequency(440.0);

  const tokkel::VoiceId voice = engine.play(sine, 0.2f);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  engine.stop(voice);
  return 0;
}
