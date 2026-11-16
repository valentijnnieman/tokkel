#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tokkel {

class Sampler;
class Synth;

// Something that produces audio: a synth voice, an effect chain, a generator.
// Subclass it directly, or wrap an STK object with tokkel/stk_instrument.hpp.
class Instrument {
public:
  virtual ~Instrument() = default;

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

// Identifies a playing Instrument. 0 is never a valid voice.
using VoiceId = std::uint32_t;

// A point in the game's world, in its own units.
struct Vec3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
};

// Where sounds placed in the world are heard from (Engine::set_listener()).
// A sound at `position` comes in at its full volume up to `min_distance` away, fades
// out between `min_distance` and `max_distance`, and is silent from `max_distance` on. It's panned by
// how far it lies along `right` (normalised here; zero means no panning),
// all the way to one side at `max_distance`.
struct Listener {
  Vec3 position;
  Vec3 right{1.0f, 0.0f, 0.0f};
  float min_distance = 1.0f;
  float max_distance = 100.0f;

  // The volume a sound at `at` gets: 1 up to `min_distance`, to 0 at `max_distance`.
  float gain(const Vec3 &at) const;
  // Where a sound at `at` sits from left (-1) to right (1).
  float pan(const Vec3 &at) const;
};

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

  // The main reverb, on everything play(), add_instrument() and
  // add_sampler() play together (on top of an instrument's own reverb):
  // 0 (none, the default) to 1 (a big room, heard well). Changes glide in
  // over about 50 ms. Callable from any thread.
  void set_reverb(float amount);

  // Fire-and-forget playback of an audio file (wav, flac, mp3). It skips
  // the main reverb.
  bool play_file(const std::string &path);

  // Start mixing a Instrument. The engine shares ownership until stop().
  VoiceId play(std::shared_ptr<Instrument> instrument, float volume = 1.0f);
  void set_volume(VoiceId voice, float volume);
  void stop(VoiceId voice);

  // Instruments and samplers by name: the engine plays them and keeps them
  // for as long as it lives, and play_note() and trigger() reach them by
  // name. Instruments and samplers share the names: adding under a name
  // that's taken replaces what had it, which stops.

  // Give the engine an instrument (a Synth) under `name`. False on an
  // engine that isn't ok().
  //
  //   engine.add_instrument("GuitarSynth", std::make_shared<tokkel::GuitarSynth>());
  //   engine.play_note("GuitarSynth", tokkel::note_frequency("E2"));
  bool add_instrument(const std::string &name, std::shared_ptr<Instrument> instrument);
  // The instrument added under `name`, to set its parameters; null if none.
  std::shared_ptr<Instrument> instrument(const std::string &name) const;
  // The names of all instruments added (not samplers).
  std::vector<std::string> instruments() const;
  // Set a parameter on the instrument added under `instrument` (see
  // Synth::set_parameter()). False when there is no such instrument.
  bool set_parameter(const std::string &instrument, const std::string &name, float value);
  // Play a note on the instrument added under `name`: `frequency` in Hz (see
  // note_frequency() in tokkel/synth.hpp), `amplitude` in [0, 1], `sustain`
  // as that instrument reads it, `delay` seconds from now. Callable from any
  // thread. False, and silent, when there is no such instrument.
  bool play_note(const std::string &name, float frequency, float amplitude = 1.0f,
                 float sustain = 0.95f, float delay = 0.0f, float speed = 1.0f);
  // play_note() from `position` in the world: its amplitude scaled by the
  // listener's gain() there when it starts, and its pan() passed on to the
  // synth (Synth::note_pan(); only synths that place their notes, like the
  // BellSynth, use it). True, and silent, when that's out of hearing.
  bool play_note_at(const std::string &name, const Vec3 &position, float frequency,
                    float amplitude = 1.0f, float sustain = 0.95f, float delay = 0.0f);

  // Where sounds placed in the world (trigger_at(), play_note_at()) are heard
  // from. Every sampler follows it from its next block of audio, also with
  // sounds already playing, so a moving listener keeps them in place.
  // Callable from any thread.
  void set_listener(const Listener &listener);
  Listener listener() const;

  // A new sampler under `name`, in the engine's format: add its sounds with
  // add()/add_file(), play them with trigger(). Null on an engine that isn't
  // ok().
  //
  //   engine.add_sampler("Sounds")->add_file("Key Click", "key_click_1.wav");
  //   engine.trigger("Sounds", "Key Click");
  std::shared_ptr<Sampler> add_sampler(const std::string &name);
  // The sampler added under `name`, to add sounds to; null if none.
  std::shared_ptr<Sampler> sampler(const std::string &name) const;
  // Play one of `event`'s sounds on the sampler added under `name` (see
  // Sampler::trigger() for `volume`, `restart` and `delay`). Callable from
  // any thread. False, and silent, when there is no such sampler or it has
  // no such event.
  bool trigger(const std::string &name, const std::string &event, float volume = 1.0f,
               bool restart = false, float delay = 0.0f);
  // trigger() from `position` in the world (see Sampler::trigger_at()).
  bool trigger_at(const std::string &name, const std::string &event, const Vec3 &position,
                  float volume = 1.0f, bool restart = false, float delay = 0.0f);
  // Stop what `event` plays on the sampler added under `name`, and what it
  // was about to (see Sampler::stop()). False when there's no such sampler.
  bool stop_event(const std::string &name, const std::string &event);
  // Stop everything the sampler added under `name` plays.
  bool stop_events(const std::string &name);

  // Offline engines only: mix the next `frames` frames into `out`
  // (interleaved, channels() wide). Returns the frames written.
  std::uint32_t render(float *out, std::uint32_t frames);

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace tokkel
