#include <threadable-benchmarks/util.hxx>
#include <threadable/ring_buffer.hxx>

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

TEST_CASE("ring: push")
{
  bench::Bench b;
  b.warmup(1).relative(true).batch(jobs_per_iteration).unit("job");

  using job_t = decltype([](){
    bench::doNotOptimizeAway(val = fho::utils::do_trivial_work(val) );
  });

  b.title("ring: push");
  {
    auto ring = std::vector<std::function<void()>>();
    ring.reserve(jobs_per_iteration);

    b.run("std::vector",
          [&]
          {
            ring.clear();
            for (std::size_t i = 0; i < jobs_per_iteration; ++i)
            {
              bench::doNotOptimizeAway(ring.emplace_back(job_t{}));
            }
          });
  }
  b.title("ring: push");
  {
    auto ring  = fho::ring_buffer<fho::job, jobs_per_iteration>();
    auto token = fho::job_token{};

    b.run("fho::ring_buffer",
          [&]
          {
            ring.clear();
            for (std::size_t i = 0; i < ring.max_size(); ++i)
            {
              bench::doNotOptimizeAway(ring.push(token, job_t{}));
            }
          });
  }
}

TEST_CASE("ring: iterate (sequential)")
{
  bench::Bench b;
  b.warmup(1).relative(true).batch(jobs_per_iteration).unit("job");

  using job_t = decltype([](){
    bench::doNotOptimizeAway(val = fho::utils::do_trivial_work(val) );
  });

  b.title("ring: iterate - sequential");
  {
    auto ring = std::vector<std::function<void()>>();
    ring.resize(jobs_per_iteration, job_t{});

    b.run("std::vector",
          [&]
          {
            std::for_each(std::execution::seq, std::begin(ring), std::end(ring),
                          [](auto const& job)
                          {
                            bench::doNotOptimizeAway(job);
                          });
          });
  }
  {
    auto ring = fho::ring_buffer<fho::job, jobs_per_iteration>();
    for (std::size_t i = 0; i < ring.max_size(); ++i)
    {
      ring.push(job_t{});
    }

    b.run("fho::ring_buffer",
          [&]
          {
            std::for_each(std::execution::seq, std::begin(ring), std::end(ring),
                          [](auto const& job)
                          {
                            bench::doNotOptimizeAway(job);
                          });
          });
  }
}

TEST_CASE("ring: iterate (parallel)")
{
  bench::Bench b;
  b.warmup(1).relative(true).batch(jobs_per_iteration).unit("job");

  using job_t = decltype([](){
    bench::doNotOptimizeAway(val = fho::utils::do_trivial_work(val) );
  });

  b.title("ring: iterate - parallel");
  {
    auto ring = std::vector<std::function<void()>>();
    ring.resize(jobs_per_iteration, job_t{});

    b.run("std::vector",
          [&]
          {
            std::for_each(std::execution::par, std::begin(ring), std::end(ring),
                          [](auto const& job)
                          {
                            bench::doNotOptimizeAway(job);
                          });
          });
  }
  {
    auto ring = fho::ring_buffer<fho::job, jobs_per_iteration>();
    for (std::size_t i = 0; i < ring.max_size(); ++i)
    {
      ring.push(job_t{});
    }

    b.run("fho::ring_buffer",
          [&]
          {
            std::for_each(std::execution::par, std::begin(ring), std::end(ring),
                          [](auto const& job)
                          {
                            bench::doNotOptimizeAway(job);
                          });
          });
  }
}

TEST_CASE("ring: execute (sequential)")
{
  bench::Bench b;
  b.warmup(1).relative(true).batch(jobs_per_iteration).unit("job");

  using job_t = decltype([](){
    bench::doNotOptimizeAway(val = fho::utils::do_trivial_work(val) );
  });

  b.title("ring: execute - sequential");
  {
    auto ring = std::vector<std::function<void()>>();
    ring.resize(jobs_per_iteration, job_t{});

    b.run("std::vector",
          [&]
          {
            std::for_each(std::execution::seq, std::begin(ring), std::end(ring),
                          [](auto& job)
                          {
                            job();
                          });
          });
  }
  {
    auto ring = fho::ring_buffer<fho::job, jobs_per_iteration>();
    for (std::size_t i = 0; i < ring.max_size(); ++i)
    {
      ring.push(job_t{});
    }
    auto range = ring.consume();

    b.run("fho::ring_buffer",
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

TEST_CASE("ring: execute (parallel)")
{
  bench::Bench b;
  b.warmup(1).relative(true).batch(jobs_per_iteration).unit("job");

  using job_t = decltype([](){
    bench::doNotOptimizeAway(val = fho::utils::do_trivial_work(val) );
  });

  b.title("ring: execute - parallel");
  {
    auto ring = std::vector<std::function<void()>>();
    ring.resize(jobs_per_iteration, job_t{});

    b.run("std::vector",
          [&]
          {
            std::for_each(std::execution::par, std::begin(ring), std::end(ring),
                          [](auto& job)
                          {
                            job();
                          });
          });
  }
  {
    auto ring = fho::ring_buffer<fho::job, jobs_per_iteration>();
    for (std::size_t i = 0; i < ring.max_size(); ++i)
    {
      ring.push(job_t{});
    }
    auto range = ring.consume();

    b.run("fho::ring_buffer",
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
