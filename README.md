# tokkel

A small C++17 audio library for games: synthesized instruments you play by
note, sampled sound effects you trigger by name, and sounds placed in the world
around a listener. It builds on [miniaudio](https://github.com/mackron/miniaudio)
(the audio device, mixing and file decoding) and
[STK](https://github.com/thestk/stk) (synthesis and DSP), and runs wherever
miniaudio does, including the browser through Emscripten.

```cpp
tokkel::Engine engine;
engine.add_instrument("Piano", std::make_shared<tokkel::PianoSynth>());
engine.play_note("Piano", tokkel::note_frequency("C4"));
```

> tokkel is young (0.1): its API may still change between minor versions.

## Features

- **Instruments by name.** Hand the engine a synth once, then play notes on it
  by name from any thread. Nothing has to be stopped afterwards.
- **Built-in synths:** plucked strings, piano, acoustic guitar, synth bass,
  cello, a chiptune bell and a drum kit played by General MIDI drum note.
- **A sampler** for sound effects: an event can have several recorded
  variations, and triggering it plays a random one.
- **Sound in the world:** notes and samples placed at a position are panned and
  attenuated relative to a listener you move with the camera.
- **Reverb** per instrument and on the whole mix, and parameters (volume, pan,
  reverb and a synth's own) set by name.
- **Offline rendering:** an engine without an audio device, whose mix you pull
  yourself, for tests, tools and rendering to a file.
- **Your own sounds:** subclass `tokkel::Synth` for something played by note,
  or `tokkel::Instrument` for anything else that produces audio, or play any of
  STK's generators and instruments as they are.

## Getting it

tokkel's dependencies are git submodules, so clone it with them:

```bash
git clone --recursive https://github.com/valentijnnieman/tokkel.git
# or, in an existing clone:
git submodule update --init --recursive
```

### Building on its own

Needs CMake 3.20 or newer and a C++17 compiler.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build   # renders offline, so it needs no audio device
```

### In your project

Add it as a subdirectory and link `tokkel::tokkel`:

```cmake
add_subdirectory(vendor/tokkel)
target_link_libraries(my_game PRIVATE tokkel::tokkel)
```

Built this way, its examples and tests are off unless you set
`TOKKEL_BUILD_EXAMPLES` or `TOKKEL_BUILD_TESTS`.

## Usage

### The engine

`tokkel::Engine` opens the default audio device and owns the mix. Make it
before any other tokkel or STK object: it sets STK's sample rate to its own.

```cpp
#include <tokkel/tokkel.hpp>

tokkel::Engine engine;   // or Engine(tokkel::EngineConfig{...})
if (!engine.ok()) {
  // no audio device: every call is now a harmless no-op
}
engine.set_master_volume(0.8f);
engine.play_file("click.wav");   // fire and forget: wav, flac or mp3
```

`EngineConfig` sets the sample rate (default 48000), the number of channels
(default 2), and `offline` (see [Offline rendering](#offline-rendering)).

### Instruments

Give the engine a synth under a name; it plays it and keeps it for as long as
it lives. `play_note()` then plays a note on it, from any thread.

```cpp
#include <tokkel/guitar_synth.hpp>

engine.add_instrument("Guitar", std::make_shared<tokkel::GuitarSynth>());

//               name      frequency in Hz                    amplitude  sustain  delay (s)
engine.play_note("Guitar", tokkel::note_frequency("E2"));
engine.play_note("Guitar", tokkel::note_frequency("B2"),      0.7f,      0.95f,   0.25f);
```

`note_frequency()` (in `<tokkel/synth.hpp>`) turns a note name like `"G#3"`
into Hz. `amplitude` runs from 0 to 1, `delay` starts the note that many
seconds from now, and `sustain` is up to the instrument: for most, how long the
note is held.

| Synth | Header | Sound |
|---|---|---|
| `StringSynth` | `<tokkel/synth.hpp>` | Plucked strings. |
| `PianoSynth` | `<tokkel/piano_synth.hpp>` | A piano-like tone: pairs of strings behind a soft hammer. |
| `GuitarSynth` | `<tokkel/guitar_synth.hpp>` | An acoustic guitar: plucked strings, brighter the harder they're played, through a guitar body. `sustain` is how long a note rings before it's damped. |
| `BassSynth` | `<tokkel/bass_synth.hpp>` | A synth bass: one oscillator (`waveform`: `Sine`, `Triangle`, `Square` or `Saw`) through a resonant low-pass that sweeps down after each note. One note at a time; of notes started together, the lowest plays. |
| `CelloSynth` | `<tokkel/cello_synth.hpp>` | A bowed cello, with a slow bow attack and vibrato that sets in after a moment. `sustain` is how long the note is bowed. |
| `BellSynth` | `<tokkel/bell_synth.hpp>` | A chiptune chime: a square wave that dies away. `sustain` 1 rings about a second and a half. |
| `DrumSynth` | `<tokkel/drum_synth.hpp>` | A drum kit, by General MIDI drum note (as on MIDI channel 10): C2 kick, D2 snare, F#2 closed hi-hat, G#2 a softer closed hi-hat, A#2 open hi-hat (a closed hit chokes it). Other notes are silent. |

`engine.instrument(name)` hands an instrument back so you can change it, and
`engine.instruments()` lists their names.

### Sound effects

A sampler plays recorded sounds by event name. Give an event several
variations, and each trigger picks one at random:

```cpp
#include <tokkel/sampler.hpp>

auto sounds = engine.add_sampler("Sounds");
sounds->add_file("Footstep", "footstep_1.wav");
sounds->add_file("Footstep", "footstep_2.wav");

engine.trigger("Sounds", "Footstep");
engine.trigger("Sounds", "Footstep", 0.5f);   // at half volume
engine.stop_event("Sounds", "Footstep");
```

Instruments and samplers share one set of names: adding something under a
name that's taken replaces (and stops) what had it.

### Sound in the world

Set where the listener is, and play notes and sounds at a position, in your
game's own units. A sound is at full volume up to the listener's
`min_distance`, fades out towards `max_distance` and is silent beyond it, and
is panned by how far it lies along the listener's `right`.

```cpp
tokkel::Listener listener;
listener.position = {0.0f, 0.0f, 0.0f};
listener.right = {1.0f, 0.0f, 0.0f};   // the direction that pans fully right
listener.max_distance = 50.0f;
engine.set_listener(listener);

engine.trigger_at("Sounds", "Footstep", tokkel::Vec3{4.0f, 0.0f, 2.0f});

engine.add_instrument("Bell", std::make_shared<tokkel::BellSynth>());
engine.play_note_at("Bell", tokkel::Vec3{-3.0f, 0.0f, 0.0f}, tokkel::note_frequency("D5"));
```

Samples follow the listener while they play, so a moving camera keeps them in
place. A note played at a position is quieter with distance on every synth;
only synths that place their notes in stereo, like the `BellSynth`, are panned
too.

### Reverb and parameters

Every synth has its own reverb, and the engine has one on the whole mix, which
glides to a new amount over about 50 ms:

```cpp
engine.set_reverb(0.3f);                         // 0 (none) to 1 (a big room)
engine.set_parameter("Guitar", "Reverb", 20.0f); // rings for reverb / 20 seconds
engine.set_parameter("Guitar", "Volume", 0.5f);
```

Every synth knows `"Reverb"`, `"Volume"` and `"Pan"`; a synth can add its own.
Names a synth doesn't know are ignored, so one parameter can be sent to every
instrument and only those that use it change. `play_file()` skips the main
reverb; everything else goes through it.

### Your own synth

Subclass `tokkel::Synth`: `note_on()` starts a note and `tick()` gives the next
stereo sample. Both run on the audio thread, so don't block, allocate or lock
in them.

```cpp
class MySynth : public tokkel::Synth {
protected:
  void note_on(float frequency, float amplitude, float sustain) override;
  void tick(float &left, float &right) override;
};
```

A synth rests when it's done: once its sound (with its reverb) has been
silent for 0.2 s and no note is waiting, `tick()` isn't called until the next
note starts, so a quiet instrument costs nothing. If your synth holds notes
back itself (strums its strings one after another, say), override
`holding_notes()` to return true while it does, or it may rest before they
start. Inside `tick()`, skip voices that have gone silent: the built-in synths
do, which is most of what they save.

For audio that isn't played by note, subclass `tokkel::Instrument` and fill
the buffer in `render()`, then `engine.play()` it (and `engine.stop()` it with
the id that returns).

### STK objects

Any mono STK object with a `tick()` (generators like `stk::SineWave` or
`stk::Noise`, instruments like `stk::Plucked`) plays as is, wrapped in a
`tokkel::StkInstrument`:

```cpp
#include <tokkel/stk_instrument.hpp>
#include <SineWave.h>

auto sine = std::make_shared<tokkel::StkInstrument<stk::SineWave>>();
sine->unit.setFrequency(440.0);
tokkel::VoiceId voice = engine.play(sine, 0.2f);
// ...
engine.stop(voice);
```

`unit` is the STK object. It's read on the audio thread, so changing it while
it plays is only as safe as that STK class makes it (setting a parameter is
usually a plain write).

### Offline rendering

With `EngineConfig::offline`, the engine opens no audio device. Nothing plays;
instead you pull the mix with `Engine::render()`, as interleaved floats:

```cpp
tokkel::EngineConfig config;
config.offline = true;
tokkel::Engine engine(config);
// ... add instruments, play notes ...
std::vector<float> buffer(48000 * 2);
engine.render(buffer.data(), 48000);   // one second, two channels
```

### Threading

Playing notes, triggering sounds, setting parameters, the reverb and the
listener are all safe from any thread. The mixing itself runs on the audio
device's thread.

## Notes

- STK is compiled without its realtime classes (RtAudio, RtMidi, sockets,
  threads): miniaudio owns the device.
- STK instruments that read wave tables (`Rhodey`, `Wurley`, ...) need
  `stk::Stk::setRawwavePath()` pointed at `vendor/stk/rawwaves/` or a copy of
  it.

## Used by

- [Timeless](https://github.com/valentijnnieman/timeless), a C++ game engine,
  for its `SoundSystem`.

## Versioning

tokkel follows [Semantic Versioning](https://semver.org/): below 1.0, a
breaking change raises the minor version (0.1 → 0.2), anything else the patch
version. Releases are tagged in git (`v0.1.0`).

## License

tokkel is released under the [MIT License](LICENSE). miniaudio (public domain
or MIT-0) and STK (its own MIT-style license) keep theirs, in `vendor/`.
