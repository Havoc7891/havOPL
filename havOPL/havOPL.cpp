// SPDX-License-Identifier: MIT

#include "havOPL.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace havOPL
{
  namespace
  {
    constexpr std::uint64_t masterClock = 14318180;
    constexpr double synthesisRate = static_cast<double>(masterClock) / 288.0;
    constexpr double silentDb = 96.0;

    // A 1 dB offset fits Yamaha's full and 10%->90% attack durations together.
    // An exponential fitted to an arbitrary near-zero cutoff rises too quickly.
    constexpr double attackOffsetDb = 1.0;
    constexpr std::size_t waveSize = 2048;
    constexpr std::size_t levelSteps = 6144; // 192 dB in 1/32 dB steps

    // Independent two-operator tone measurements give about four cycles at full
    // modulator amplitude. Feedback has its own documented scale below.
    constexpr double phaseModulation = 4.0;

    double Wrap(double phase) noexcept
    {
      return phase - std::floor(phase);
    }

    // Constructed from analytic functions once. Linear interpolation keeps the
    // tables small.
    struct Functions
    {
      std::array<std::array<double, waveSize + 1>, 8> waves{};
      std::array<double, levelSteps + 1> levels{};

      Functions()
      {
        for (std::size_t i = 0; i < waveSize; ++i)
        {
          const double phase = static_cast<double>(i) / static_cast<double>(waveSize);
          const double sine = std::sin(2.0 * std::numbers::pi * phase);
          const double doubled = std::sin(4.0 * std::numbers::pi * phase);

          waves[0][i] = sine;
          waves[1][i] = phase < 0.5 ? sine : 0.0;
          waves[2][i] = std::abs(sine);
          waves[3][i] = Wrap(phase * 2.0) < 0.5 ? std::abs(sine) : 0.0;
          waves[4][i] = phase < 0.5 ? doubled : 0.0;
          waves[5][i] = phase < 0.5 ? std::abs(doubled) : 0.0;
          waves[6][i] = phase < 0.5 ? 1.0 : -1.0;
          waves[7][i] = phase < 0.5 ? std::exp2(-16.0 * phase) : -std::exp2(-16.0 * (1.0 - phase));
        }

        for (auto &wave : waves)
        {
          wave[waveSize] = wave[0];
        }

        for (std::size_t i = 0; i <= levelSteps; ++i)
        {
          levels[i] = std::pow(10.0, -static_cast<double>(i) / (32.0 * 20.0));
        }
      }

      double Wave(unsigned shape, double phase) const noexcept
      {
        const double position = Wrap(phase) * static_cast<double>(waveSize);
        const auto index = static_cast<std::size_t>(position);
        const double fraction = position - static_cast<double>(index);

        return waves[shape][index] + fraction * (waves[shape][index + 1] - waves[shape][index]);
      }

      double Amplitude(double attenuation) const noexcept
      {
        if (attenuation >= 192.0)
        {
          return 0.0;
        }

        const double position = std::max(0.0, attenuation) * 32.0;
        const auto index = static_cast<std::size_t>(position);
        const double fraction = position - static_cast<double>(index);

        return levels[index] + fraction * (levels[index + 1] - levels[index]);
      }
    };

    const Functions &Curves()
    {
      static const Functions result;

      return result;
    }

    unsigned SlotAddress(std::size_t channel, std::size_t carrier) noexcept
    {
      const auto local = static_cast<unsigned>(channel % 9);

      return static_cast<unsigned>(channel / 9) * 0x100U + (local / 3U) * 8U + local % 3U +
             static_cast<unsigned>(carrier) * 3U;
    }

    unsigned ChannelAddress(std::size_t channel) noexcept
    {
      return static_cast<unsigned>(channel / 9) * 0x100U + static_cast<unsigned>(channel % 9);
    }

    double Multiplier(unsigned value) noexcept
    {
      if (value == 0)
      {
        return 0.5;
      }

      if (value == 11 || value == 13)
      {
        --value;
      }

      if (value == 14)
      {
        ++value;
      }

      return static_cast<double>(value);
    }

    unsigned EffectiveRate(unsigned rate, unsigned key, bool scaling) noexcept
    {
      if (rate == 0)
      {
        return 0;
      }

      return std::min(63U, rate * 4U + (scaling ? key : key / 4U));
    }

    // Yamaha gives attack/decay durations by rate, not a continuous differential
    // equation. This fit follows their octave-doubling progression and interpolates
    // each group of four rates. It intentionally doesn't reproduce clocked steps.
    double RateDuration(unsigned rate, double baseSeconds) noexcept
    {
      const double octave = static_cast<double>(static_cast<int>(rate / 4U) - 1);

      return std::exp2(-octave) * baseSeconds / (1.0 + static_cast<double>(rate % 4U) * 0.25);
    }

    double DecayIncrement(unsigned rate) noexcept
    {
      if (rate == 0)
      {
        return 0.0;
      }

      return silentDb / (synthesisRate * RateDuration(std::min(rate, 60U), 39.28064));
    }

    enum class Envelope
    {
      Off,
      Attack,
      Decay,
      Sustain,
      Release
    };

    struct Operator
    {
      double phase{};
      double increment{};
      double attenuation{silentDb};
      double level{};
      double attackFactor{1.0};
      double decayStep{};
      double releaseStep{};
      double sustainLevel{};
      double recent{}, older{};
      unsigned waveform{};
      bool key{}, sustaining{}, tremolo{}, vibrato{}, instantAttack{};
      Envelope envelope{Envelope::Off};

      void SetKey(bool pressed) noexcept
      {
        if (pressed && !key)
        {
          phase = 0.0;
          recent = 0.0;
          older = 0.0;

          envelope = Envelope::Attack;
        }
        else if (!pressed && key && envelope != Envelope::Off)
        {
          envelope = Envelope::Release;
        }

        key = pressed;
      }

      void StepEnvelope() noexcept
      {
        switch (envelope)
        {
        case Envelope::Off:
          break;

        case Envelope::Attack:
          if (instantAttack)
          {
            attenuation = 0.0;
          }
          else
          {
            attenuation = (attenuation + attackOffsetDb) * attackFactor - attackOffsetDb;
          }

          if (attenuation <= 0.0)
          {
            attenuation = 0.0;
            envelope = Envelope::Decay;
          }
          break;

        case Envelope::Decay:
          attenuation += decayStep;

          if (attenuation >= sustainLevel)
          {
            envelope = Envelope::Sustain;
          }
          break;

        case Envelope::Sustain:
          if (!sustaining)
          {
            attenuation += releaseStep;
          }
          break;

        case Envelope::Release:
          attenuation += releaseStep;
          break;
        }

        if (attenuation >= silentDb && envelope != Envelope::Attack)
        {
          attenuation = silentDb;

          envelope = Envelope::Off;
        }
      }
    };

    struct Channel
    {
      unsigned frequency{}, block{}, routing{};
      double feedback{};
      bool additive{}, key{};
    };

    std::int16_t Pcm(double value) noexcept
    {
      return static_cast<std::int16_t>(std::clamp(std::lround(value * 4096.0), -32768L, 32767L));
    }
  } // namespace

  struct Chip::State
  {
    std::uint32_t sampleRate;
    std::uint64_t outputPeriod;
    std::uint64_t outputPhase{};
    std::array<std::uint8_t, 512> registers{};
    std::array<Operator, 36> operators{};
    std::array<Channel, 18> channels{};
    std::array<double, 2> previous{}, current{};
    double tremoloPhase{}, vibratoPhase{};
    double tremoloDb{}, pitchScale{1.0};
    std::uint32_t noise{0x31415926U};
    bool extended{}, rhythm{};

    // Timers: one step of timer 1 is 288 chip clocks (80 us) and one step of timer 2
    // is 1152 (320 us), so 1152 and 4608 master clocks. A running timer overflows
    // every (256 - preset) steps, reloading from its register, and an unmasked
    // overflow sets its flag.
    std::uint64_t clock{};          // Master clocks elapsed
    std::uint64_t clockRemainder{}; // Render's fraction of a master clock in 1/sampleRate
    std::array<std::uint64_t, 2> overflowAt{};
    std::array<bool, 2> timerRunning{};
    std::uint8_t timerFlags{};
    const Functions &curves;

    explicit State(std::uint32_t rate)
        : sampleRate(rate), outputPeriod(static_cast<std::uint64_t>(rate) * 288U), curves(Curves())
    {
      Refresh();
    }

    bool Paired(std::size_t primary) const noexcept
    {
      if (!extended || primary % 9 >= 3)
      {
        return false;
      }

      const auto bit = static_cast<unsigned>((primary / 9) * 3 + primary % 9);

      return (registers[0x104] & (1U << bit)) != 0;
    }

    std::size_t PitchChannel(std::size_t channel) const noexcept
    {
      const auto local = channel % 9;

      if (local >= 3 && local < 6 && Paired(channel - 3))
      {
        return channel - 3;
      }

      return channel;
    }

    void Refresh()
    {
      extended = (registers[0x105] & 1U) != 0;
      rhythm = (registers[0xbd] & 0x20U) != 0;

      for (std::size_t index = 0; index < channels.size(); ++index)
      {
        auto &channel = channels[index];
        const unsigned address = ChannelAddress(index);
        const unsigned high = registers[address + 0xb0U];
        const unsigned connection = registers[address + 0xc0U];

        channel.frequency = registers[address + 0xa0U] | ((high & 3U) << 8U);
        channel.block = (high >> 2U) & 7U;
        channel.key = (high & 0x20U) != 0 && (extended || index < 9);
        channel.additive = (connection & 1U) != 0;
        channel.routing = extended ? connection >> 4U : 3U;

        const unsigned feedback = (connection >> 1U) & 7U;

        // Published feedback factors pi/16..4*pi converted to cycles
        channel.feedback = feedback == 0 ? 0.0 : std::exp2(static_cast<double>(feedback) - 6.0);
      }

      for (std::size_t index = 0; index < operators.size(); ++index)
      {
        auto &op = operators[index];

        const auto physicalChannel = index / 2;
        const auto &channel = channels[PitchChannel(physicalChannel)];
        const unsigned address = SlotAddress(physicalChannel, index % 2);
        const unsigned flags = registers[address + 0x20U];
        const unsigned level = registers[address + 0x40U];
        const unsigned attackDecay = registers[address + 0x60U];
        const unsigned sustainRelease = registers[address + 0x80U];
        const unsigned noteBit = (registers[8] & 0x40U) != 0 ? 9U : 8U;
        const unsigned keyNumber = channel.block * 2U + ((channel.frequency >> noteBit) & 1U);
        const bool scaling = (flags & 0x10U) != 0;
        const unsigned attack = EffectiveRate(attackDecay >> 4U, keyNumber, scaling);

        op.instantAttack = attack >= 60U;
        op.attackFactor = attack == 0
                              ? 1.0
                              : std::exp(-std::log((silentDb + attackOffsetDb) / attackOffsetDb) /
                                         (synthesisRate * RateDuration(attack, 2.82624)));
        op.decayStep = DecayIncrement(EffectiveRate(attackDecay & 15U, keyNumber, scaling));
        op.releaseStep = DecayIncrement(EffectiveRate(sustainRelease & 15U, keyNumber, scaling));

        const unsigned sustain = sustainRelease >> 4U;
        op.sustainLevel = sustain == 15U ? 93.0 : static_cast<double>(sustain) * 3.0;
        op.sustaining = (flags & 0x20U) != 0;
        op.tremolo = (flags & 0x80U) != 0;
        op.vibrato = (flags & 0x40U) != 0;
        op.increment = std::ldexp(static_cast<double>(channel.frequency),
                                  static_cast<int>(channel.block) - 20) *
                       Multiplier(flags & 15U);
        op.level = static_cast<double>(level & 63U) * 0.75;

        const unsigned keyScale = level >> 6U;

        if (keyScale != 0 && channel.frequency != 0)
        {
          // Continuous log-frequency approximation to the documented
          // quantized KSL curve. Codes 1,2,3 mean 3,1.5,6 dB/octave.
          const double slope = keyScale == 1U ? 3.0 : (keyScale == 2U ? 1.5 : 6.0);
          const double octaves = static_cast<double>(channel.block) +
                                 std::log2(static_cast<double>(channel.frequency) / 1024.0);

          op.level += std::max(0.0, octaves * slope);
        }

        op.waveform = extended
                          ? registers[address + 0xe0U] & 7U
                          : ((registers[1] & 0x20U) != 0 ? registers[address + 0xe0U] & 3U : 0U);

        bool key = channel.key;

        if (rhythm && physicalChannel >= 6 && physicalChannel <= 8)
        {
          unsigned mask = 0x10U; // Both bass drum operators

          if (physicalChannel == 7)
          {
            mask = index % 2 == 0 ? 1U : 8U;
          }

          if (physicalChannel == 8)
          {
            mask = index % 2 == 0 ? 4U : 2U;
          }

          key = (registers[0xbd] & mask) != 0;
        }

        op.SetKey(key);
      }
    }

    double EnvelopeAmplitude(std::size_t index) noexcept
    {
      auto &op = operators[index];
      op.StepEnvelope();

      if (op.envelope == Envelope::Off || op.attenuation >= silentDb)
      {
        return 0.0;
      }

      return curves.Amplitude(op.attenuation + op.level + (op.tremolo ? tremoloDb : 0.0));
    }

    void AdvancePhase(Operator &op) const noexcept
    {
      op.phase = Wrap(op.phase + op.increment * (op.vibrato ? pitchScale : 1.0));
    }

    double Oscillator(std::size_t index, double modulation) noexcept
    {
      auto &op = operators[index];

      const double amplitude = EnvelopeAmplitude(index);
      const double result =
          amplitude == 0.0 ? 0.0 : curves.Wave(op.waveform, op.phase + modulation) * amplitude;

      AdvancePhase(op);

      op.older = op.recent;
      op.recent = result;

      return result;
    }

    double FirstOperator(std::size_t channel) noexcept
    {
      const auto index = channel * 2;
      const auto &op = operators[index];

      return Oscillator(index, (op.recent + op.older) * 0.5 * channels[channel].feedback);
    }

    void Route(std::array<double, 2> &output, std::size_t channel, double signal) const noexcept
    {
      const unsigned routing = channels[channel].routing;

      // The two auxiliary buses are folded into the matching stereo side
      if ((routing & 5U) != 0)
      {
        output[0] += signal;
      }

      if ((routing & 10U) != 0)
      {
        output[1] += signal;
      }
    }

    void TwoOperators(std::array<double, 2> &output, std::size_t channel) noexcept
    {
      const double first = FirstOperator(channel);
      const bool additive = channels[channel].additive;
      const double second = Oscillator(channel * 2 + 1, additive ? 0.0 : first * phaseModulation);

      Route(output, channel, second + (additive ? first : 0.0));
    }

    void FourOperators(std::array<double, 2> &output, std::size_t channel) noexcept
    {
      const auto secondChannel = channel + 3;
      const bool firstConnection = channels[channel].additive;
      const bool secondConnection = channels[secondChannel].additive;
      const double a = FirstOperator(channel);
      const double b = Oscillator(channel * 2 + 1, firstConnection ? 0.0 : a * phaseModulation);
      const double c = Oscillator(secondChannel * 2,
                                  secondConnection && !firstConnection ? 0.0 : b * phaseModulation);
      const double d = Oscillator(secondChannel * 2 + 1,
                                  firstConnection && secondConnection ? 0.0 : c * phaseModulation);

      if (!firstConnection && !secondConnection)
      {
        Route(output, secondChannel, d);
      }
      else if (!firstConnection)
      {
        Route(output, channel, b);
        Route(output, secondChannel, d);
      }
      else if (!secondConnection)
      {
        Route(output, channel, a);
        Route(output, secondChannel, d);
      }
      else
      {
        Route(output, channel, a);
        Route(output, secondChannel, c + d);
      }
    }

    void Percussion(std::array<double, 2> &output) noexcept
    {
      TwoOperators(output, 6);

      const double hatPhase = operators[14].phase;
      const double cymbalPhase = operators[17].phase;
      const bool metallic = (Wrap(hatPhase * 4.0) < 0.5) != (Wrap(cymbalPhase * 8.0) < 0.5);
      const double randomSign = (noise & 1U) != 0 ? 1.0 : -1.0;

      // Yamaha documents the rhythm slots, but not their internal noise/phase
      // logic. This approximation combines their pitched operators
      // with deterministic broadband noise and inharmonic square components.
      const double hat = EnvelopeAmplitude(14) * randomSign * (metallic ? 1.0 : 0.25);
      const double snare =
          EnvelopeAmplitude(15) * (0.65 * randomSign + 0.35 * curves.Wave(6, operators[15].phase));

      AdvancePhase(operators[14]);
      AdvancePhase(operators[15]);

      const double tom = Oscillator(16, 0.0);
      const double cymbal = EnvelopeAmplitude(17) * (metallic ? 1.0 : -1.0);

      AdvancePhase(operators[17]);

      Route(output, 7, hat + snare);
      Route(output, 8, tom + cymbal);
    }

    std::array<double, 2> Tick() noexcept
    {
      tremoloPhase = Wrap(tremoloPhase + 3.7 / synthesisRate);
      vibratoPhase = Wrap(vibratoPhase + 6.4 / synthesisRate);

      const double triangle = 1.0 - std::abs(tremoloPhase * 2.0 - 1.0);

      tremoloDb = triangle * ((registers[0xbd] & 0x80U) != 0 ? 4.8 : 1.0);

      // The manual's 7/14-cent pitch ranges are approximated by a triangle
      // LFO and its small-signal frequency ratio (ln(2) / 1200 per cent).
      const double vibrato = (1.0 - std::abs(vibratoPhase * 2.0 - 1.0)) * 2.0 - 1.0;
      const double depth = (registers[0xbd] & 0x40U) != 0 ? 14.0 : 7.0;

      pitchScale = 1.0 + vibrato * depth * (std::numbers::ln2 / 1200.0);

      // A deterministic PRNG. It makes no attempt to match the chip's LFSR.
      noise ^= noise << 13U;
      noise ^= noise >> 17U;
      noise ^= noise << 5U;

      std::array<double, 2> output{};

      const std::size_t count = extended ? 18 : 9;

      for (std::size_t channel = 0; channel < count; ++channel)
      {
        if (rhythm && channel >= 6 && channel <= 8)
        {
          continue;
        }

        if (PitchChannel(channel) != channel)
        {
          continue;
        }

        const bool silent = operators[channel * 2].envelope == Envelope::Off &&
                            operators[channel * 2 + 1].envelope == Envelope::Off;

        if (Paired(channel))
        {
          if (silent && operators[(channel + 3) * 2].envelope == Envelope::Off &&
              operators[(channel + 3) * 2 + 1].envelope == Envelope::Off)
          {
            continue;
          }

          FourOperators(output, channel);
        }
        else if (!silent)
        {
          TwoOperators(output, channel);
        }
      }

      if (rhythm)
      {
        Percussion(output);
      }

      return output;
    }

    static constexpr std::array<std::uint64_t, 2> timerStep{1152, 4608};

    std::uint64_t TimerPeriod(std::size_t timer) const noexcept
    {
      return (256U - registers[2 + timer]) * timerStep[timer];
    }

    void TimerControl(std::uint8_t value) noexcept
    {
      if ((value & 0x80U) != 0)
      {
        // IRQ reset: the flags cleared, nothing else
        timerFlags = 0;

        return;
      }

      registers[4] = value;

      for (std::size_t timer = 0; timer < 2; ++timer)
      {
        const bool start = (value & (1U << timer)) != 0;

        if (start && !timerRunning[timer])
        {
          overflowAt[timer] = clock + TimerPeriod(timer);
        }

        timerRunning[timer] = start;
      }
    }

    void RunTimers() noexcept
    {
      for (std::size_t timer = 0; timer < 2; ++timer)
      {
        while (timerRunning[timer] && clock >= overflowAt[timer])
        {
          if ((registers[4] & (0x40U >> timer)) == 0)
          {
            timerFlags |= static_cast<std::uint8_t>(0x40U >> timer);
          }

          overflowAt[timer] += TimerPeriod(timer);
        }
      }
    }

    void Advance(std::uint64_t masterClocks) noexcept
    {
      clock += masterClocks;

      RunTimers();
    }

    void Render(std::span<std::int16_t> stereo) noexcept
    {
      clockRemainder += static_cast<std::uint64_t>(stereo.size() / 2) * masterClock;

      Advance(clockRemainder / sampleRate);

      clockRemainder %= sampleRate;

      for (std::size_t frame = 0; frame < stereo.size(); frame += 2)
      {
        outputPhase += masterClock;

        while (outputPhase >= outputPeriod)
        {
          previous = current;
          current = Tick();
          outputPhase -= outputPeriod;
        }

        const double fraction =
            static_cast<double>(outputPhase) / static_cast<double>(outputPeriod);

        stereo[frame] = Pcm(previous[0] + (current[0] - previous[0]) * fraction);
        stereo[frame + 1] = Pcm(previous[1] + (current[1] - previous[1]) * fraction);
      }
    }
  };

  Chip::Chip(std::uint32_t sampleRate)
  {
    if (sampleRate < 8000 || sampleRate > 192000)
    {
      throw std::runtime_error("OPL sample rate must be between 8000 and 192000 Hz");
    }

    mState = std::make_unique<State>(sampleRate);
  }

  Chip::~Chip() = default;
  Chip::Chip(Chip &&) noexcept = default;
  Chip &Chip::operator=(Chip &&) noexcept = default;

  void Chip::Reset()
  {
    mState = std::make_unique<State>(mState->sampleRate);
  }

  void Chip::Write(std::uint16_t reg, std::uint8_t value)
  {
    if (reg >= 0x200)
    {
      throw std::runtime_error("OPL register must be in the range 0x000..0x1ff");
    }

    if (reg == 0x004)
    {
      // Timer control, not a synthesis register
      mState->TimerControl(value);

      return;
    }

    mState->registers[reg] = value;
    mState->Refresh();
  }

  std::uint8_t Chip::Status() const noexcept
  {
    const std::uint8_t flags = mState->timerFlags;

    return static_cast<std::uint8_t>(flags | (flags != 0 ? 0x80U : 0U));
  }

  void Chip::Advance(std::uint64_t masterClocks) noexcept
  {
    mState->Advance(masterClocks);
  }

  void Chip::Render(std::span<std::int16_t> stereo)
  {
    if (stereo.size() % 2 != 0)
    {
      throw std::runtime_error("OPL output must contain complete stereo frames");
    }

    mState->Render(stereo);
  }
} // namespace havOPL
