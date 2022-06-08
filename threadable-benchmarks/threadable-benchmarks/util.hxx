#pragma once

#include <benchmark/benchmark.h>

#include <chrono>
#include <concepts>

namespace threadable::utils
{
  template<std::invocable block_t>
  static void time_block(benchmark::State& state, block_t&& block)
  {
    auto start = std::chrono::high_resolution_clock::now();

    block();

    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed_seconds = std::chrono::duration_cast<std::chrono::duration<double>>(
        end - start);
    state.SetIterationTime(elapsed_seconds.count());
  }

  int do_trivial_work(int& val);
  int do_non_trivial_work(int& val);
}
