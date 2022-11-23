#include <threadable/queue.hxx>

#include <threadable-benchmarks/util.hxx>

#include <algorithm>
#include <cstddef>
#if __has_include(<execution>)
#include <execution>
#endif
#include <functional>
#include <vector>
#include <queue>

namespace
{
  constexpr std::size_t jobs_per_iteration = 1 << 16;
}

static void queue_threadable_pop(benchmark::State& state)
{
  const std::size_t nr_of_jobs = state.range(0);
  auto queue = threadable::queue<jobs_per_iteration>();
  for (auto _ : state)
  {
    for(std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      queue.push([]() mutable {});
    }

    threadable::utils::time_block(state, [&]{
      while(auto job = queue.pop())
      {
        benchmark::DoNotOptimize(job);
      }
    });
  }
  state.SetItemsProcessed(nr_of_jobs * state.iterations());
}

static void queue_threadable_steal(benchmark::State& state)
{
  const std::size_t nr_of_jobs = state.range(0);
  auto queue = threadable::queue<jobs_per_iteration>();
  for (auto _ : state)
  {
    for(std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      queue.push([]() mutable {});
    }

    threadable::utils::time_block(state, [&]{
      while(!queue.empty())
      {
        if(auto job = queue.steal())
        {
          benchmark::DoNotOptimize(job);
        }
      }
    });
  }
  state.SetItemsProcessed(nr_of_jobs * state.iterations());
}

static void queue_std(benchmark::State& state)
{
  const std::size_t nr_of_jobs = state.range(0);
  std::queue<std::function<void()>> queue;

  for (auto _ : state)
  {
    for(std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      queue.emplace([]() mutable {});
    }
    threadable::utils::time_block(state, [&]{
      while(!queue.empty())
      {
        auto& job = queue.front();
        benchmark::DoNotOptimize(job);
        queue.pop();
      }
    });
  }
  state.SetItemsProcessed(nr_of_jobs * state.iterations());
}

BENCHMARK(queue_threadable_pop)->Args({jobs_per_iteration})->ArgNames({"jobs"})->UseManualTime();
BENCHMARK(queue_threadable_steal)->Args({jobs_per_iteration})->ArgNames({"jobs"})->UseManualTime();
BENCHMARK(queue_std)->Args({jobs_per_iteration})->ArgNames({"jobs"})->UseManualTime();

#if defined(__cpp_lib_execution) && defined(__cpp_lib_parallel_algorithm)

static void queue_threadable_parallel_for(benchmark::State& state)
{
  const std::size_t nr_of_jobs = state.range(0);
  auto queue = threadable::queue<jobs_per_iteration>();
  for (auto _ : state)
  {
    for(std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      queue.push([]() mutable {});
    }

    threadable::utils::time_block(state, [&]{
      std::for_each(std::execution::par, std::begin(queue), std::end(queue), [](auto& job){
        if(job)
        {
          benchmark::DoNotOptimize(job);
          job(); // BUG: Withuot invocation here it locks in "queue::quit()". Verify destructors are called correctly.
        }
      });
    });
  }
  state.SetItemsProcessed(nr_of_jobs * state.iterations());
}
BENCHMARK(queue_threadable_parallel_for)->Args({jobs_per_iteration})->ArgNames({"jobs"})->UseManualTime();

static void queue_std_parallel_for(benchmark::State& state)
{
  const std::size_t nr_of_jobs = state.range(0);
  std::vector<std::function<void()>> queue;

  for (auto _ : state)
  {
    for(std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      queue.emplace_back([]() mutable {});
    }
    threadable::utils::time_block(state, [&]{
      std::for_each(std::execution::par, std::begin(queue), std::end(queue), [](auto& job){
        if(job)
        {
          benchmark::DoNotOptimize(job);
          job();
        }
      });
    });
    queue.clear();
  }
  state.SetItemsProcessed(nr_of_jobs * state.iterations());
}
BENCHMARK(queue_std_parallel_for)->Args({jobs_per_iteration})->ArgNames({"jobs"})->UseManualTime();

#endif
