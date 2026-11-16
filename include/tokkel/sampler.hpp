#pragma once

#include "tokkel/tokkel.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace tokkel {

// A Instrument that plays recorded sounds by name. An event has one or more
// variations (audio files); triggering it plays one of them, picked at
// random, so a sound that repeats a lot doesn't sound identical every time.
// Start it once with Engine::play(); nothing has to be stopped.
//
//   auto sampler = std::make_shared<tokkel::Sampler>(engine.sample_rate(), engine.channels());
//   sampler->add_file("Key Click", "key_click_1.wav");
//   sampler->add_file("Key Click", "key_click_2.wav");
//   engine.play(sampler);
//   sampler->trigger("Key Click");
class Sampler : public Instrument {
public:
  // The format of the engine that will play it: files are converted to it
  // when they are added.
  Sampler(std::uint32_t sample_rate, std::uint32_t channels);

  // Add one variation to `event`, from an audio file (wav, flac, mp3) on disk
  // or already in memory. False when it can't be decoded. Callable from any
  // thread, also while the sampler plays, but it decodes the whole file: do
  // it at load time.
  bool add_file(const std::string &event, const std::string &path);
  bool add(const std::string &event, const void *data, std::size_t size);

  // Play one of `event`'s variations at `volume`: a random one, and with more
  // than one never the same twice in a row. False when the event has no
  // variations. Callable from any thread; the sound starts with the next
  // block of audio.
  //
  // With `restart`, what the event was still playing is cut off (faded out in
  // a few milliseconds) when this one starts, so a sound triggered in quick
  // succession, like a key click while typing, doesn't pile up on its own
  // tails. Without it every trigger plays to its end and they overlap.
  // Restarts of one event that arrive before its next block of audio count as
  // one (the last), as an FMOD event instance started twice in one update.
  //
  // `delay`: start that many seconds from the next block of audio.
  bool trigger(const std::string &event, float volume = 1.0f, bool restart = false,
               float delay = 0.0f);
  // trigger() from `position` in the world: while it plays, its volume and
  // pan follow the listener (set_listener()), block by block, so it stays put
  // as the listener moves. Out of hearing it still plays, silently, and comes
  // in if the listener gets near.
  bool trigger_at(const std::string &event, const Vec3 &position, float volume = 1.0f,
                  bool restart = false, float delay = 0.0f);

  // Stop what `event` plays (faded out in a few milliseconds, as a restart
  // cuts it), and drop what it was about to: its delayed sounds, and
  // triggers since the last block. Callable from any thread.
  void stop(const std::string &event);
  // stop() every event.
  void stop_all();

  // Where trigger_at()'s sounds are heard from. Callable from any thread; the
  // audio thread picks it up with its next block.
  void set_listener(const Listener &listener);

  void render(float *out, std::uint32_t frames, std::uint32_t channels) override;

private:
  // A decoded file: interleaved frames in the sampler's format.
  struct Sample {
    std::vector<float> frames;
    std::size_t frame_count = 0;
  };
  struct Event {
    std::vector<std::shared_ptr<const Sample>> variations;
    std::size_t last = 0; // the variation played last
    std::uint32_t id = 0; // which event a playing sound belongs to
  };
  struct Playing {
    std::shared_ptr<const Sample> sample; // empty = a free voice
    std::size_t position = 0;             // the next frame to play
    float volume = 1.0f;
    std::uint32_t event = 0;
    std::size_t start_in = 0; // frames until it starts
    bool restart = false;     // starting it cuts the event's other sounds off
    bool cut = false;     // being cut off: `fade` runs down to 0
    float fade = 1.0f;
    // trigger_at(): where it plays from, and the left and right gains it
    // ended the last block on (each block glides to its new ones).
    bool placed = false;
    Vec3 at;
    float left = 1.0f, right = 1.0f;
    bool gains_set = false; // left/right hold a block's gains yet
  };

  bool start(Playing start, const std::string &event);
  bool add_decoded(const std::string &event, const float *frames, std::uint64_t frame_count);
  // The left and right gains of a sound at `at` under `listener`.
  static void placed_gains(const Listener &listener, const Vec3 &at, float &left, float &right);

  const std::uint32_t sample_rate;
  const std::uint32_t sample_channels;

  // The events and what picks their variations: never touched by the audio
  // thread. The events keep every Sample alive for as long as the sampler
  // lives, so the audio thread never frees one.
  std::mutex events_mutex;
  std::unordered_map<std::string, Event> events;
  std::uint32_t next_event_id = 1;
  std::minstd_rand random;

  // Sounds triggered but not started yet: trigger() adds them, render()
  // starts them on the audio thread.
  std::mutex waiting_mutex;
  std::vector<Playing> waiting;
  // Events stop() asked to stop (0: all of them), for render() to cut off
  // before it starts the waiting sounds. Under waiting_mutex.
  std::vector<std::uint32_t> stopping;

  // The listener as set_listener() left it, for the audio thread to copy.
  // Under listener_mutex, which the audio thread only try_locks: when it's
  // busy, that block keeps the copy it had.
  std::mutex listener_mutex;
  Listener listener_shared;
  Listener listener_audio; // audio thread only

  // Audio thread only. When all are busy, a new sound takes over the voice
  // closest to its end.
  std::array<Playing, 32> voices;
};

} // namespace tokkel
