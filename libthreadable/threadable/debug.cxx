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
}
