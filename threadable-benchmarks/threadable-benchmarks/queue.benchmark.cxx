#include <threadable/queue.hxx>

#include <threadable-benchmarks/util.hxx>

#include <algorithm>
#include <cstddef>
#if __has_include(<execution>)
#include <execution>
#endif
#include <functional>
#if __has_include (<pstld/pstld.h>)
    #include <pstld/pstld.h>
#endif
#include <queue>
#include <vector>

namespace
{
  constexpr std::size_t jobs_per_iteration = 1 << 16;
}

static void queue_threadable_iterate(benchmark::State& state)
{
  using job_t = threadable::job;
  const std::size_t nr_of_jobs = state.range(0);
  auto queue = threadable::queue2<jobs_per_iteration>();
  benchmark::DoNotOptimize(queue);

  for(std::size_t i = 0; i < nr_of_jobs; ++i)
  {
    queue.push([]() mutable {});
  }
  for (auto _ : state)
  {
    for(auto& job : queue)
    {
      benchmark::DoNotOptimize(job);
    }
  }
  state.SetItemsProcessed(nr_of_jobs * state.iterations());
  state.SetBytesProcessed(nr_of_jobs * sizeof(job_t) * state.iterations());
}

static void queue_std_iterate(benchmark::State& state)
{
  using job_t = std::function<void()>;
  const std::size_t nr_of_jobs = state.range(0);
  std::vector<job_t> queue;
  benchmark::DoNotOptimize(queue);

  for(std::size_t i = 0; i < nr_of_jobs; ++i)
  {
    queue.emplace_back([]() mutable {});
  }
  for (auto _ : state)
  {
    for(auto& job : queue)
    {
      benchmark::DoNotOptimize(job);
    }
  }
  state.SetItemsProcessed(nr_of_jobs * state.iterations());
  state.SetBytesProcessed(nr_of_jobs * sizeof(job_t) * state.iterations());
}

BENCHMARK(queue_threadable_iterate)->Args({jobs_per_iteration})->ArgNames({"jobs"});
BENCHMARK(queue_std_iterate)->Args({jobs_per_iteration})->ArgNames({"jobs"});

#if defined(__cpp_lib_execution) && defined(__cpp_lib_parallel_algorithm)

static void queue2_threadable_parallel_for(benchmark::State& state)
{
  using job_t = threadable::job;
  const std::size_t nr_of_jobs = state.range(0);
  auto queue = threadable::queue2<jobs_per_iteration>();
  benchmark::DoNotOptimize(queue);

  for(std::size_t i = 0; i < nr_of_jobs; ++i)
  {
    queue.push([]() mutable {});
  }
  for (auto _ : state)
  {
    std::for_each(std::execution::par, queue.begin(), queue.end(), [](auto& job) {
      benchmark::DoNotOptimize(job);
    });
  }
  state.SetItemsProcessed(nr_of_jobs * state.iterations());
  state.SetBytesProcessed(nr_of_jobs * sizeof(job_t) * state.iterations());
}
BENCHMARK(queue2_threadable_parallel_for)->Args({jobs_per_iteration})->ArgNames({"jobs"});

static void queue_std_parallel_for(benchmark::State& state)
{
  using job_t = std::function<void()>;
  const std::size_t nr_of_jobs = state.range(0);
  std::vector<job_t> queue;
  benchmark::DoNotOptimize(queue);

  for(std::size_t i = 0; i < nr_of_jobs; ++i)
  {
    queue.emplace_back([]() mutable {});
  }
  for (auto _ : state)
  {
    std::for_each(std::execution::par, std::begin(queue), std::end(queue), [](auto& job){
      benchmark::DoNotOptimize(job);
    });
  }
  state.SetItemsProcessed(nr_of_jobs * state.iterations());
  state.SetBytesProcessed(nr_of_jobs * sizeof(job_t) * state.iterations());
}
BENCHMARK(queue_std_parallel_for)->Args({jobs_per_iteration})->ArgNames({"jobs"});

#endif
