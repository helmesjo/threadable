#include <threadable/pool.hxx>

namespace threadable::details
{
  pool_t& pool()
  {
    static pool_t pool_ = {};
    return pool_;
  }
}
