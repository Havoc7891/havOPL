// SPDX-License-Identifier: MIT

#include "havOPL.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Checks envelope timing against the durations Yamaha specifies in the YM3812
// Application Manual, Table 3-6 (printed page 16). The OPL3 square waveform
// exposes the envelope directly. This avoids estimating a moving sine envelope
// from RMS windows. Zero frequency gives a constant carrier for the
// decay/release measurements.
namespace
{
  using Samples = std::vector<std::int16_t>;

  constexpr std::uint32_t sampleRate = 48000;
  constexpr double fullScale = 4096.0;

  void Reg(havOPL::Chip &chip, int address, int value)
  {
    chip.Write(static_cast<std::uint16_t>(address), static_cast<std::uint8_t>(value));
  }

  void Start(havOPL::Chip &chip,
             int attack,
             int decay = 0,
             int release = 8,
             int sustain = 0,
             int block = 0,
             bool keyScaling = false,
             int frequency = 0,
             bool noteSelect = false)
  {
    Reg(chip, 0x105, 1);
    Reg(chip, 8, noteSelect ? 0x40 : 0);
    Reg(chip, 0x20, 0x21);
    Reg(chip, 0x40, 63);
    Reg(chip, 0x60, 0); // Silent modulator: observe only the carrier envelope
    Reg(chip, 0x23, 0x21 | (keyScaling ? 0x10 : 0));
    Reg(chip, 0x43, 0);
    Reg(chip, 0x63, attack * 16 + decay);
    Reg(chip, 0x83, sustain * 16 + release);
    Reg(chip, 0xe3, 6);
    Reg(chip, 0xc0, 0x30);
    Reg(chip, 0xa0, frequency & 255);
    Reg(chip, 0xb0, 0x20 | (block << 2) | (frequency >> 8));
  }

  Samples Render(havOPL::Chip &chip, double seconds)
  {
    Samples result(static_cast<std::size_t>(seconds * sampleRate) * 2);

    chip.Render(result);

    return result;
  }

  double Crossing(const Samples &samples, double fraction, bool rising, std::size_t startFrame = 0)
  {
    for (std::size_t frame = startFrame; frame < samples.size() / 2; ++frame)
    {
      const double amplitude = std::abs(static_cast<double>(samples[frame * 2]));

      if (rising ? amplitude >= fullScale * fraction : amplitude <= fullScale * fraction)
      {
        return static_cast<double>(frame) / sampleRate;
      }
    }

    throw std::runtime_error("Envelope did not cross the requested amplitude");
  }

  void Near(double actual, double expected, double tolerance, const char *description)
  {
    if (std::abs(actual - expected) > tolerance)
    {
      throw std::runtime_error(std::string(description) + ": got " + std::to_string(actual) +
                               ", expected " + std::to_string(expected));
    }
  }

  double AttackSpan(
      int attack, int block = 0, bool scaling = false, int frequency = 0, bool noteSelect = false)
  {
    havOPL::Chip chip;

    Start(chip, attack, 0, 8, 0, block, scaling, frequency, noteSelect);

    const auto samples = Render(chip, 3.0);

    return Crossing(samples, 0.9, true) - Crossing(samples, 0.1, true);
  }

  void AttackTimes()
  {
    // These are the manual's measured 10%-90% amplitude intervals, not the
    // implementation's full attack durations or its fitted curve coefficients.
    Near(AttackSpan(2), 0.74138, 0.74138 * 0.04, "AR 2 audible attack time");
    Near(AttackSpan(4), 0.18534, 0.18534 * 0.04, "AR 4 audible attack time");
    Near(AttackSpan(6), 0.04634, 0.04634 * 0.04, "AR 6 audible attack time");

    // The documented key-number offset is added to the fourfold register rate.
    // Block 4 gives N=8: KSR=0 adds 2 and KSR=1 adds 8.
    Near(AttackSpan(4, 4), 0.12390, 0.12390 * 0.04, "Reduced key-scale rate offset");
    Near(AttackSpan(4, 4, true), 0.04634, 0.04634 * 0.04, "Full key-scale rate offset");

    // F-number 512 has bit 9 set and bit 8 clear. NOTE SEL=1 adds one rate step.
    Near(AttackSpan(4, 4, true, 512, true), 0.03610, 0.03610 * 0.04, "Note-select key-scale split");
  }

  void DecayAndRelease()
  {
    constexpr double duration = 0.30688;             // Rate 8: 0-96 dB, Table 3-6
    constexpr double fraction = 0.03162277660168379; // -30 dB, readily measurable in 16-bit PCM
    constexpr double expected = duration * 30.0 / 96.0;

    havOPL::Chip decay;

    Start(decay, 15, 8, 8, 14);

    const auto decaying = Render(decay, 0.2);

    // Skip the resampler's initial zero-valued frame before the immediate attack
    Near(Crossing(decaying, fraction, false, 4), expected, 0.001,
         "Decay attenuation follows the full-range duration");

    havOPL::Chip release;
    Start(release, 15, 0, 8);
    (void)Render(release, 0.01);
    Reg(release, 0xb0, 0);
    Near(Crossing(Render(release, 0.2), fraction, false), expected, 0.001,
         "Release uses the same attenuation slope as decay");
  }
} // namespace

int main()
{
  try
  {
    DecayAndRelease();
    AttackTimes();

    std::cout << "havOPL envelope timing tests passed\n";

    return 0;
  }
  catch (const std::exception &error)
  {
    std::cerr << error.what() << '\n';

    return 1;
  }
}
