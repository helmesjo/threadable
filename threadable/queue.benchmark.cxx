#include <threadable/function.hxx>
#include <threadable/pool.hxx>
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

static void threadable_pool(benchmark::State& state)
{
  static constexpr std::size_t nr_of_jobs = 1 << 16;
  static constexpr std::size_t nr_of_threads = 8;
  auto pool = threadable::pool<nr_of_jobs>(nr_of_threads);
  std::vector<threadable::job_token> tokens;
  tokens.reserve(nr_of_jobs);

  for (auto _ : state)
  {
    int val = 0;
    for(std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      tokens.push_back(pool.push([&val]{ ++val; }));
    }

    for(const auto& token : tokens)
    {
      token.wait();
    }

    benchmark::DoNotOptimize(val);
  }
}

static void std_no_pool(benchmark::State& state)
{
  static constexpr std::size_t nr_of_jobs = 1 << 16;
  std::queue<std::function<void()>> queue;

  for (auto _ : state)
  {
    int val = 0;
    for(std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      queue.emplace([&val]{ ++val; });
    }
    while(!queue.empty())
    {
      auto& job = queue.front();
      job();
      queue.pop();
    }

    benchmark::DoNotOptimize(val);
  }
}

BENCHMARK(threadable_function);
BENCHMARK(std_function);

BENCHMARK(threadable_queue);
BENCHMARK(std_queue);

BENCHMARK(threadable_pool);
BENCHMARK(std_no_pool);

BENCHMARK_MAIN();
