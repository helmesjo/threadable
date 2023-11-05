#include <threadable/pool.hxx>

namespace threadable::details
{
  extern threadable::pool<(1 << 22)>& pool()
  {
    static threadable::pool<(1 << 22)> pool_ = {};
    return pool_;
  }
}
