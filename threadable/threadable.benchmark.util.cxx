#include <threadable/threadable.benchmark.util.hxx>

#include <cstddef>

namespace
{
  std::size_t non_trivial_work(auto& val)
  {
    std::size_t total = val;
    std::size_t lbound = 1;
    std::size_t ubound = 100;
    while (lbound <= ubound)
    {
      bool found = false;
      for(std::size_t i = 2; i <= lbound/2; i++)
      {
        if(lbound % i == 0)
        {
          found = true;
          break;
        }
      }
      if (found == 0)
      {
        total += lbound;
      }
      lbound++;
    }

    return total;
  }

  std::size_t trivial_work(auto& val)
  {
    return val;
  }
}

namespace threadable::benchmark
{
  int do_trivial_work(int& val)
  {
    return trivial_work(val);
  }

  int do_non_trivial_work(int& val)
  {
    return non_trivial_work(val);
  }
}
