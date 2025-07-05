#include <threadable/pool.hxx>

// #include <iostream>
#include <memory>

namespace fho::details
{
  auto
  pool() -> pool_t&
  {
    static auto once = std::once_flag{};
    std::call_once(once,
                   []
                   {
                     default_pool = std::make_unique<fho::details::pool_t>(); // NOLINT
                     std::atexit(
                       []
                       {
                         default_pool = nullptr;
                       });
                   });
    return *default_pool;
  }
}
