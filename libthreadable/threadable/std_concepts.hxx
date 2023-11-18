#pragma once

#include <concepts>
#include <version>

// clang with libc++ on mac or webassembly known to be missing some required concepts
#if defined(__clang__) && defined(_LIBCPP_VERSION)
#if ((__clang_major__ < 13 || (__clang_major__ == 13 && __clang_minor__ == 0)) && (defined(__APPLE__) || defined(__EMSCRIPTEN__))) || __clang_major__ < 13
#include <functional>

namespace std
{
    // Credit: https://en.cppreference.com
    template<class F, class... Args>
    concept invocable =
        requires(F && f, Args&&... args) {
        std::invoke(std::forward<F>(f), std::forward<Args>(args)...);
    };
}
#endif
#endif
