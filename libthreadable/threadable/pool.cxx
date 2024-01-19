#include <threadable/pool.hxx>

namespace threadable::details
{
  auto
  pool() -> pool_t&
  {
    static pool_t pool = {};
    return pool;
  }
}
