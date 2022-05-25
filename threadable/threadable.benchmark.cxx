#include <threadable/function.hxx>
#include <threadable/pool.hxx>
#include <threadable/queue.hxx>

#include <threadable/threadable.benchmark.util.hxx>
#include <benchmark/benchmark.h>

#include <functional>
#include <queue>
#include <vector>

static void threadable_function(benchmark::State& state)
{
  using threadable_func_t = threadable::function<>;
  threadable_func_t func;
  for (auto _ : state)
  {
    func = [val = 0]() mutable {
      benchmark::DoNotOptimize(threadable::benchmark::do_work(val));
    };
    func();
  }
  state.SetItemsProcessed(state.iterations());
}

static void std_function(benchmark::State& state)
{
  std::function<void()> func;
  for (auto _ : state)
  {
    func = [val = 0]() mutable {
      benchmark::DoNotOptimize(threadable::benchmark::do_work(val));
    };
    func();
  }
  state.SetItemsProcessed(state.iterations());
}

static void threadable_queue(benchmark::State& state)
{
  threadable::queue queue;
  for (auto _ : state)
  {
    queue.push([val = 0]() mutable {
      benchmark::DoNotOptimize(threadable::benchmark::do_work(val));
    });
    auto job = queue.steal();
    job();
  }
  state.SetItemsProcessed(state.iterations());
}

static void std_queue(benchmark::State& state)
{
  std::queue<std::function<void()>> queue;

  for (auto _ : state)
  {
    queue.emplace([val = 0]() mutable {
      benchmark::DoNotOptimize(threadable::benchmark::do_work(val));
    });
    auto& job = queue.front();
    job();
    queue.pop();
  }
  state.SetItemsProcessed(state.iterations());
}

static void threadable_pool(benchmark::State& state, std::size_t nr_of_threads)
{
  auto pool = threadable::pool(nr_of_threads);
  std::vector<threadable::job_token> tokens;
  tokens.reserve(state.range(0));

  for (auto _ : state)
  {
    for(std::int64_t i = 0; i < state.range(0); ++i)
    {
      tokens.emplace_back(pool.push([val = 0]() mutable {
        benchmark::DoNotOptimize(threadable::benchmark::do_work(val));
      }));
    }

    for(const auto& token : tokens)
    {
      token.wait();
    }

    state.PauseTiming();
    tokens.clear();
    state.ResumeTiming();
  }
  
  state.SetItemsProcessed(state.range(0) * state.iterations());
}

static void std_no_pool(benchmark::State& state)
{
  std::vector<std::function<void()>> queue;
  queue.reserve(state.range(0));

  for (auto _ : state)
  {
    for(std::int64_t i = 0; i < state.range(0); ++i)
    {
      queue.emplace_back([val = 0]() mutable {
        benchmark::DoNotOptimize(threadable::benchmark::do_work(val));
      });
    }

    for(auto& job : queue)
    {
      job();
    }

    state.PauseTiming();
    queue.clear();
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.range(0) * state.iterations());
}

BENCHMARK(threadable_function);
BENCHMARK(std_function);

BENCHMARK(threadable_queue);
BENCHMARK(std_queue);

static constexpr std::size_t jobs_per_iteration = 1024;
BENCHMARK_CAPTURE(threadable_pool, threads: 1, 1)->Arg(jobs_per_iteration);
BENCHMARK_CAPTURE(threadable_pool, threads: 2, 2)->Arg(jobs_per_iteration);
BENCHMARK_CAPTURE(threadable_pool, threads: 3, 3)->Arg(jobs_per_iteration);
BENCHMARK(std_no_pool)->Arg(jobs_per_iteration);

BENCHMARK_MAIN();

