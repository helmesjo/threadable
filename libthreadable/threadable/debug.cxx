#include <threadable/debug.hxx>
#include <threadable/token.hxx>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <io.h>
#else
  #include <unistd.h>
#endif

namespace fho::dbg
{
  // Detect terminal color support (lazy-evaluated static)
  auto
  is_tty_color() noexcept -> bool
  {
    static bool const val = []() noexcept -> bool
    {
#ifdef _WIN32
      return _isatty(_fileno(stderr)) != 0;
#else
      return isatty(fileno(stderr)) != 0;
#endif
    }();
    return val;
  }

  auto
  to_str(slot_state s) noexcept -> std::string
  {
    std::string result;

    bool first  = true;
    auto append = [&](char const* str)
    {
      if (!first)
      {
        result += "|";
      }
      result += str;
      first = false;
    };

    if (s == fho::invalid)
    {
      return "invalid";
    }
    if (s == fho::empty)
    {
      append("empty");
    }
    if (s & fho::ready)
    {
      append("ready");
    }
    if (s & fho::locked)
    {
      append("locked");
    }
    if (s & fho::tag_seq)
    {
      append("sequential");
    }

    return result.empty() ? "unknown" : result;
  }
}
