#include <threadable-benchmarks/util.hxx>
#include <threadable/function.hxx>
#include <threadable/queue.hxx>

#include <algorithm>
#include <functional>
#include <numeric>
#include <vector>

#include <doctest/doctest.h>

#include <nanobench.h>

namespace bench = ankerl::nanobench;

namespace
{
  constexpr auto jobs_per_iteration = 1 << 20;
  auto           val                = 1; // NOLINT

  using index_t = std::atomic_size_t;

  struct no_cacheline
  {
    index_t first  = 0;
    index_t second = 0;
  };

  static_assert(sizeof(no_cacheline) == 16);
  static_assert(alignof(no_cacheline) == 8);

  struct alignas(threadable::details::cache_line_size) one_cacheline
  {
    index_t first  = 0;
    index_t second = 0;
  };

  static_assert(sizeof(one_cacheline) == threadable::details::cache_line_size);
  static_assert(alignof(one_cacheline) == threadable::details::cache_line_size);

  struct alignas(threadable::details::cache_line_size) two_cacheline
  {
    alignas(threadable::details::cache_line_size) index_t first  = 0;
    alignas(threadable::details::cache_line_size) index_t second = 0;
  };

  static_assert(sizeof(two_cacheline) == threadable::details::cache_line_size * 2);
  static_assert(alignof(two_cacheline) == threadable::details::cache_line_size);
}

TEST_CASE("queue: push")
{
  bench::Bench b;
  b.warmup(1).relative(true).batch(jobs_per_iteration).unit("job");

  using job_t = decltype([](){
    bench::doNotOptimizeAway(val = threadable::utils::do_trivial_work(val) );
  });

  b.title("push");
  {
    auto queue = std::vector<std::function<void()>>();
    queue.reserve(jobs_per_iteration);

    // b.run("std::vector",
    //       [&]
    //       {
    //         queue.clear();
    //         for (std::size_t i = 0; i < jobs_per_iteration; ++i)
    //         {
    //           bench::doNotOptimizeAway(queue.emplace_back(job_t{}));
    //         }
    //       });
    auto iters = std::vector<int>(jobs_per_iteration);
    std::iota(std::begin(iters), std::end(iters), 0);

    auto no = no_cacheline{};
    b.run("default size",
          [&]
          {
            std::for_each(std::execution::par, std::begin(iters), std::end(iters),
                          [&no](auto const& i)
                          {
                            if (i % 2 == 0)
                            {
                              bench::doNotOptimizeAway(
                                no.first.fetch_add(1, std::memory_order_acq_rel));
                            }
                            else
                            {
                              bench::doNotOptimizeAway(
                                no.second.fetch_add(1, std::memory_order_acq_rel));
                            }
                          });
          });

    auto one = one_cacheline{};
    b.run("1 cacheline",
          [&]
          {
            std::for_each(std::execution::par, std::begin(iters), std::end(iters),
                          [&one](auto const& i)
                          {
                            if (i % 2 == 0)
                            {
                              bench::doNotOptimizeAway(
                                one.first.fetch_add(1, std::memory_order_acq_rel));
                            }
                            else
                            {
                              bench::doNotOptimizeAway(
                                one.second.fetch_add(1, std::memory_order_acq_rel));
                            }
                          });
          });

    auto two = two_cacheline{};
    b.run("2 cacheline",
          [&]
          {
            std::for_each(std::execution::par, std::begin(iters), std::end(iters),
                          [&two](auto const& i)
                          {
                            if (i % 2 == 0)
                            {
                              bench::doNotOptimizeAway(
                                two.first.fetch_add(1, std::memory_order_acq_rel));
                            }
                            else
                            {
                              bench::doNotOptimizeAway(
                                two.second.fetch_add(1, std::memory_order_acq_rel));
                            }
                          });
          });

    auto job = threadable::job{};
    b.run("threadable::job",
          [&]
          {
            std::for_each(std::execution::par, std::begin(iters), std::end(iters),
                          [&job](auto const& i)
                          {
                            if (i % 2 == 0)
                            {
                              bench::doNotOptimizeAway(
                                job.state.fetch_add(1, std::memory_order_acq_rel));
                            }
                            else
                            {
                              job.func_.reset();
                            }
                          });
          });
  }
  b = {};
  b.warmup(1).relative(true).batch(jobs_per_iteration).unit("job");
  // b.minEpochIterations(16);
  {
    // auto queue = threadable::queue<64>();
    auto queue = threadable::queue<jobs_per_iteration>();
    // auto token = threadable::job_token{};
    auto test = std::atomic_size_t{0};
    auto j    = job_t{};
    REQUIRE(std::atomic_size_t{}.is_lock_free());
    b.run("threadable::queue (seq)",
          [&]
          {
            queue.clear();
            // assert(queue.size() == 0);
            for (std::size_t i = 0; i < queue.max_size(); ++i)
            {
              bench::doNotOptimizeAway(queue.push(j));
              // assert(queue.size() == i + 1);
            }
            // assert(queue.size() == queue.max_size());
          });

    auto iters = std::vector<int>{};
    iters.resize(queue.max_size());
    b.run("threadable::queue (par)",
          [&]
          {
            queue.clear();
            std::for_each(std::execution::par, std::begin(iters), std::end(iters),
                          [&queue, &j](auto const&)
                          {
                            bench::doNotOptimizeAway(queue.push(j));
                          });
            // assert(queue.size() == queue.max_size());
          });
  }
}

// TEST_CASE("queue: iterate (sequential)")
// {
//   bench::Bench b;
//   b.warmup(1).relative(true).batch(jobs_per_iteration).unit("job");

//   using job_t = decltype([](){
//     bench::doNotOptimizeAway(val = threadable::utils::do_trivial_work(val) );
//   });

//   b.title("iterate - sequential");
//   {
//     auto queue = std::vector<std::function<void()>>();
//     queue.resize(jobs_per_iteration, job_t{});

//     b.run("std::vector",
//           [&]
//           {
//             std::for_each(std::execution::seq, std::begin(queue), std::end(queue),
//                           [](auto const& job)
//                           {
//                             bench::doNotOptimizeAway(job);
//                           });
//           });
//   }
//   {
//     auto queue = threadable::queue<jobs_per_iteration>();
//     for (std::size_t i = 0; i < queue.max_size(); ++i)
//     {
//       queue.push(job_t{});
//     }

//     b.run("threadable::queue",
//           [&]
//           {
//             std::for_each(std::execution::seq, std::begin(queue), std::end(queue),
//                           [](auto const& job)
//                           {
//                             bench::doNotOptimizeAway(job);
//                           });
//           });
//   }
// }

// TEST_CASE("queue: iterate (parallel)")
// {
//   bench::Bench b;
//   b.warmup(1).relative(true).batch(jobs_per_iteration).unit("job");

//   using job_t = decltype([](){
//     bench::doNotOptimizeAway(val = threadable::utils::do_trivial_work(val) );
//   });

//   b.title("iterate - parallel");
//   {
//     auto queue = std::vector<std::function<void()>>();
//     queue.resize(jobs_per_iteration, job_t{});

//     b.run("std::vector",
//           [&]
//           {
//             std::for_each(std::execution::par, std::begin(queue), std::end(queue),
//                           [](auto const& job)
//                           {
//                             bench::doNotOptimizeAway(job);
//                           });
//           });
//   }
//   {
//     auto queue = threadable::queue<jobs_per_iteration>();
//     for (std::size_t i = 0; i < queue.max_size(); ++i)
//     {
//       queue.push(job_t{});
//     }

//     b.run("threadable::queue",
//           [&]
//           {
//             std::for_each(std::execution::par, std::begin(queue), std::end(queue),
//                           [](auto const& job)
//                           {
//                             bench::doNotOptimizeAway(job);
//                           });
//           });
//   }
// }

// TEST_CASE("queue: execute (sequential)")
// {
//   bench::Bench b;
//   b.warmup(1).relative(true).batch(jobs_per_iteration).unit("job");

//   using job_t = decltype([](){
//     bench::doNotOptimizeAway(val = threadable::utils::do_trivial_work(val) );
//   });

//   b.title("execute - sequential");
//   {
//     auto queue = std::vector<std::function<void()>>();
//     queue.resize(jobs_per_iteration, job_t{});

//     b.run("std::vector",
//           [&]
//           {
//             std::for_each(std::execution::seq, std::begin(queue), std::end(queue),
//                           [](auto& job)
//                           {
//                             job();
//                           });
//           });
//   }
//   {
//     auto queue = threadable::queue<jobs_per_iteration>();
//     for (std::size_t i = 0; i < queue.max_size(); ++i)
//     {
//       queue.push(job_t{});
//     }
//     auto range = queue.consume();

//     b.run("threadable::queue",
//           [&]
//           {
//             std::for_each(std::execution::seq, std::begin(range), std::end(range),
//                           [](auto& job)
//                           {
//                             job.get()();
//                           });
//           });
//   }
// }

// TEST_CASE("queue: execute (parallel)")
// {
//   bench::Bench b;
//   b.warmup(1).relative(true).batch(jobs_per_iteration).unit("job");

//   using job_t = decltype([](){
//     bench::doNotOptimizeAway(val = threadable::utils::do_trivial_work(val) );
//   });

//   b.title("execute - parallel");
//   {
//     auto queue = std::vector<std::function<void()>>();
//     queue.resize(jobs_per_iteration, job_t{});

//     b.run("std::vector",
//           [&]
//           {
//             std::for_each(std::execution::par, std::begin(queue), std::end(queue),
//                           [](auto& job)
//                           {
//                             job();
//                           });
//           });
//   }
//   {
//     auto queue = threadable::queue<jobs_per_iteration>();
//     for (std::size_t i = 0; i < queue.max_size(); ++i)
//     {
//       queue.push(job_t{});
//     }
//     auto range = queue.consume();

//     b.run("threadable::queue",
//           [&]
//           {
//             std::for_each(std::execution::par, std::begin(range), std::end(range),
//                           [](auto& job)
//                           {
//                             job.get()();
//                           });
//           });
//   }
// }
