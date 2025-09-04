#include <threadable/debug.hxx>
#include <threadable/token.hxx>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <unistd.h>
#endif

// Macros for cross-platform function signature, file, and line
#if defined(__GNUC__) || defined(__clang__)
  #define FHO_FUNC __PRETTY_FUNCTION__
#elif defined(_MSC_VER)
  #define FHO_FUNC __FUNCSIG__
#else
  #define FHO_FUNC "unknown"
#endif

#define FHO_FILE __FILE__
#define FHO_LINE __LINE__

namespace fho::dbg
{
  // Detect terminal color support (lazy-evaluated static)
  auto
  is_tty_color() noexcept -> bool
  {
    static bool const val = []() noexcept -> bool
    {
#ifdef _WIN32
      HANDLE hStderr = GetStdHandle(STD_ERROR_HANDLE);
      if (hStderr == INVALID_HANDLE_VALUE)
        return false;
      DWORD mode = 0;
      if (!GetConsoleMode(hStderr, &mode))
        return false;
      // Enable VT processing if not already
      if (!SetConsoleMode(hStderr, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
      {
        return false;
      }
      return true;
#else
      return isatty(fileno(stderr)) != 0;
#endif
    }();
    return val;
  }
}
