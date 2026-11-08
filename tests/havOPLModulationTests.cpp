// SPDX-License-Identifier: MIT

#include "havOPL.hpp"

#include <cmath>
#include <complex>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
  constexpr std::uint32_t sampleRate = 48000;
  constexpr double fundamental = 580.0 * 16.0 * (14318180.0 / 288.0) / 1048576.0;

  void Check(bool condition, const std::string &message)
  {
    if (!condition)
    {
      throw std::runtime_error(message);
    }
  }

  // Project onto a known frequency rather than rounding it to an FFT bin. The
  // Hann window suppresses leakage from the adjacent musical partials. This
  // measures the audible output and doesn't inspect synthesizer internals.
  double Partial(std::span<const std::int16_t> stereo, double frequency)
  {
    const std::size_t frames = stereo.size() / 2;
    const double angularStep = 2.0 * std::numbers::pi * frequency / sampleRate;
    const std::complex<double> step{std::cos(angularStep), -std::sin(angularStep)};

    std::complex<double> phase{1.0, 0.0}, sum{};

    double weight = 0.0;

    for (std::size_t frame = 0; frame < frames; ++frame)
    {
      const double window =
          0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(frame) /
                               static_cast<double>(frames - 1));

      sum += static_cast<double>(stereo[frame * 2]) * window * phase;
      weight += window;
      phase *= step;
    }

    return 2.0 * std::abs(sum) / weight;
  }

  void CheckModulation(unsigned modulatorLevel, double minimum, double maximum)
  {
    havOPL::Chip chip(sampleRate);

    const auto write = [&](std::uint16_t address, unsigned value)
    {
      chip.Write(address, static_cast<std::uint8_t>(value));
    };

    // A deliberately simple patch: sustained sine modulator at f, sine carrier
    // at 4f, immediate attack, no feedback, key scaling, LFO or rhythm mode.
    write(0x20, 0x21);
    write(0x23, 0x24);
    write(0x40, modulatorLevel);
    write(0x43, 16);
    write(0x60, 0xf0);
    write(0x63, 0xf0);
    write(0x80, 0x0f);
    write(0x83, 0x0f);
    write(0xc0, 0);
    write(0xa0, 580 & 255);
    write(0xb0, (580 >> 8) | (4 << 2) | 32);

    std::vector<std::int16_t> warmup(sampleRate / 5);
    chip.Render(warmup);

    std::vector<std::int16_t> samples(sampleRate * 2);
    chip.Render(samples);

    const double carrier = Partial(samples, fundamental * 4);
    const double lower = Partial(samples, fundamental * 3) / carrier;
    const double upper = Partial(samples, fundamental * 5) / carrier;

    Check(carrier > 20.0, "FM probe must retain an audible carrier");

    // For sin(4*w*t + beta*sin(w*t)), first sideband/carrier amplitudes are
    // |J1(beta)/J0(beta)|. Independent DOSBox register-only probes gave
    // about 1.186 at TL32 and 0.424 at TL40. Four cycles of full-scale phase
    // modulation and nominal 0.75 dB TL steps predict about 1.23 and 0.432.
    // These bounds allow analytic-versus-quantized chip arithmetic while
    // rejecting a two-cycle coupling without comparing PCM vectors.
    const std::string label = "FM first sideband at modulator TL " + std::to_string(modulatorLevel);

    Check(lower >= minimum && lower <= maximum, label + " (lower): " + std::to_string(lower));
    Check(upper >= minimum && upper <= maximum, label + " (upper): " + std::to_string(upper));
    Check(std::abs(lower - upper) < 0.02, label + " must be symmetric around the carrier");

    std::cout << label << ": " << lower << ", " << upper << '\n';
  }
} // namespace

int main()
{
  try
  {
    CheckModulation(32, 1.10, 1.30);
    CheckModulation(40, 0.39, 0.46);

    std::cout << "havOPL modulation-depth spectral tests passed\n";

    return 0;
  }
  catch (const std::exception &error)
  {
    std::cerr << error.what() << '\n';

    return 1;
  }
}
