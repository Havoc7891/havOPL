# Original OPL synthesis core

`havOPL/havOPL.hpp` and `havOPL/havOPL.cpp` implement havOPL's own register-driven
FM synthesizer. The architecture, envelope equations, waveform construction,
noise generator, and resampler were written from Yamaha's hardware documentation
and ordinary signal-processing mathematics. No code or ROM tables from other
emulators were used. Behavioral calibration also compares black-box recordings
from DOSBox's OPL emulation with havOPL's output for the same register writes.
See [OPL calibration](calibration.md) for the procedure, measurements and
regression tests.

The hardware references are original Yamaha publications, available as archived
scans:

- [YMF262, FM Operator Type L3 (OPL3), October 1991, LSI-4MF2622](https://archive.decromancer.ca/bitsavers.org/components/yamaha/YMF262_199110.pdf):
  register maps and operator/channel allocation on pages 6-7, pitch, rate, level,
  and modulation controls on pages 8-12, four-operator connections on page 13,
  and eight waveforms, output routing, and pairing on page 14.
- [YM3812 Application Manual, June 1994, LSI-2438124](https://c64.xentax.com/media/Yamaha_YM3812_Application_Manual.pdf):
  numbered pages 8-21 describe registers, key scaling, envelope timing, pitch,
  feedback, rhythm controls, and the four OPL2 waveforms. Table 3-6 supplies
  envelope timing ranges, and the text specifies a logarithmic envelope with an
  exponential attack. The vibrato depths of 7 and 14 cents also come from here.

## Implemented musical controls

- Nine two-operator channels in OPL2 mode and eighteen in OPL3 mode, with all
  thirty-six operators addressed through the documented register layout.
- Ten-bit F-number, octave block, and all sixteen multiplier codes, including
  half-frequency and the duplicated high multiplier values.
- Key-on/off, attack/decay/sustain/release, percussive versus sustained envelope
  types, key scale rate and note select, total level, and key scale level.
- FM and additive two-operator connections, and seven nonzero feedback settings.
- Four gated OPL2 waveforms and all eight OPL3 waveform selections.
- Per-operator tremolo/vibrato with shared low-frequency oscillators and depth
  controls. Both sound banks share those clocks.
- All six four-operator pairs and their four connection patterns. Paired
  operators use the primary channel's pitch and key state.
- Five rhythm triggers in the first bank: bass drum, snare, tom, cymbal, and hi-hat.
  The bass drum uses FM operators. The other percussion voices combine the
  assigned operators with a deterministic noise approximation.
- OPL3 output selection. The API maps bus A to left and B to right, folding the
  auxiliary C/D buses into the corresponding stereo sides. OPL2 output is mono
  on both sides. An OPL3 channel with all output bits cleared is silent.

## Timing and implementation

The native synthesis clock is `14,318,180 / 288` samples per second. Each
operator accumulates a phase in cycles, with native increment
`F-number * 2^(block - 20) * multiplier`. Operators produce mathematically
constructed waveforms whose envelope and total-level attenuation determine
their amplitude. A full-amplitude modulator changes the next operator's phase by
four cycles. This scale was established with controlled tone measurements.
Feedback uses the average of its previous two outputs and Yamaha's published
feedback factors.

Waveforms and decibel-to-amplitude curves are generated once using `sin`,
`exp2`, and `pow`, then sampled with linear interpolation. Register writes cache
pitch increments, level factors, and envelope coefficients, keeping logarithms
and exponentials out of the sample loop.

The envelope uses continuous attenuation in decibels. Attack follows an offset
exponential fitted to both the full and audible 10%-90% durations in Yamaha's
manual. Decay and release rise linearly in decibels. Rate zero holds the
corresponding envelope stage, maximum attack is immediate, and sustain level 15
means 93 dB. Intermediate times follow a mathematical fit to Yamaha's
rate-doubling progression. Key scale level uses a continuous logarithmic pitch
curve with the documented dB-per-octave slopes.

The public API accepts integer output rates from 8,000 through 192,000 Hz and
interleaved signed 16-bit stereo samples. An integer clock accumulator and linear
resampling preserve native timing across arbitrary buffer boundaries. Rendering
the same writes at the same frame positions produces the same samples regardless
of buffer partitioning within a given build. Reset clears registers, oscillators,
envelopes, noise, low-frequency clocks, timers, and resampler state. A register
above `0x1FF`, invalid sample rate, or incomplete stereo frame raises `std::runtime_error`
before altering synthesis state. Unassigned register addresses have no musical
effect. Rendering performs no dynamic allocation.

The two timers and the status register follow Yamaha's manual, for hosts whose
drivers detect the chip by polling them. Timer 1 counts in 80 µs steps and timer 2
in 320 µs steps (288 and 1152 cycles of the 3.58 MHz chip clock, so 1152 and 4608
master clocks), from the presets in registers `0x02` and `0x03`. Register `0x04`
starts them (bits 0 and 1) and masks their flags (bits 6 and 5). A write with
bit 7 set only clears the flags. A running timer overflows every `256 - preset`
steps and reloads from its register, and an unmasked overflow sets its flag.
`Status()` returns bit 7 (IRQ, set while any flag is), bit 6 (timer 1) and bit 5
(timer 2), the other bits 0. Time advances with each render, by the rendered
duration in an exact integer accumulator, and through `Advance()` in master
clocks for hosts that poll between renders. Register `0x04` is not a synthesis
register, so timer use never changes the sound.

## Fidelity boundaries

This is a functional musical emulation, not a cycle-accurate or bit-exact model
of Yamaha silicon. In particular:

- Envelopes are smooth approximations, not the chip's quantized envelope steps.
  Key scale level is continuous rather than the documented quantized curve.
- Waveforms are analytic shapes, including an exponential approximation to
  waveform seven. Modulation, feedback arithmetic, and rounding use floating
  point rather than the chip's internal fixed-point widths and logarithmic ROMs.
- Tremolo/vibrato use triangle oscillators at the documented nominal rates.
  Rhythm noise and metallic phase interactions are approximations, and their
  sample sequences are not intended to match the hardware.
- Linear resampling does not provide a steep antialiasing filter. Very high
  pitches, strong feedback, and low output rates can alias. The default 48 kHz
  output is the intended playback rate.
- There is no emulation of DAC quantization, analog output filtering, bus write
  delays, interrupt lines, or timer-driven CSM speech mode. The timers set the
  status flags only. Timer writes and the test bits of register `0x01` have no
  musical effect. Only its waveform select enable bit does.
- Floating-point math libraries can differ slightly between compilers/platforms,
  so cross-platform bit-identical PCM is not promised.

The focused tests verify audible pitch and pitch ratios, level attenuation,
envelope timing, modulation sidebands, register gating, routing, the timers and
status register, deterministic restart/streaming, and safe handling of malformed
calls. They use mathematical invariants, Yamaha's published measurements, and
broad spectral bounds from controlled DOSBox captures. They do not compare exact
sample sequences. Passing them establishes functional coverage and calibrated
balance rather than equivalence to a physical sound card.
