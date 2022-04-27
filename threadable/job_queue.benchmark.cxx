#include <threadable/job_queue.hxx>
#include <benchmark/benchmark.h>

#include <functional>
#include <queue>

static void job_queue(benchmark::State& state)
{
  threadable::job_queue queue;
  for (auto _ : state)
  {
    int val = 0;
    queue.push([&val]{ ++val; });
    auto& job = queue.pop();
    job();
    benchmark::DoNotOptimize(job);
    benchmark::DoNotOptimize(val);
  }
}

static void std_function(benchmark::State& state)
{
  for (auto _ : state)
  {
    int val = 0;
    auto job = std::function<void()>{[&val]{ ++val; }};
    job();
    benchmark::DoNotOptimize(job);
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
    benchmark::DoNotOptimize(job);
    benchmark::DoNotOptimize(val);
  }
}

BENCHMARK(job_queue);
BENCHMARK(std_function);
BENCHMARK(std_queue);

BENCHMARK_MAIN();
