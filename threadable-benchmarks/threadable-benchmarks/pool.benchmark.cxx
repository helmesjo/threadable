#include <threadable/pool.hxx>

#include <threadable-benchmarks/util.hxx>

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
}

static void pool_std_thread(benchmark::State& state)
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
        benchmark::DoNotOptimize(threadable::utils::do_trivial_work(val));
      }));
    }
    threadable::utils::time_block(state, [&]{
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

static void pool_threadable(benchmark::State& state)
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
        benchmark::DoNotOptimize(threadable::utils::do_trivial_work(val));
      }));
    }
    threadable::utils::time_block(state, [&]{
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

#if defined(__cpp_lib_execution) && defined(__cpp_lib_parallel_algorithm)

static void pool_std_parallel_for(benchmark::State& state)
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
        benchmark::DoNotOptimize(threadable::utils::do_trivial_work(val));
      });
    }

    threadable::utils::time_block(state, [&]{
      std::for_each(std::execution::par, std::begin(queue), std::end(queue), [](auto& job){
        job();
      });
    });

    queue.clear();
  }
  state.SetItemsProcessed(nr_of_jobs * state.iterations());
}

BENCHMARK(pool_std_parallel_for)->Args({jobs_per_iteration, std::thread::hardware_concurrency()})->ArgNames({"jobs", "threads"})->UseManualTime();
#endif

BENCHMARK(pool_threadable)->Args({jobs_per_iteration, 1})->ArgNames({"jobs", "threads"})->UseManualTime();
BENCHMARK(pool_std_thread)->Args({jobs_per_iteration, 1})->ArgNames({"jobs", "threads"})->UseManualTime();

