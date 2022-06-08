#include <threadable/queue.hxx>

#include <threadable-benchmarks/util.hxx>

#include <cstddef>
#include <functional>
#include <queue>

namespace
{
  int val;
  constexpr std::size_t jobs_per_iteration = 1 << 16;
}

static void queue_threadable(benchmark::State& state)
{
  const std::size_t nr_of_jobs = state.range(0);
  auto queue = std::make_shared<threadable::queue<jobs_per_iteration>>();
  for (auto _ : state)
  {
    for(std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      queue->push([]() mutable {
        benchmark::DoNotOptimize(threadable::utils::do_trivial_work(val));
      });
    }

    threadable::utils::time_block(state, [&]{
      while(auto job = queue->steal())
      {
        job();
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
      queue.emplace([]() mutable {
        benchmark::DoNotOptimize(val = threadable::utils::do_trivial_work(val));
      });
    }
    threadable::utils::time_block(state, [&]{
      while(!queue.empty())
      {
        auto& job = queue.front();
        job();
        queue.pop();
      }
    });
  }
  state.SetItemsProcessed(nr_of_jobs * state.iterations());
}

BENCHMARK(queue_threadable)->Args({jobs_per_iteration})->ArgNames({"jobs"})->UseManualTime();
BENCHMARK(queue_std)->Args({jobs_per_iteration})->ArgNames({"jobs"})->UseManualTime();
