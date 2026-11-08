// SPDX-License-Identifier: MIT

#include "havOPL.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// Functional tests of the behavior documented in Yamaha's YM3812 application
// manual and YMF262 data sheet. Pitch, register routing and relative levels are
// tested independently of the approximation chosen for envelope timing and the
// output amplitude scale.
namespace
{
  using Samples = std::vector<std::int16_t>;

  constexpr std::uint32_t rate = 48000;
  constexpr std::array<int, 9> slots{0, 1, 2, 8, 9, 10, 16, 17, 18};

  void Check(bool value, const std::string &what)
  {
    if (!value)
    {
      throw std::runtime_error(what);
    }
  }

  template <class F> void Rejected(F &&function, const char *what)
  {
    try
    {
      function();
    }
    catch (const std::runtime_error &)
    {
      return;
    }

    throw std::runtime_error(std::string("Accepted invalid OPL input: ") + what);
  }

  void Reg(havOPL::Chip &chip, int address, int value)
  {
    chip.Write(static_cast<std::uint16_t>(address), static_cast<std::uint8_t>(value));
  }

  void Note(havOPL::Chip &chip,
            int bank = 0,
            int channel = 0,
            int fnum = 580,
            int block = 4,
            bool key = true)
  {
    Reg(chip, bank + 0xa0 + channel, fnum & 255);
    Reg(chip, bank + 0xb0 + channel, ((fnum >> 8) & 3) | (block << 2) | (key ? 32 : 0));
  }

  void Tone(havOPL::Chip &chip,
            int bank = 0,
            int channel = 0,
            int multiplier = 1,
            int level = 0,
            int routing = 0,
            int attack = 15,
            int waveform = 0)
  {
    const int mod = bank + slots[static_cast<std::size_t>(channel)];
    const int carrier = mod + 3;

    Reg(chip, 1, 0x20);
    Reg(chip, mod + 0x20, 0x21);
    Reg(chip, mod + 0x40, 63);
    Reg(chip, mod + 0x60, 0); // AR zero keeps the modulator silent
    Reg(chip, mod + 0x80, 0x0f);
    Reg(chip, carrier + 0x20, 0x20 | multiplier);
    Reg(chip, carrier + 0x40, level);
    Reg(chip, carrier + 0x60, attack << 4);
    Reg(chip, carrier + 0x80, 0x0f);
    Reg(chip, carrier + 0xe0, waveform);
    Reg(chip, bank + 0xc0 + channel, routing);

    Note(chip, bank, channel);
  }

  Samples Render(havOPL::Chip &chip, std::size_t frames)
  {
    Samples output(frames * 2, 12345);

    chip.Render(output);

    return output;
  }

  double Rms(std::span<const std::int16_t> data, int channel = 0)
  {
    long double sum = 0;

    for (std::size_t i = static_cast<std::size_t>(channel); i < data.size(); i += 2)
    {
      const auto value = static_cast<long double>(data[i]);

      sum += value * value;
    }

    return data.empty() ? 0 : std::sqrt(static_cast<double>(sum / (data.size() / 2)));
  }

  double Frequency(std::span<const std::int16_t> data, std::uint32_t sampleRate, int channel = 0)
  {
    std::size_t crossings = 0;

    for (std::size_t i = static_cast<std::size_t>(channel) + 2; i < data.size(); i += 2)
    {
      if (data[i - 2] <= 0 && data[i] > 0)
      {
        ++crossings;
      }
    }

    return static_cast<double>(crossings) * sampleRate / static_cast<double>(data.size() / 2);
  }

  void Near(double actual, double expected, double tolerance, const char *what)
  {
    Check(std::abs(actual - expected) <= tolerance, std::string(what) + ": got " +
                                                        std::to_string(actual) + ", expected " +
                                                        std::to_string(expected));
  }

  void Basics()
  {
    havOPL::Chip chip;

    Check(Rms(Render(chip, 1000)) == 0, "Reset chip must be silent");

    chip.Reset();

    Tone(chip);

    const auto initial = Render(chip, 10000);

    Check(Rms(initial) > 10, "Key-on must produce audible output");

    chip.Reset();

    Tone(chip);

    Check(Render(chip, 10000) == initial,
          "Reset must reproduce oscillator/envelope/resampler state");

    Rejected([] { havOPL::Chip bad(7999); }, "sample rate below bound");
    Rejected([] { havOPL::Chip bad(192001); }, "sample rate above bound");
    Rejected([&] { chip.Write(0x200, 0); }, "register outside two banks");

    std::array<std::int16_t, 3> odd{};

    Rejected([&] { chip.Render(odd); }, "odd interleaved sample count");

    chip.Reset();

    Tone(chip, 0, 0, 1, 0, 0, 0);

    Check(Rms(Render(chip, rate)) == 0, "AR zero must not start a silent operator");
  }

  void PitchAndLevels()
  {
    constexpr double expected = 580.0 * 16.0 * (14318180.0 / 288.0) / 1048576.0;

    for (const auto sampleRate : {8000U, 48000U, 192000U})
    {
      havOPL::Chip chip(sampleRate);
      Tone(chip);

      (void)Render(chip, sampleRate / 10);

      const auto data = Render(chip, sampleRate);

      Near(Frequency(data, sampleRate), expected, 2.0,
           "Documented F-number/block pitch at output rate");

      for (std::size_t i = 0; i < data.size(); i += 2)
      {
        Check(data[i] == data[i + 1], "OPL2 output must be mono in both lanes");
      }
    }

    for (const auto multiplier : {0, 1, 2, 4})
    {
      havOPL::Chip chip;

      Tone(chip, 0, 0, multiplier);

      const auto data = Render(chip, rate);

      Near(Frequency(data, rate), expected * (multiplier ? multiplier : 0.5), 2.0,
           "Operator frequency multiplier including code zero");
    }

    havOPL::Chip octave;

    Tone(octave);

    Note(octave, 0, 0, 580, 5);

    Near(Frequency(Render(octave, rate), rate), expected * 2, 2.0,
         "Block increment doubles frequency");

    havOPL::Chip loud;
    havOPL::Chip quiet;

    Tone(loud);
    Tone(quiet, 0, 0, 1, 16);

    const auto a = Render(loud, rate / 4), b = Render(quiet, rate / 4);

    // Each TL step is 0.75 dB: sixteen steps reduce amplitude by 12 dB
    Near(Rms(b) / Rms(a), std::pow(10.0, -12.0 / 20.0), 0.025, "Total-level attenuation");
  }

  void EnvelopesAndChunks()
  {
    havOPL::Chip uninterrupted;
    havOPL::Chip repeated;

    Tone(uninterrupted);
    Tone(repeated);

    Check(Render(uninterrupted, 4000) == Render(repeated, 4000), "Matched initial state");

    Note(repeated);

    Check(Render(uninterrupted, 10000) == Render(repeated, 10000),
          "Held key-on must not retrigger");

    const double sustained = Rms(Render(repeated, rate / 4));

    (void)Render(repeated, rate);

    Near(Rms(Render(repeated, rate / 4)) / sustained, 1, 0.03,
         "EGT sustain remains while key held");

    Note(repeated, 0, 0, 580, 4, false);

    (void)Render(repeated, rate / 2);

    Check(Rms(Render(repeated, rate / 4)) < sustained * 0.01, "Fast key-off release becomes quiet");

    havOPL::Chip whole(44100);
    havOPL::Chip chunks(44100);

    Tone(whole);
    Tone(chunks);

    const auto expected = Render(whole, 22050);

    Samples actual(expected.size());

    constexpr std::array<std::size_t, 7> sizes{1, 17, 129, 7, 512, 3, 1001};

    std::size_t at = 0, iteration = 0;

    while (at < actual.size())
    {
      chunks.Render({}); // Empty buffers must not advance synthesis

      const auto count = std::min(sizes[iteration++ % sizes.size()] * 2, actual.size() - at);

      chunks.Render(std::span(actual).subspan(at, count));

      at += count;
    }

    Check(actual == expected, "Render partitioning must preserve sample and resampling phase");
  }

  Samples Waveform(int selection, bool enabled)
  {
    havOPL::Chip chip;

    Tone(chip, 0, 0, 1, 0, 0, 15, selection);

    Reg(chip, 1, enabled ? 0x20 : 0);

    return Render(chip, rate / 5);
  }

  void Waveforms()
  {
    Check(Waveform(1, false) == Waveform(0, false),
          "OPL2 waveform-enable bit gates waveform selection");

    const auto sine = Waveform(0, true);

    std::array<Samples, 4> forms{sine, Waveform(1, true), Waveform(2, true), Waveform(3, true)};

    for (std::size_t i = 0; i < forms.size(); ++i)
    {
      Check(Rms(forms[i]) > 10, "Every documented OPL2 waveform produces output");

      for (std::size_t j = 0; j < i; ++j)
      {
        Check(forms[i] != forms[j], "OPL2 waveforms must be distinct");
      }
    }
  }

  void Routing()
  {
    havOPL::Chip secondBank;
    Tone(secondBank, 0x100, 0, 1, 0, 0x10);
    Check(Rms(Render(secondBank, 2000)) == 0, "Second bank is inaudible in OPL2 mode");
    Reg(secondBank, 0x105, 1);
    (void)Render(secondBank, 1000);

    const auto left = Render(secondBank, 10000);
    Check(Rms(left, 0) > 10 && Rms(left, 1) == 0, "OPL3 second-bank left routing");
    Reg(secondBank, 0x1c0, 0x20);
    (void)Render(secondBank, 1000);

    const auto right = Render(secondBank, 10000);
    Check(Rms(right, 1) > 10 && Rms(right, 0) == 0, "OPL3 second-bank right routing");
    Reg(secondBank, 0x105, 0);
    (void)Render(secondBank, 1000);

    Check(Rms(Render(secondBank, 1000)) == 0, "OPL2 selection disables second-bank output");

    havOPL::Chip muted;
    Reg(muted, 0x105, 1);
    Tone(muted);
    Check(Rms(Render(muted, 1000)) == 0, "OPL3 channel with no output route is silent");

    havOPL::Chip stereo;
    Reg(stereo, 0x105, 1);
    Tone(stereo, 0, 0, 1, 0, 0x10);
    Tone(stereo, 0x100, 0, 1, 0, 0x20);
    Note(stereo, 0x100, 0, 870);

    const auto mixed = Render(stereo, rate);

    Near(Frequency(mixed, rate, 0), 440, 2, "Independent bank-zero pitch");
    Near(Frequency(mixed, rate, 1), 660, 2, "Independent bank-one pitch");
  }

  Samples FourOperator(int bank, bool paired, int algorithm = 0)
  {
    havOPL::Chip chip;
    Reg(chip, 0x105, 1);
    Reg(chip, 0x104, paired ? (bank ? 8 : 1) : 0);

    for (const auto channel : {0, 3})
    {
      Tone(chip, bank, channel, 1, 8, 0x30 | ((algorithm >> (channel == 0 ? 0 : 1)) & 1));

      const auto mod = bank + slots[static_cast<std::size_t>(channel)];

      Reg(chip, mod + 0x40, 16);
      Reg(chip, mod + 0x60, 0xf0);
      Reg(chip, mod + 0x80, 0x0f);
    }

    return Render(chip, rate / 4);
  }

  void FourOperatorPairs()
  {
    for (const auto bank : {0, 0x100})
    {
      const auto pair = FourOperator(bank, true);

      Check(Rms(pair) > 10, "Four-operator pair produces output in each bank");
      Check(pair != FourOperator(bank, false),
            "Four-operator connection differs from separate voices");
      Check(pair != FourOperator(bank, true, 3), "Four-operator algorithms change connection");
    }
  }
} // namespace

// The timers and status register (YM3812 manual: 80 us / 320 us steps, register 4
// start, mask and IRQ reset bits) in 14.31818 MHz master clocks: 1152 / 4608 a step
void Timers()
{
  havOPL::Chip chip(rate);
  Check(chip.Status() == 0, "Timers start stopped with no flags");

  // The detection sequence of OPL drivers: reset, preset 0xff, start timer 1 masking timer 2
  Reg(chip, 4, 0x60);
  Reg(chip, 4, 0x80);
  Check(chip.Status() == 0, "IRQ reset clears the flags");

  Reg(chip, 2, 0xff);
  Reg(chip, 4, 0x21);
  chip.Advance(1151);
  Check(chip.Status() == 0, "Timer 1 does not overflow before one 80 us step");

  chip.Advance(1);
  Check(chip.Status() == 0xc0, "Timer 1 overflow sets bits 7 and 6");

  Reg(chip, 4, 0x80);
  Check(chip.Status() == 0, "IRQ reset clears timer 1's flag");

  chip.Advance(1152);
  Check(chip.Status() == 0xc0, "A running timer reloads and overflows again");

  // Timer 2: two 320 us steps
  Reg(chip, 4, 0x00);
  Reg(chip, 4, 0x80);
  Reg(chip, 3, 0xfe);
  Reg(chip, 4, 0x02);
  chip.Advance(2 * 4608 - 1);
  Check(chip.Status() == 0, "Timer 2 does not overflow early");

  chip.Advance(1);
  Check(chip.Status() == 0xa0, "Timer 2 overflow sets bits 7 and 5");

  // A masked timer runs without setting its flag
  Reg(chip, 4, 0x00);
  Reg(chip, 4, 0x80);
  Reg(chip, 4, 0x41);
  chip.Advance(100000);
  Check(chip.Status() == 0, "A masked timer sets no flag");

  // Rendering advances the timers by the rendered duration: 256 steps of timer 1 are
  // 294912 master clocks, 988.65 frames at 48 kHz
  havOPL::Chip rendered(rate);
  Reg(rendered, 2, 0x00);
  Reg(rendered, 4, 0x01);
  Render(rendered, 988);
  Check(rendered.Status() == 0, "Rendering short of the period leaves the flag clear");

  Render(rendered, 1);
  Check(rendered.Status() == 0xc0, "Rendering past the period sets the flag");
  Check(Rms(Render(rendered, 1000)) == 0, "Timer registers do not make sound");
}

int main()
{
  try
  {
    Basics();
    PitchAndLevels();
    EnvelopesAndChunks();
    Waveforms();
    Routing();
    FourOperatorPairs();
    Timers();

    std::cout << "havOPL functional tests passed\n";

    return 0;
  }
  catch (const std::exception &error)
  {
    std::cerr << error.what() << '\n';

    return 1;
  }
}
