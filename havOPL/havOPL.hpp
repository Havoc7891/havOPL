// SPDX-License-Identifier: MIT

#ifndef HAVOPL_HPP
#define HAVOPL_HPP

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace havOPL
{
  inline constexpr std::uint32_t VersionMajor = 0;
  inline constexpr std::uint32_t VersionMinor = 1;
  inline constexpr std::uint32_t VersionPatch = 0;
  inline constexpr std::string_view VersionString = "0.1.0";

  // Original, register-driven OPL2/OPL3 synthesis. The implementation models the
  // documented musical functions rather than individual chip clock cycles.
  class Chip
  {
  public:
    explicit Chip(std::uint32_t sampleRate = 48000);
    ~Chip();

    Chip(Chip &&) noexcept;
    Chip &operator=(Chip &&) noexcept;
    Chip(const Chip &) = delete;
    Chip &operator=(const Chip &) = delete;

    void Reset();

    void Write(std::uint16_t reg, std::uint8_t value);

    // Interleaved left/right samples. An odd sample count is an error. Rendering
    // also advances the timers by the rendered duration.
    void Render(std::span<std::int16_t> stereo);

    // The status register (address port read): bit 7 IRQ, bit 6 timer 1 overflowed,
    // bit 5 timer 2 overflowed. The other bits read 0. Timer 1 counts in 80 us
    // steps, timer 2 in 320 us steps, from registers 0x02/0x03. Register 0x04
    // starts (bits 0/1) and masks (bits 6/5) them, and bit 7 resets the flags.
    [[nodiscard]] std::uint8_t Status() const noexcept;

    // Advances the timers without rendering in 14.31818 MHz master clocks (the
    // chip runs at a quarter of that). For hosts that poll the status register
    // between renders such as driver detection loops.
    void Advance(std::uint64_t masterClocks) noexcept;

  private:
    struct State;
    std::unique_ptr<State> mState;
  };
} // namespace havOPL

#endif // HAVOPL_HPP
