#pragma once

#include <threadable/token.hxx>

#include <atomic>
#include <source_location>

namespace fho::dbg
{
  auto is_tty_color() noexcept -> bool;

  inline void
  log(char const* pref, slot_state current, slot_state expected, // NOLINT
      std::source_location l = std::source_location::current())
  {
    static constexpr auto to_cstr = [](slot_state s)
    {
      switch (s)
      {
        case fho::invalid:
          return "invalid";
        case fho::empty:
          return "empty";
        case fho::ready:
          return "ready";
        case fho::locked:
          return "locked";
        case fho::locked_empty:
          return "locked_empty";
        case fho::locked_ready:
          return "locked_ready";
        case all:
          return "all";
      }
      return "unknown";
    };

    static bool        color = is_tty_color();
    static char const* red   = color ? "\033[0;31m" : "";
    static char const* bred  = color ? "\033[1;31m" : "";
    static char const* reset = color ? "\033[0m" : "";

    std::fprintf( // NOLINT
      stderr,
      "%s%sstate%s %s(%s)%s == expected%s %s(%s)%s, file %s, line %d:%d, function "
      "%s%s\n",
      pref, bred, reset, red, to_cstr(current), bred, reset, red, to_cstr(expected), reset,
      l.file_name(), l.line(), l.column(), l.function_name(), reset);
    std::fflush(stderr);
  }

  inline void
  verify(slot_state current, slot_state expected, // NOLINT
         std::source_location l = std::source_location::current()) noexcept
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

  template<typename T>
  inline void
  verify(T& current, slot_state expected,
         std::source_location l = std::source_location::current()) noexcept
    requires requires (T& c) { c.load(std::memory_order_relaxed); }
  {
    verify(static_cast<slot_state>(current.load(std::memory_order_relaxed)), expected, l);
  }
}
