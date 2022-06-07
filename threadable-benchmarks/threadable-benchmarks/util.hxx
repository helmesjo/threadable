#pragma once

#include <threadable/export.hxx>

namespace threadable::benchmark
{
  THREADABLE_SYMEXPORT int do_trivial_work(int& val);
  THREADABLE_SYMEXPORT int do_non_trivial_work(int& val);
}
