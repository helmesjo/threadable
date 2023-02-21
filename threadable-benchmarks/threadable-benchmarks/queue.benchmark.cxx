#include <threadable/queue.hxx>
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
  constexpr std::size_t jobs_per_iteration = 1 << 14;
  int val = 1;
}

TEST_CASE("queue: iterate")
{
  bench::Bench b;
  b.warmup(2'000).relative(true)
   .minEpochIterations(500);

  using job_t = decltype([](){
    bench::doNotOptimizeAway(val = threadable::utils::do_trivial_work(val) );
  });

  b.title("iterate - sequential");
  {
    auto queue = std::vector<std::function<void()>>();
    queue.resize(jobs_per_iteration, job_t{});

    b.run("std::vector", [&] {
      std::for_each(std::execution::seq, std::begin(queue), std::end(queue), [](auto& job){
        bench::doNotOptimizeAway(job);
      });
    });
  }
  {
    auto queue = threadable::queue2<jobs_per_iteration>();
    for(std::size_t i=0; i<jobs_per_iteration; ++i)
    {
      queue.push(job_t{});
    }

    b.run("threadable::queue", [&] {
      std::for_each(std::execution::seq, std::begin(queue), queue.template end<false>(), [](auto& job){
        bench::doNotOptimizeAway(job);
      });
    });
  }

  b.title("iterate - parallel");
  {
    auto queue = std::vector<std::function<void()>>();
    queue.resize(jobs_per_iteration, job_t{});

    b.run("std::vector", [&] {
      std::for_each(std::execution::par, std::begin(queue), std::end(queue), [](auto& job){
        bench::doNotOptimizeAway(job);
      });
    });
  }
  {
    auto queue = threadable::queue2<jobs_per_iteration>();
    for(std::size_t i=0; i<jobs_per_iteration; ++i)
    {
      queue.push(job_t{});
    }

    b.run("threadable::queue", [&] {
      std::for_each(std::execution::par, std::begin(queue), queue.template end<false>(), [](auto& job){
        bench::doNotOptimizeAway(job);
      });
    });
  }
}

TEST_CASE("queue: execute")
{
  bench::Bench b;
  b.warmup(3'000).relative(true)
   .minEpochIterations(500);

  using job_t = decltype([](){
    bench::doNotOptimizeAway(val = threadable::utils::do_trivial_work(val) );
  });

  b.title("execute - sequential");
  {
    auto queue = std::vector<std::function<void()>>();
    queue.resize(jobs_per_iteration, job_t{});

    b.run("std::vector", [&] {
      std::for_each(std::execution::seq, std::begin(queue), std::end(queue), [](auto& job){
        job();
      });
    });
  }
  {
    auto queue = threadable::queue2<jobs_per_iteration>();
    for(std::size_t i=0; i<jobs_per_iteration; ++i)
    {
      queue.push(job_t{});
    }

    b.run("threadable::queue", [&] {
      std::for_each(std::execution::seq, std::begin(queue), queue.template end<false>(), [](auto& job){
        job.get()();
      });
    });
  }

  b.title("execute - parallel");
  {
    auto queue = std::vector<std::function<void()>>();
    queue.resize(jobs_per_iteration, job_t{});

    b.run("std::vector", [&] {
      std::for_each(std::execution::par, std::begin(queue), std::end(queue), [](auto& job){
        job();
      });
    });
  }
  {
    auto queue = threadable::queue2<jobs_per_iteration>();
    for(std::size_t i=0; i<jobs_per_iteration; ++i)
    {
      queue.push(job_t{});
    }

    b.run("threadable::queue", [&] {
      std::for_each(std::execution::par, std::begin(queue), queue.template end<false>(), [](auto& job){
        job.get()();
      });
    });
  }
}
