#include <threadable/std_concepts.hxx>
#include <threadable/function.hxx>
#include <threadable/pool.hxx>
#include <threadable/queue.hxx>

#include <threadable-benchmarks/util.hxx>
#include <benchmark/benchmark.h>

#if __has_include(<execution>)
#include <execution>
#endif
#include <functional>
#include <queue>
#include <thread>
#include <vector>

namespace
{
  int val;
  constexpr std::size_t jobs_per_iteration = 1 << 16;

  template<std::invocable block_t>
  inline void time_block(benchmark::State& state, block_t&& block)
  {
    auto start = std::chrono::high_resolution_clock::now();

    block();

    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed_seconds = std::chrono::duration_cast<std::chrono::duration<double>>(
        end - start);
    state.SetIterationTime(elapsed_seconds.count());
  }
}

static void threadable_function(benchmark::State& state)
{
  const std::size_t nr_of_jobs = state.range(0);
  using threadable_func_t = threadable::function<>;
  threadable_func_t func;
  for (auto _ : state)
  {
    time_block(state, [&]{
      for(std::size_t i = 0; i < nr_of_jobs; ++i)
      {
        func = []() mutable {
          benchmark::DoNotOptimize(val = threadable::benchmark::do_trivial_work(val));
        };
        func();
      }
    });
  }
  state.SetItemsProcessed(nr_of_jobs * state.iterations());
}

static void std_function(benchmark::State& state)
{
  const std::size_t nr_of_jobs = state.range(0);
  std::function<void()> func;
  for (auto _ : state)
  {
    time_block(state, [&]{
      for(std::size_t i = 0; i < nr_of_jobs; ++i)
      {
        func = []() mutable {
          benchmark::DoNotOptimize(val = threadable::benchmark::do_trivial_work(val));
        };
        func();
      }
    });
  }
  state.SetItemsProcessed(nr_of_jobs * state.iterations());
}
// function
BENCHMARK(threadable_function)->Args({jobs_per_iteration})->ArgNames({"jobs"})->UseManualTime();
BENCHMARK(std_function)->Args({jobs_per_iteration})->ArgNames({"jobs"})->UseManualTime();

static void threadable_queue(benchmark::State& state)
{
  const std::size_t nr_of_jobs = state.range(0);
  auto queue = std::make_shared<threadable::queue<jobs_per_iteration>>();
  for (auto _ : state)
  {
    for(std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      queue->push([]() mutable {
        benchmark::DoNotOptimize(threadable::benchmark::do_trivial_work(val));
      });
    }

    time_block(state, [&]{
      while(auto job = queue->steal())
      {
        job();
      }
    });
  }
  state.SetItemsProcessed(nr_of_jobs * state.iterations());
}

static void std_queue(benchmark::State& state)
{
  const std::size_t nr_of_jobs = state.range(0);
  std::queue<std::function<void()>> queue;

  for (auto _ : state)
  {
    for(std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      queue.emplace([]() mutable {
        benchmark::DoNotOptimize(val = threadable::benchmark::do_trivial_work(val));
      });
    }
    time_block(state, [&]{
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
// queue
BENCHMARK(threadable_queue)->Args({jobs_per_iteration})->ArgNames({"jobs"})->UseManualTime();
BENCHMARK(std_queue)->Args({jobs_per_iteration})->ArgNames({"jobs"})->UseManualTime();

static void std_thread(benchmark::State& state)
{
  const std::size_t nr_of_jobs = state.range(0);
  const std::size_t nr_of_threads = state.range(1);
  std::vector<threadable::job_token> tokens;
  tokens.reserve(nr_of_jobs);

  std::atomic_bool stop{false};
  auto queue = std::make_shared<threadable::queue<jobs_per_iteration>>();
  std::vector<std::thread> threads;
  for(std::size_t i=0; i<nr_of_threads; ++i)
  {
    threads.emplace_back([&]{
      while(!stop)
      {
        if(auto q = std::atomic_load(&queue))
        {
          while(auto job = q->steal())
          {
            job();
          }
        }
      }
    });
  }
  for (auto _ : state)
  {
    // pre-fill queue
    auto q = std::make_shared<threadable::queue<jobs_per_iteration>>();

    for(std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      tokens.emplace_back(q->push([&]() mutable {
        benchmark::DoNotOptimize(threadable::benchmark::do_trivial_work(val));
      }));
    }
    time_block(state, [&]{
      std::atomic_store(&queue, q);
      for(const auto& token : tokens)
      {
        token.wait();
      }
    });

    tokens.clear();
    q = nullptr;
    std::atomic_store(&queue, q);
  }

  stop = true;
  for(auto& thread : threads)
  {
    thread.join();
  }

  state.SetItemsProcessed(nr_of_jobs * state.iterations());
}
static void threadable_pool(benchmark::State& state)
{
  const std::size_t nr_of_jobs = state.range(0);
  const std::size_t nr_of_threads = state.range(1);
  auto pool = threadable::pool<jobs_per_iteration>(nr_of_threads);
  std::vector<threadable::job_token> tokens;
  tokens.reserve(nr_of_jobs);

  for (auto _ : state)
  {
    // pre-fill queue
    auto queue = std::make_shared<decltype(pool)::queue_t>(threadable::execution_policy::concurrent);

    for(std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      tokens.emplace_back(queue->push([&]() mutable {
        benchmark::DoNotOptimize(threadable::benchmark::do_trivial_work(val));
      }));
    }
    time_block(state, [&]{
      pool.add(queue);

      for(const auto& token : tokens)
      {
        token.wait();
      }
    });

    tokens.clear();
    pool.remove(*queue);
  }
  
  state.SetItemsProcessed(nr_of_jobs * state.iterations());
}
// thread pool
BENCHMARK(threadable_pool)->Args({jobs_per_iteration, 1})->ArgNames({"jobs", "threads"})->UseManualTime();
BENCHMARK(std_thread)->Args({jobs_per_iteration, 1})->ArgNames({"jobs", "threads"})->UseManualTime();

#if defined(__cpp_lib_execution) && defined(__cpp_lib_parallel_algorithm)

static void std_parallel_for(benchmark::State& state)
{
  const std::size_t nr_of_jobs = state.range(0);
  // const std::size_t nr_of_threads = state.range(1);
  std::vector<std::function<void()>> queue;
  queue.reserve(nr_of_jobs);

  for (auto _ : state)
  {
    // pre-fill queue
    for(std::size_t i = 0; i < nr_of_jobs; ++i)
    {
      queue.emplace_back([]() mutable {
        benchmark::DoNotOptimize(threadable::benchmark::do_trivial_work(val));
      });
    }

    time_block(state, [&]{
      std::for_each(std::execution::par, std::begin(queue), std::end(queue), [](auto& job){
        job();
      });
    });

    queue.clear();
  }
  state.SetItemsProcessed(nr_of_jobs * state.iterations());
}
BENCHMARK(std_parallel_for)->Args({jobs_per_iteration, std::thread::hardware_concurrency()})->ArgNames({"jobs", "threads"})->UseManualTime();

#endif

BENCHMARK_MAIN();

