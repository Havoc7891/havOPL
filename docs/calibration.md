# OPL instrument balance calibration

This document records how havOPL's modulation depth and attack timing were
established. The modulation depth was measured against DOSBox's OPL emulation
by comparing WAV captures with havOPL's output for the same register writes.
The attack timing comes from Yamaha's published envelope tables.

## Reference procedure

The reference is DOSBox, whose manual documents WAV capture. During development,
a throwaway DOS program played the probe's register/value sequence into ports
`0x388/0x389` inside DOSBox while the output was captured. That program is not
part of havOPL. The comparison is black-box only. No DOSBox source or any other
emulator code was read or copied.

Initial captures used DOSBox's default 44100 Hz OPL/mixer settings. A 49716 Hz
comparison checked that the result was not solely a sample-rate difference.

## Modulation strength

The synthetic probe uses sine operators with no feedback, F-number 580, block 4,
modulator multiplier 1, carrier multiplier 4, sustained instantaneous envelopes,
and carrier total level 16. Modulator total levels 32 and 40 give nominal
attenuations of 24 and 30 dB. It also includes a clean carrier segment.

Hann-windowed Fourier measurements give the first modulation sideband amplitude
relative to the carrier. Combining this ratio with the ordinary sinusoidal FM
relation `J1(beta)/J0(beta)` gives the phase modulation index, independently of
master output gain.

| Modulator TL | Reference sideband/carrier | Inferred full-amplitude phase depth |
| ------------ | -------------------------: | ----------------------------------: |
| 32           |                     1.1864 |                        3.940 cycles |
| 40           |                     0.4237 |                        3.931 cycles |

These measurements support the nominal four-cycle scale havOPL uses for a
full-amplitude modulator. The mathematical core produces ratios of about 1.226
and 0.432 with that scale. Remaining differences are consistent with its
continuous level/phase model. Feedback and total output volume are separate
parameters with their own documented scales.

`havOPLModulationTests` checks both probes with broad bounds and sideband
symmetry. The bounds are tight enough that a two-cycle scale fails the test.
The expected values are the spectral measurements above.

## Attack timing

Fitting an exponential attack to an arbitrary near-zero completion threshold
makes its audible rise almost twice as fast as Yamaha specifies. havOPL instead
uses an offset exponential fitted to both the full and the audible 10%-90%
durations:

```text
aNext = (a + 1 dB) * exp(-ln(97) / (synthesisRate * fullAttackTime)) - 1 dB
```

It clamps at zero attenuation and enters decay. Starting from 96 dB attenuation,
this preserves the full attack duration while giving a 10%-90% amplitude span
of about 52.35% of that duration. Yamaha's table gives about 52.46%.
`havOPLEnvelopeTests` checks published audible times at several rates, key
scaling, note select and decay/release timing. The reference is Table 3-6,
printed page 16 of the
[Yamaha YM3812 Application Manual](https://c64.xentax.com/media/Yamaha_YM3812_Application_Manual.pdf).
This is still a continuous approximation to a clocked hardware envelope.
