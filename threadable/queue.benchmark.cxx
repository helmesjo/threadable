#include <threadable/function.hxx>
#include <threadable/pool.hxx>
#include <threadable/queue.hxx>

#include <benchmark/benchmark.h>

#include <atomic>
#include <functional>
#include <thread>
#include <queue>

namespace
{
  // just some random semi-heavy but consistent function
  std::size_t prime_total(std::size_t ubound)
  {
    std::size_t total = 0;
    std::size_t lbound = 0;
    while (lbound <= ubound)
    {
      bool found = false;
      for(std::size_t i = 2; i <= lbound/2; i++)
      {
        if(lbound % i == 0)
        {
          found = true;
          break;
        }
      }
      if (found == 0)
      {
        total += lbound;
      }
      lbound++;
    }

    return total;
  }
}

static void threadable_function(benchmark::State& state)
{
  using threadable_func_t = threadable::function<>;
  threadable_func_t func;
  for (auto _ : state)
  {
    int val = 0;
    func = [&val]{ ++val; };
    func();
    benchmark::DoNotOptimize(val);
  }
}

static void std_function(benchmark::State& state)
{
  std::function<void()> func;
  for (auto _ : state)
  {
    int val = 0;
    func = [&val]{ ++val; };
    func();
    benchmark::DoNotOptimize(val);
  }
}

static void threadable_queue(benchmark::State& state)
{
  threadable::queue queue;
  for (auto _ : state)
  {
    int val = 0;
    queue.push([&val]{ ++val; });
    auto job = queue.pop();
    job();
    benchmark::DoNotOptimize(val);
  }
}

static void std_queue(benchmark::State& state)
{
  std::queue<std::function<void()>> queue;
  for (auto _ : state)
  {
    int val = 0;
    queue.emplace([&val]{ ++val; });
    auto& job = queue.front();
    job();
    queue.pop();
    benchmark::DoNotOptimize(val);
  }
}

static void threadable_pool_4_threads(benchmark::State& state)
{
  static constexpr std::size_t big_val = 200;
  static constexpr std::size_t nr_of_iterations = 16;
  static constexpr std::size_t nr_of_threads = 4;

  auto pool = threadable::pool(nr_of_threads);
  std::queue<threadable::job_token> tokens;

  for (auto _ : state)
  {
    std::atomic_size_t val = 0;
    for(std::size_t i = 0; i < nr_of_iterations; ++i)
    {
      tokens.emplace(pool.push([&val]{
        val += prime_total(big_val);
      }));
    }

    while(!tokens.empty())
    {
      tokens.front().wait();
      tokens.pop();
    }

    benchmark::DoNotOptimize(val);
  }
}

static void std_no_pool_1_thread(benchmark::State& state)
{
  static constexpr std::size_t big_val = 200;
  static constexpr std::size_t nr_of_iterations = 16;

  std::function<void()> job;
  for (auto _ : state)
  {
    std::size_t val = 0;
    for(std::size_t i = 0; i < nr_of_iterations; ++i)
    {
      job = [&val](){ val += prime_total(big_val); };
      job();
    }

    benchmark::DoNotOptimize(val);
  }
}

BENCHMARK(threadable_function);
BENCHMARK(std_function);

BENCHMARK(threadable_queue);
BENCHMARK(std_queue);

BENCHMARK(threadable_pool_4_threads);
BENCHMARK(std_no_pool_1_thread);

BENCHMARK_MAIN();

