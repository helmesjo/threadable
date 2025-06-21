#include <threadable/affinity.hxx>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <errno.h>
  #include <pthread.h>
  #ifdef __linux__
    #include <sched.h>
  #elif defined(__APPLE__)
    #include <mach/thread_act.h>
    #include <mach/thread_policy.h>
  #endif
#endif

namespace fho
{
  // Pin current thread to a specific core
  auto
  pin_to_core(int coreId) -> int
  {
#ifdef _WIN32
    HANDLE    thread = GetCurrentThread();
    DWORD_PTR mask   = (DWORD_PTR)1 << coreId;
    if (SetThreadAffinityMask(thread, mask) == 0)
    {
      return GetLastError(); // NOLINT
    }
    return 0;
#else
  #ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(coreId, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0)
    {
      return errno;
    }
    return 0;
  #elif defined(__APPLE__)
    thread_affinity_policy_data_t policy = {coreId};
    thread_port_t                 thread = mach_thread_self();
    kern_return_t                 ret =
      thread_policy_set(thread, THREAD_AFFINITY_POLICY, (thread_policy_t)&policy, 1);
    mach_port_deallocate(mach_task_self(), thread);
    if (ret != KERN_SUCCESS)
    {
      return EINVAL; // macOS doesn't provide specific errno
    }
    return 0;
  #else
    return ENOTSUP; // Unsupported platform
  #endif
#endif
  }

  // Pin a specific thread to a core
  auto
  pin_to_core(void* thread, int coreId) -> int
  {
#ifdef _WIN32
    auto      hThread = (HANDLE)thread;
    DWORD_PTR mask    = (DWORD_PTR)1 << coreId;
    if (SetThreadAffinityMask(hThread, mask) == 0)
    {
      return GetLastError(); // NOLINT
    }
    return 0;
#else
  #ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(coreId, &cpuset);
    if (pthread_setaffinity_np((pthread_t)thread, sizeof(cpu_set_t), &cpuset) != 0)
    {
      return errno;
    }
    return 0;
  #elif defined(__APPLE__)
    thread_affinity_policy_data_t policy      = {coreId};
    thread_port_t                 mach_thread = pthread_mach_thread_np((pthread_t)thread);
    kern_return_t                 ret =
      thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY, (thread_policy_t)&policy, 1);
    if (ret != KERN_SUCCESS)
    {
      return EINVAL;
    }
    return 0;
  #else
    return ENOTSUP; // Unsupported platform
  #endif
#endif
  }
}
