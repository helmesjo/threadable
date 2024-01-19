#pragma once

namespace threadable::utils
{
  auto do_trivial_work(int& val) -> int;
  auto do_non_trivial_work(int& val) -> int;
}
