#pragma once

#include <threadable/token.hxx>

#include <format>
#include <source_location>

template<>
struct std::formatter<fho::slot_state> : std::formatter<string_view>
{
  auto
  format(fho::slot_state v, std::format_context& ctx) const
  {
    switch (v)
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
    }
  }
};

namespace fho::dbg
{
  auto is_tty_color() noexcept -> bool;

  inline void
  verify(atomic_state_t& state, slot_state expected,
         std::source_location l = std::source_location::current()) noexcept
  {
#ifndef NDEBUG
    auto current = static_cast<slot_state>(state.load(std::memory_order_relaxed));
    if (current == expected) [[likely]]
    {
      return;
    }

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
      }
      return "unknown";
    };

    bool        color = is_tty_color();
    char const* red   = color ? "\033[1;31m" : "";
    char const* reset = color ? "\033[0m" : "";

    std::fprintf( // NOLINT
      stderr,
      "%sAssertion failed:%s state == expected (%s%s == %s%s), file %s, line %d:%d, function "
      "%s%s\n",
      red, reset, red, to_cstr(current), to_cstr(expected), reset, l.file_name(), l.line(),
      l.column(), l.function_name(), reset);
    std::fflush(stderr);

    std::abort();
#endif
  }
}
