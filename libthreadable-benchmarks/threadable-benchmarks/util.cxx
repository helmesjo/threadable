#include <threadable-benchmarks/util.hxx>

namespace
{
  auto
  trivial_work(int& val) -> int
  {
    return val;
  }

  auto
  non_trivial_work(int& val) -> int
  {
    int total  = val;
    int lbound = 1;
    int ubound = 100;
    while (lbound <= ubound)
    {
      bool found = false;
      for (int i = 2; i <= lbound / 2; i++)
      {
        if (lbound % i == 0)
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
}

namespace fho::utils
{
  auto
  do_trivial_work(int& val) -> int
  {
    return trivial_work(val);
  }

  auto
  do_non_trivial_work(int& val) -> int
  {
    return non_trivial_work(val);
  }
}
