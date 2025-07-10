#include <threadable-benchmarks/util.hxx>
#include <threadable/pool.hxx>

#include <queue>

#include <doctest/doctest.h>

#include <nanobench.h>

namespace bench = ankerl::nanobench;

namespace
{
#ifdef NDEBUG
  constexpr auto tasks_per_iteration = 1 << 21;
#else
  constexpr auto tasks_per_iteration = 1 << 18;
#endif
  auto val = 1; // NOLINT
}

TEST_CASE("pool: task execution")
{
  bench::Bench b;
  b.warmup(1).relative(true).unit("task");

  using task_t = decltype([](){
    bench::doNotOptimizeAway(val = fho::utils::do_non_trivial_work(val) );
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
    auto pool = fho::pool<tasks_per_iteration>();

    auto& queue = pool.create(fho::execution::par);
    b.batch(tasks_per_iteration)
      .run("fho::pool (queues: 1)",
           [&]
           {
             fho::token_group group;
             for (std::size_t i = 0; i < tasks_per_iteration; ++i)
             {
               group += queue.emplace_back(task_t{});
             }
             group.wait();
           });
  }
  {
    auto pool = fho::pool<tasks_per_iteration>();

    auto queues =
      std::vector<std::reference_wrapper<fho::ring_buffer<fho::fast_func_t, tasks_per_iteration>>>{
        pool.create(fho::execution::par), pool.create(fho::execution::par),
        pool.create(fho::execution::par), pool.create(fho::execution::par)};

    b.batch(tasks_per_iteration)
      .run("fho::pool (queues: 4)",
           [&]
           {
             fho::token_group group;
             for (std::size_t i = 0; i < tasks_per_iteration >> 2; ++i)
             {
               for (decltype(pool)::queue_t& queue : queues)
               {
                 group += queue.emplace_back(task_t{});
               }
             }
             group.wait();
           });
  }
}
