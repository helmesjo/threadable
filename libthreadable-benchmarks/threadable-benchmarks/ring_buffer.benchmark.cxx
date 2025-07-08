#include <threadable-benchmarks/util.hxx>
#include <threadable/ring_buffer.hxx>

#include <algorithm>
#include <functional>
#include <vector>

#if __cpp_lib_execution >= 201603L && __cpp_lib_parallel_algorithm >= 201603L
  #include <execution>
#endif

#include <doctest/doctest.h>

#include <nanobench.h>

namespace bench = ankerl::nanobench;

namespace
{
#ifdef NDEBUG
  constexpr auto jobs_per_iteration = 1 << 21;
#else
  constexpr auto jobs_per_iteration = 1 << 18;
#endif
  auto val = 1; // NOLINT

  using func_t = fho::fast_func_t;
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
    auto ring  = fho::ring_buffer<func_t, jobs_per_iteration>();
    auto token = fho::slot_token{};

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
            std::ranges::for_each(ring,
                                  [](auto const& job)
                                  {
                                    bench::doNotOptimizeAway(job);
                                  });
          });
  }
  {
    auto ring = fho::ring_buffer<func_t, jobs_per_iteration>();
    for (std::size_t i = 0; i < ring.max_size(); ++i)
    {
      ring.push(job_t{});
    }

    b.run("fho::ring_buffer",
          [&]
          {
            std::ranges::for_each(ring,
                                  [](auto const& job)
                                  {
                                    bench::doNotOptimizeAway(job);
                                  });
          });
  }
}

#if __cpp_lib_execution >= 201603L && __cpp_lib_parallel_algorithm >= 201603L
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
    auto ring = fho::ring_buffer<func_t, jobs_per_iteration>();
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
#endif

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
            std::ranges::for_each(ring,
                                  [](auto& job)
                                  {
                                    job();
                                  });
          });
  }
  {
    auto ring = fho::ring_buffer<func_t, jobs_per_iteration>();
    for (std::size_t i = 0; i < ring.max_size(); ++i)
    {
      ring.push(job_t{});
    }
    auto range = ring.consume();

    b.run("fho::ring_buffer",
          [&]
          {
            std::ranges::for_each(range,
                                  [](auto& job)
                                  {
                                    job();
                                  });
          });
  }
}

#if __cpp_lib_execution >= 201603L && __cpp_lib_parallel_algorithm >= 201603L
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
    auto ring = fho::ring_buffer<func_t, jobs_per_iteration>();
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
                            job();
                          });
          });
  }
}
#endif
