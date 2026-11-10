#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace tokkel {

// Something that produces audio: a synth voice, an effect chain, a generator.
// Subclass it directly, or wrap an STK object with tokkel/stk_source.hpp.
class Source {
public:
  virtual ~Source() = default;

  // Fill `out` with `frames` frames of interleaved float samples in [-1, 1].
  // Called on the audio thread (or from Engine::render on an offline engine):
  // don't block, allocate or lock in here.
  virtual void render(float *out, std::uint32_t frames, std::uint32_t channels) = 0;
};

struct EngineConfig {
  std::uint32_t sample_rate = 48000;
  std::uint32_t channels = 2;
  // No audio device: nothing plays, and the mix is pulled with Engine::render
  // instead. For tests, tools and rendering to a file.
  bool offline = false;
};

// Identifies a playing Source. 0 is never a valid voice.
using VoiceId = std::uint32_t;

// Owns the audio device and the mix. Also sets STK's global sample rate to
// the engine's, so create the Engine before any STK object.
class Engine {
public:
  explicit Engine(const EngineConfig &config = {});
  ~Engine();
  Engine(const Engine &) = delete;
  Engine &operator=(const Engine &) = delete;

  // False when the device or engine failed to start; every call is then a no-op.
  bool ok() const;
  std::uint32_t sample_rate() const;
  std::uint32_t channels() const;

  void set_master_volume(float volume);

  // Fire-and-forget playback of an audio file (wav, flac, mp3).
  bool play_file(const std::string &path);

  // Start mixing a Source. The engine shares ownership until stop().
  VoiceId play(std::shared_ptr<Source> source, float volume = 1.0f);
  void set_volume(VoiceId voice, float volume);
  void stop(VoiceId voice);

  // Offline engines only: mix the next `frames` frames into `out`
  // (interleaved, channels() wide). Returns the frames written.
  std::uint32_t render(float *out, std::uint32_t frames);

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace tokkel
