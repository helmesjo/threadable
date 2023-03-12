#include <threadable-benchmarks/util.hxx>

namespace
{
  int non_trivial_work(int& val)
  {
    int total = val;
    int lbound = 1;
    int ubound = 100;
    while (lbound <= ubound)
    {
      bool found = false;
      for(int i = 2; i <= lbound/2; i++)
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

  int trivial_work(int& val)
  {
    return val;
  }
}

namespace threadable::utils
{
  int do_trivial_work(int& val)
  {
    return val;
  }

  int do_non_trivial_work(int& val)
  {
    return non_trivial_work(val);
  }
}
