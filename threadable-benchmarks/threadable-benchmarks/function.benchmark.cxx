#include <threadable/function.hxx>

#include <threadable-benchmarks/util.hxx>
#include <benchmark/benchmark.h>

namespace
{
  int val;
  constexpr std::size_t jobs_per_iteration = 1 << 16;
}

static void function_threadable(benchmark::State& state)
{
  const std::size_t nr_of_jobs = state.range(0);
  using threadable_func_t = threadable::function<>;
  threadable_func_t func;
  for (auto _ : state)
  {
    threadable::utils::time_block(state, [&]{
      for(std::size_t i = 0; i < nr_of_jobs; ++i)
      {
        func = []() mutable {
          benchmark::DoNotOptimize(val = threadable::utils::do_trivial_work(val));
        };
        func();
      }
    });
  }
  state.SetItemsProcessed(nr_of_jobs * state.iterations());
}

static void function_std(benchmark::State& state)
{
  const std::size_t nr_of_jobs = state.range(0);
  std::function<void()> func;
  for (auto _ : state)
  {
    threadable::utils::time_block(state, [&]{
      for(std::size_t i = 0; i < nr_of_jobs; ++i)
      {
        func = []() mutable {
          benchmark::DoNotOptimize(val = threadable::utils::do_trivial_work(val));
        };
        func();
      }
    });
  }
  state.SetItemsProcessed(nr_of_jobs * state.iterations());
}

BENCHMARK(function_threadable)->Args({jobs_per_iteration})->ArgNames({"jobs"})->UseManualTime();
BENCHMARK(function_std)->Args({jobs_per_iteration})->ArgNames({"jobs"})->UseManualTime();
