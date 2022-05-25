#include <threadable/threadable.benchmark.util.hxx>
#include <cstdlib>

namespace threadable::benchmark
{
  int do_work(int& val)
  {
    return val += std::rand();
  }
}
