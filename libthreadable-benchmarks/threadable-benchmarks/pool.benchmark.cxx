#include <threadable-benchmarks/util.hxx>
#include <threadable/pool.hxx>

#include <queue>

#include <doctest/doctest.h>

#include <nanobench.h>

namespace bench = ankerl::nanobench;

namespace
{
#ifdef NDEBUG
  constexpr auto    tasks_per_iteration = 1 << 16;
  inline auto const max_thread_count    = std::thread::hardware_concurrency();
#else
  constexpr auto    tasks_per_iteration = 1 << 16;
  inline auto const max_thread_count    = std::min(8u, std::thread::hardware_concurrency());
#endif
  auto dummyVal = 1; // NOLINT
}

TEST_CASE("pool: task execution")
{
  bench::Bench b;
  b.warmup(1).relative(true).unit("task");

  using task_t = decltype([](){
    bench::doNotOptimizeAway(dummyVal = fho::utils::do_non_trivial_work(dummyVal) );
  });

  b.title("pool: push & wait");
  {
    // too slow with large batch size, but also unaffected for
    // stats reported.
    static constexpr auto             tasks_per_iteration_reduced = 1 << 14;
    std::queue<std::function<void()>> queue;
    b.batch(tasks_per_iteration_reduced)
      .run("std::queue<function>",
           [&]
           {
             for (std::size_t i = 0; i < tasks_per_iteration_reduced; ++i)
             {
               queue.emplace(task_t{});
             }
             while (!queue.empty())
             {
               auto& task = queue.back();
               task();
               queue.pop();
             }
           });
  }
  {
    for (auto threads = 1u; threads <= max_thread_count; threads *= 2)
    {
      {
        auto pool = fho::pool(threads);

        auto title = std::format("fho::pool (t: {})", threads);
        b.batch(tasks_per_iteration)
          .run(title,
               [&]
               {
                 for (std::size_t i = 0; i < tasks_per_iteration; ++i)
                 {
                   pool.push(task_t{});
                 }
                 pool.wait();
               });
      }
    }
  }
}
