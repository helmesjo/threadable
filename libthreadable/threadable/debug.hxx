#pragma once

#include <threadable/token.hxx>

#include <atomic>
#include <mutex>
#include <source_location>

namespace fho::dbg
{
  auto is_tty_color() noexcept -> bool;
  auto to_str(slot_state s) noexcept -> std::string;

  inline void
  log(char const* pref, slot_state current, slot_state expected, // NOLINT
      std::source_location l = std::source_location::current())
  {
    static bool        color = is_tty_color();
    static char const* red   = color ? "\033[0;31m" : "";
    static char const* bred  = color ? "\033[1;31m" : "";
    static char const* reset = color ? "\033[0m" : "";

    static auto mut  = std::mutex{};
    auto        _    = std::scoped_lock{mut};
    auto        curr = to_str(current);
    auto        exp  = to_str(expected);
    std::fprintf( // NOLINT
      stderr,
      "%s%s(%s) %sstate == expected%s (%s)%s, file %s, line %d:%d, function "
      "%s%s\n",
      pref, red, curr.c_str(), bred, red, exp.c_str(), reset, l.file_name(), l.line(), l.column(),
      l.function_name(), reset);
    std::fflush(stderr);
  }

  inline void
  verify([[maybe_unused]] slot_state current, [[maybe_unused]] slot_state expected, // NOLINT
         [[maybe_unused]] std::source_location l = std::source_location::current()) noexcept
  {
#ifndef NDEBUG
    if (current == expected) [[likely]]
    {
      return;
    }

    log("Assertion failed: ", current, expected, l);
    std::abort();
#endif
  }

  template<slot_state Mask = slot_state::all_mask, typename T>
  inline void
  verify(T const& current, slot_state expected,
         std::source_location l = std::source_location::current()) noexcept
    requires requires (T& c) { c.load(std::memory_order_relaxed); }
  {
    verify(static_cast<slot_state>(current.load(std::memory_order_relaxed) & Mask), expected, l);
  }

  inline void
  verify_bitwise([[maybe_unused]] slot_state current, [[maybe_unused]] slot_state mask, // NOLINT
                 [[maybe_unused]] std::source_location l = std::source_location::current()) noexcept
  {
#ifndef NDEBUG
    if ((current & mask) != 0) [[likely]]
    {
      return;
    }

    log("Assertion failed: ", current, mask, l);
    std::abort();
#endif
  }

  template<typename T>
  inline void
  verify_bitwise(T& current, slot_state mask,
                 std::source_location l = std::source_location::current()) noexcept
    requires requires (T& c) { c.load(std::memory_order_relaxed); }
  {
    verify_bitwise(static_cast<slot_state>(current.load(std::memory_order_relaxed)), mask, l);
  }
}
