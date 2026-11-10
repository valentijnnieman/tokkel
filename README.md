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
```

`tokkel::Source` is the interface for anything that produces audio; subclass
it for custom synths. `EngineConfig::offline` gives an engine with no device
whose mix is pulled with `Engine::render`.

## Notes

- STK is compiled without its realtime classes (RtAudio, RtMidi, sockets,
  threads): miniaudio owns the device.
- STK instruments that read wave tables (`Rhodey`, `Wurley`, ...) need
  `stk::Stk::setRawwavePath()` pointed at `vendor/stk/rawwaves/` or a copy of it.
