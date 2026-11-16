# tokkel

A small C++17 audio library: [miniaudio](https://github.com/mackron/miniaudio)
for the device, mixing and file decoding, and [STK](https://github.com/thestk/stk)
for synthesis and DSP. It is being built to replace FMOD in the
[timeless](https://github.com/valentijnnieman/timeless) engine's sound system.

Both dependencies are git submodules under `vendor/`:

```bash
git submodule update --init --recursive
```

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build      # renders offline, needs no audio device
./build/tokkel_sine         # two seconds of A4 on the default device
```

As a subdirectory of another project, link `tokkel::tokkel`; the examples and
tests are then off unless `TOKKEL_BUILD_EXAMPLES` / `TOKKEL_BUILD_TESTS` are set.

## Use

```cpp
#include <tokkel/tokkel.hpp>
#include <tokkel/stk_source.hpp>
#include <SineWave.h>

tokkel::Engine engine;                 // opens the default device
engine.play_file("click.wav");         // fire-and-forget file playback

auto sine = std::make_shared<tokkel::StkSource<stk::SineWave>>();
sine->unit.setFrequency(440.0);
tokkel::VoiceId voice = engine.play(sine, 0.2f);
engine.stop(voice);

engine.add_instrument("Strings", std::make_shared<tokkel::StringSynth>()); // <tokkel/synth.hpp>
engine.play_note("Strings", tokkel::note_frequency("G2"));
```

A synth is an instrument. `add_instrument()` gives it to the engine under a
name: the engine plays it and keeps it for as long as it lives, and
`play_note()` plays a note on it by that name, from any thread; nothing has to
be stopped. `instrument(name)` hands it back to set its parameters. (A synth
can also be `play()`ed directly and its own `play_note()` called.) `tokkel::StringSynth`
is plucked strings. For other sounds, subclass `tokkel::Synth` (`note_on()`
starts a note, `tick()` gives the next stereo sample).

Every synth has its own reverb, `synth->reverb` (how long it rings is
`reverb / 20` seconds; 0, the default, is none). On top of that the engine
has a main reverb on the whole mix, `engine.set_reverb(0..1)`, which glides to
a new amount over about 50 ms. Everything `play()`, `add_instrument()` and
`add_sampler()` start goes through it; `play_file()` skips it.

`tokkel::PianoSynth` (`<tokkel/piano_synth.hpp>`) is a second synth, a
piano-like tone from pairs of strings behind a soft "hammer" low-pass.

`tokkel::GuitarSynth` (`<tokkel/guitar_synth.hpp>`) is an acoustic guitar:
strings (STK's Twang) plucked by a burst of noise that is brighter the harder
the note, two slightly detuned polarisations per string for a natural decay,
a fixed body of air and top-plate resonances, and strummed chords. Its
`sustain` is how long a note is held before it is damped.

`tokkel::BassSynth` (`<tokkel/bass_synth.hpp>`) is a synth bass: one
oscillator (`waveform`: `Sine`, `Triangle`, `Square` or `Saw`) through a
resonant low-pass that sweeps down after each note, an amp envelope and a
little drive. One note at a time: a new note takes over, and of notes started
together the lowest plays.

`tokkel::CelloSynth` (`<tokkel/cello_synth.hpp>`) is a cello: STK's Bowed
(a bowed string) with a slow bow attack, vibrato that sets in after a moment,
and a cello's body resonances. `sustain` is how long the note is bowed.

`tokkel::DrumSynth` (`<tokkel/drum_synth.hpp>`) is a drum kit, played by
General MIDI drum note as on MIDI channel 10: C2 (36) a kick (a sine with a
falling pitch and a beater click), D2 (38) a snare (two head tones under
high-passed noise), F#2 (42) and G#2 (44, softer) a closed hi-hat and A#2
(46) an open one (the 808's six detuned squares with noise, high-passed; a
closed hit chokes an open one). Other notes are silent.

`tokkel::Sampler` (`<tokkel/sampler.hpp>`) plays recorded sounds by name. An
event can have several variations, and triggering it plays a random one:

```cpp
auto sampler = engine.add_sampler("Sounds"); // made in the engine's format
sampler->add_file("Key Click", "key_click_1.wav");
sampler->add_file("Key Click", "key_click_2.wav");
engine.trigger("Sounds", "Key Click");
```

Like an instrument, the engine plays it and keeps it, under a name
`trigger()` reaches it by (instruments and samplers share the names), and
`sampler(name)` hands it back to add more sounds.

`tokkel::Source` is the interface for anything that produces audio; subclass
it for anything that isn't played note by note. `EngineConfig::offline` gives
an engine with no device whose mix is pulled with `Engine::render`.

## Notes

- STK is compiled without its realtime classes (RtAudio, RtMidi, sockets,
  threads): miniaudio owns the device.
- STK instruments that read wave tables (`Rhodey`, `Wurley`, ...) need
  `stk::Stk::setRawwavePath()` pointed at `vendor/stk/rawwaves/` or a copy of it.
