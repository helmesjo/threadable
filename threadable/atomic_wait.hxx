#pragma once

#include <utility>
#include <thread>

#define FWD(...) ::std::forward<decltype(__VA_ARGS__)>(__VA_ARGS__)

namespace threadable::details
{
#if __cpp_lib_atomic_wait >= 201907 && !defined(__APPLE__)
  template<typename atomic_t, typename obj_t>
  inline void atomic_wait(atomic_t& atomic, obj_t&& old) noexcept
  {
    atomic.wait(FWD(old));
  }

  template<typename atomic_t>
  inline void atomic_notify_one(atomic_t& atomic) noexcept
  {
    atomic.notify_one();
  }

  template<typename atomic_t>
  inline void atomic_notify_all(atomic_t& atomic) noexcept
  {
    atomic.notify_all();
  }
#else
  template<typename atomic_t, typename obj_t>
  inline void atomic_wait(atomic_t& atomic, obj_t&& old) noexcept
  {
    while(atomic.load() == FWD(old)){ std::this_thread::yield(); }
  }

  template<typename atomic_t>
  inline void atomic_notify_one(atomic_t&) noexcept
  {
  }

  template<typename atomic_t>
  inline void atomic_notify_all(atomic_t&) noexcept
  {
  }
#endif
}

#undef FWD
