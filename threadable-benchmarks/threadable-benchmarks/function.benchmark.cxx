#include <threadable/function.hxx>

#include <threadable-benchmarks/util.hxx>
#include <benchmark/benchmark.h>

namespace
{
  int val;
  constexpr std::size_t jobs_per_iteration = 1 << 16;
}

static void function_threadable_instantiate(benchmark::State& state)
{
  using func_t = threadable::function<>;
  for (auto _ : state)
  {
    benchmark::DoNotOptimize(func_t([]() mutable {}));
  }
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(sizeof(func_t) * state.iterations());
}

static void function_threadable_invoke(benchmark::State& state)
{
  using func_t = threadable::function<>;
  const std::size_t nr_of_jobs = state.range(0);
  auto func = func_t([]() mutable {
    benchmark::DoNotOptimize(val = threadable::utils::do_trivial_work(val));
  });
  benchmark::DoNotOptimize(func);
  for (auto _ : state)
  {
    func();
  }
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(sizeof(func_t) * state.iterations());
}

static void function_std_instantiate(benchmark::State& state)
{
  using func_t = std::function<void()>;
  for (auto _ : state)
  {
    benchmark::DoNotOptimize(func_t([]() mutable {}));
  }
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(sizeof(func_t) * state.iterations());
}

static void function_std_invoke(benchmark::State& state)
{
  using func_t = std::function<void()>;
  const std::size_t nr_of_jobs = state.range(0);
  auto func = func_t([]() mutable {
    benchmark::DoNotOptimize(val = threadable::utils::do_trivial_work(val));
  });
  benchmark::DoNotOptimize(func);
  for (auto _ : state)
  {
    func();
  }
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(sizeof(func_t) * state.iterations());
}


BENCHMARK(function_threadable_instantiate)->Args({jobs_per_iteration})->ArgNames({"jobs"});
BENCHMARK(function_threadable_invoke)->Args({jobs_per_iteration})->ArgNames({"jobs"});
BENCHMARK(function_std_instantiate)->Args({jobs_per_iteration})->ArgNames({"jobs"});
BENCHMARK(function_std_invoke)->Args({jobs_per_iteration})->ArgNames({"jobs"});
