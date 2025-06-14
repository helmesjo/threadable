#include <threadable-benchmarks/util.hxx>
#include <threadable/queue.hxx>

#include <algorithm>
#include <functional>
#include <vector>

#include <doctest/doctest.h>

#include <nanobench.h>

namespace bench = ankerl::nanobench;

namespace
{
  constexpr auto jobs_per_iteration = 1 << 20;
  auto           val                = 1; // NOLINT
}

TEST_CASE("queue: push")
{
  bench::Bench b;
  b.warmup(1).relative(true).batch(jobs_per_iteration).unit("job");

  using job_t = decltype([](){
    bench::doNotOptimizeAway(val = fho::utils::do_trivial_work(val) );
  });

  b.title("queue: push");
  {
    auto queue = std::vector<std::function<void()>>();
    queue.reserve(jobs_per_iteration);

    b.run("std::vector",
          [&]
          {
            queue.clear();
            for (std::size_t i = 0; i < jobs_per_iteration; ++i)
            {
              bench::doNotOptimizeAway(queue.emplace_back(job_t{}));
            }
          });
  }
  b.title("queue: push");
  {
    auto queue = fho::queue<jobs_per_iteration>();
    auto token = fho::job_token{};

    b.run("fho::queue",
          [&]
          {
            queue.clear();
            for (std::size_t i = 0; i < queue.max_size(); ++i)
            {
              bench::doNotOptimizeAway(queue.push(token, job_t{}));
            }
          });
  }
}

TEST_CASE("queue: iterate (sequential)")
{
  bench::Bench b;
  b.warmup(1).relative(true).batch(jobs_per_iteration).unit("job");

  using job_t = decltype([](){
    bench::doNotOptimizeAway(val = fho::utils::do_trivial_work(val) );
  });

  b.title("queue: iterate - sequential");
  {
    auto queue = std::vector<std::function<void()>>();
    queue.resize(jobs_per_iteration, job_t{});

    b.run("std::vector",
          [&]
          {
            std::for_each(std::execution::seq, std::begin(queue), std::end(queue),
                          [](auto const& job)
                          {
                            bench::doNotOptimizeAway(job);
                          });
          });
  }
  {
    auto queue = fho::queue<jobs_per_iteration>();
    for (std::size_t i = 0; i < queue.max_size(); ++i)
    {
      queue.push(job_t{});
    }

    b.run("fho::queue",
          [&]
          {
            std::for_each(std::execution::seq, std::begin(queue), std::end(queue),
                          [](auto const& job)
                          {
                            bench::doNotOptimizeAway(job);
                          });
          });
  }
}

TEST_CASE("queue: iterate (parallel)")
{
  bench::Bench b;
  b.warmup(1).relative(true).batch(jobs_per_iteration).unit("job");

  using job_t = decltype([](){
    bench::doNotOptimizeAway(val = fho::utils::do_trivial_work(val) );
  });

  b.title("queue: iterate - parallel");
  {
    auto queue = std::vector<std::function<void()>>();
    queue.resize(jobs_per_iteration, job_t{});

    b.run("std::vector",
          [&]
          {
            std::for_each(std::execution::par, std::begin(queue), std::end(queue),
                          [](auto const& job)
                          {
                            bench::doNotOptimizeAway(job);
                          });
          });
  }
  {
    auto queue = fho::queue<jobs_per_iteration>();
    for (std::size_t i = 0; i < queue.max_size(); ++i)
    {
      queue.push(job_t{});
    }

    b.run("fho::queue",
          [&]
          {
            std::for_each(std::execution::par, std::begin(queue), std::end(queue),
                          [](auto const& job)
                          {
                            bench::doNotOptimizeAway(job);
                          });
          });
  }
}

TEST_CASE("queue: execute (sequential)")
{
  bench::Bench b;
  b.warmup(1).relative(true).batch(jobs_per_iteration).unit("job");

  using job_t = decltype([](){
    bench::doNotOptimizeAway(val = fho::utils::do_trivial_work(val) );
  });

  b.title("queue: execute - sequential");
  {
    auto queue = std::vector<std::function<void()>>();
    queue.resize(jobs_per_iteration, job_t{});

    b.run("std::vector",
          [&]
          {
            std::for_each(std::execution::seq, std::begin(queue), std::end(queue),
                          [](auto& job)
                          {
                            job();
                          });
          });
  }
  {
    auto queue = fho::queue<jobs_per_iteration>();
    for (std::size_t i = 0; i < queue.max_size(); ++i)
    {
      queue.push(job_t{});
    }
    auto range = queue.consume();

    b.run("fho::queue",
          [&]
          {
            std::for_each(std::execution::seq, std::begin(range), std::end(range),
                          [](auto& job)
                          {
                            job.get()();
                          });
          });
  }
}

TEST_CASE("queue: execute (parallel)")
{
  bench::Bench b;
  b.warmup(1).relative(true).batch(jobs_per_iteration).unit("job");

  using job_t = decltype([](){
    bench::doNotOptimizeAway(val = fho::utils::do_trivial_work(val) );
  });

  b.title("queue: execute - parallel");
  {
    auto queue = std::vector<std::function<void()>>();
    queue.resize(jobs_per_iteration, job_t{});

    b.run("std::vector",
          [&]
          {
            std::for_each(std::execution::par, std::begin(queue), std::end(queue),
                          [](auto& job)
                          {
                            job();
                          });
          });
  }
  {
    auto queue = fho::queue<jobs_per_iteration>();
    for (std::size_t i = 0; i < queue.max_size(); ++i)
    {
      queue.push(job_t{});
    }
    auto range = queue.consume();

    b.run("fho::queue",
          [&]
          {
            std::for_each(std::execution::par, std::begin(range), std::end(range),
                          [](auto& job)
                          {
                            job.get()();
                          });
          });
  }
}
