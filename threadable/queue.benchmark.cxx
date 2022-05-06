#include <threadable/queue.hxx>
#include <benchmark/benchmark.h>

#include <functional>
#include <queue>

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

BENCHMARK(threadable_function);
BENCHMARK(std_function);

BENCHMARK(threadable_queue);
BENCHMARK(std_queue);

BENCHMARK_MAIN();
