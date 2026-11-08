# havOPL

Havoc's Yamaha OPL2/OPL3 FM synthesis library for C++.

## Table of Contents

- [Features](#features)
- [Getting Started](#getting-started)
- [Usage](#usage)
- [Documentation](#documentation)
- [Contributing](#contributing)
- [License](#license)

## Features

- Register-driven OPL2 (YM3812) and OPL3 (YMF262) synthesis: you write registers, it renders interleaved 16-bit stereo
  - 9 channels in OPL2 mode, 18 in OPL3 mode, four-operator pairs, rhythm mode
  - All envelope, pitch, level, feedback, tremolo/vibrato and waveform controls
  - OPL3 output routing
- Any output rate from 8,000 to 192,000 Hz with exact timing across arbitrary buffer sizes
- The two timers and the status register for drivers that detect the chip by polling them
- Written from Yamaha's hardware documentation: no code or ROM tables from other emulators

## Getting Started

This library requires C++20. It has been tested under Windows and Linux.

### Installation

Add the folder to your CMake project and link the target:

```cmake
add_subdirectory(path/to/havOPL ${CMAKE_BINARY_DIR}/havOPL)
target_link_libraries(your_target PRIVATE havOPL::havOPL)
```

Or compile `havOPL/havOPL.cpp` with your sources and add the `havOPL` folder to the include path.

The tests build when havOPL is the top-level project (`HAVOPL_BUILD_TESTS`). Presets are provided for Visual Studio 2022 (`msvc` with `msvc-debug` / `msvc-release`), MinGW (`mingw-debug` / `mingw-release`) and Linux (`linux-debug` / `linux-release`):

```sh
cmake --preset msvc
cmake --build --preset msvc-debug
ctest --preset msvc-debug
```

The `.vscode` folder has matching build, test and debug configurations for VS Code with the CMake Tools and C/C++ extensions.

## Usage

```cpp
#include "havOPL.hpp"

#include <cstdint>
#include <vector>

int main()
{
  havOPL::Chip chip(48000); // Output rate in Hz

  chip.Write(0x20, 0x01);   // Modulator: multiplier 1
  chip.Write(0x23, 0x01);   // Carrier: multiplier 1
  chip.Write(0x40, 0x10);   // Modulator level
  chip.Write(0x43, 0x00);   // Carrier level: loudest
  chip.Write(0x60, 0xF0);   // Attack 15, decay 0 (both operators)
  chip.Write(0x63, 0xF0);
  chip.Write(0x80, 0x0F);   // Sustain 0, release 15
  chip.Write(0x83, 0x0F);
  chip.Write(0xA0, 0x44);   // F-number low byte
  chip.Write(0xB0, 0x32);   // Key on, block 4, F-number high bits

  std::vector<std::int16_t> pcm(48000 * 2); // One second, left/right interleaved
  chip.Render(pcm);

  // The buffer now holds one second of audio for your audio output or a WAV writer

  return 0;
}
```

Registers `0x000`-`0x0FF` are the first bank, `0x100`-`0x1FF` the second (OPL3, enabled by setting bit 0 of `0x105`).

The timers: `Status()` reads the status register (bit 7 IRQ, bit 6 timer 1, bit 5 timer 2). Rendering advances them. `Advance(masterClocks)` advances them without rendering, in 14.31818 MHz clocks, for hosts that poll between renders.

`Reset()` returns the chip to power-on state. Invalid arguments (a register above `0x1FF`, a sample rate out of range, an odd sample count) throw `std::runtime_error`.

## Documentation

- [The synthesis model, its references and its fidelity limits](docs/emulation.md)
- [How the instrument balance was calibrated](docs/calibration.md)

## Contributing

Thank you for your interest! Suggestions for features and bug reports are always welcome via [issues](https://github.com/Havoc7891/havOPL/issues).

To maintain a consistent design and quality for this library, changes are implemented by the maintainer rather than via direct pull requests.

## License

Copyright © 2026 René Nicolaus

This library is released under the [MIT license](/LICENSE).
