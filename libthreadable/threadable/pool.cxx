#include <threadable/pool.hxx>

#include <memory>

namespace fho::details
{
  auto
  pool() -> pool_t&
  {
    static auto once = std::once_flag{};
    static auto inst = std::unique_ptr<pool_t>{};
    std::call_once(once,
                   []
                   {
                     inst = std::make_unique<fho::details::pool_t>();
                     std::atexit(
                       []
                       {
                         inst = nullptr;
                       });
                   });
    return *inst;
  }
}
