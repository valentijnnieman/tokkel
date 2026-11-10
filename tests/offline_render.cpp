// Renders an STK sine through an offline engine (no audio device) and checks
// that the mix carries it at the expected level and pitch, then goes silent
// once the voice is stopped.
#include "tokkel/stk_source.hpp"
#include "tokkel/tokkel.hpp"

#include <SineWave.h>

#include <cmath>
#include <cstdio>
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

  auto sine = std::make_shared<tokkel::StkSource<stk::SineWave>>();
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

  return failures == 0 ? 0 : 1;
}
