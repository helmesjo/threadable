#include <threadable/pool.hxx>
#include <threadable-benchmarks/util.hxx>

#include <nanobench.h>
#include <doctest/doctest.h>

#include <algorithm>
#if __has_include(<execution>)
#include <execution>
#endif
#include <functional>
#if __has_include (<pstld/pstld.h>)
    #include <pstld/pstld.h>
#endif
#include <vector>

namespace bench = ankerl::nanobench;

namespace
{
  constexpr std::size_t jobs_per_iteration = 1 << 16;
  int val = 1;
}

TEST_CASE("pool: benchmark")
{
  bench::Bench b;
  b.relative(true)
   .minEpochIterations(1000)
   .batch(jobs_per_iteration);

  using job_t = decltype([](){
    bench::doNotOptimizeAway(val = threadable::utils::do_trivial_work(val) );
  });

  b.title("execute");
  // {
  //   auto queue = std::vector<std::function<void()>>();
  //   queue.resize(jobs_per_iteration, job_t{});

  //   b.run("std::vector", [&] {
  //     std::for_each(std::execution::seq, std::begin(queue), std::end(queue), [](auto& job){
  //       bench::doNotOptimizeAway(job);
  //     });
  //   });
  // }
  {
    auto pool = threadable::pool<jobs_per_iteration>(0);
    std::vector<threadable::job_token> tokens;
    tokens.reserve(jobs_per_iteration);
    auto queue = pool.create();

    b.run("threadable::queue", [&] {
      // TODO: Add support for pushing batch of jobs
      for(std::size_t i=0; i<jobs_per_iteration; ++i)
      {
        tokens.emplace_back(queue->push(job_t{}));
      }
      for(const auto& token : tokens)
      {
        token.wait();
      }
    });
  }
}

// static void pool_std_thread(benchmark::State& state)
// {
//   const std::size_t nr_of_jobs = state.range(0);
//   const std::size_t nr_of_threads = state.range(1);
//   std::vector<threadable::job_token> tokens;
//   tokens.reserve(nr_of_jobs);

//   std::atomic_bool stop{false};
//   auto queue = std::make_shared<threadable::queue<jobs_per_iteration>>();
//   std::vector<std::thread> threads;
//   for(std::size_t i=0; i<nr_of_threads; ++i)
//   {
//     threads.emplace_back([&]{
//       while(!stop)
//       {
//         if(auto q = std::atomic_load(&queue))
//         {
//           while(auto job = q->steal())
//           {
//             benchmark::DoNotOptimize(job);
//           }
//         }
//       }
//     });
//   }
//   for (auto _ : state)
//   {
//     // pre-fill queue
//     auto q = std::make_shared<threadable::queue<jobs_per_iteration>>();

//     for(std::size_t i = 0; i < nr_of_jobs; ++i)
//     {
//       tokens.emplace_back(q->push([&]() mutable {}));
//     }
//     threadable::utils::time_block(state, [&]{
//       std::atomic_store(&queue, q);
//       for(const auto& token : tokens)
//       {
//         token.wait();
//       }
//     });

//     tokens.clear();
//     q = nullptr;
//     std::atomic_store(&queue, q);
//   }

//   stop = true;
//   for(auto& thread : threads)
//   {
//     thread.join();
//   }

//   state.SetItemsProcessed(nr_of_jobs * state.iterations());
// }

// static void pool_threadable(benchmark::State& state)
// {
//   const std::size_t nr_of_jobs = state.range(0);
//   const std::size_t nr_of_threads = state.range(1);
//   auto pool = threadable::pool<jobs_per_iteration>(nr_of_threads);
//   std::vector<threadable::job_token> tokens;
//   tokens.reserve(nr_of_jobs);

//   for (auto _ : state)
//   {
//     // pre-fill queue
//     auto queue = std::make_shared<decltype(pool)::queue_t>(threadable::execution_policy::concurrent);

//     for(std::size_t i = 0; i < nr_of_jobs; ++i)
//     {
//       tokens.emplace_back(queue->push([&]() mutable {}));
//     }
//     threadable::utils::time_block(state, [&]{
//       pool.add(queue);

//       for(const auto& token : tokens)
//       {
//         token.wait();
//       }
//     });

//     tokens.clear();
//     pool.remove(*queue);
//   }
  
//   state.SetItemsProcessed(nr_of_jobs * state.iterations());
// }

// BENCHMARK(pool_threadable)->Args({jobs_per_iteration, 1})->ArgNames({"jobs", "threads"})->UseManualTime();
// BENCHMARK(pool_std_thread)->Args({jobs_per_iteration, 1})->ArgNames({"jobs", "threads"})->UseManualTime();
