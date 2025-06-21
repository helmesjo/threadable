#pragma once

namespace fho
{
  auto pin_to_core(int coreId) -> int;
  auto pin_to_core(void* thread, int coreId) -> int;
}
