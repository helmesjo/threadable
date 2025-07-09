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

  using fho_func_t = fho::fast_func_t;
  using std_func_t = std::function<void()>;
}

TEST_CASE("ring: emplace")
{
  bench::Bench b;
  b.warmup(1).relative(true).batch(jobs_per_iteration).unit("job");

  using job_t = decltype([](){
    bench::doNotOptimizeAway(val = fho::utils::do_trivial_work(val) );
  });

  b.title("ring: emplace - sequential");
  {
    auto ring = std::vector<std_func_t>();
    ring.reserve(jobs_per_iteration);

    b.run("std::vector<function>",
          [&]
          {
            ring.clear();
            for (std::size_t i = 0; i < jobs_per_iteration; ++i)
            {
              bench::doNotOptimizeAway(ring.emplace_back(job_t{}));
            }
          });
  }
  b.title("ring: emplace - sequential");
  {
    auto ring = fho::ring_buffer<fho_func_t, jobs_per_iteration>();

    b.run("fho::ring_buffer<function>",
          [&]
          {
            ring.clear();
            for (std::size_t i = 0; i < ring.max_size(); ++i)
            {
              bench::doNotOptimizeAway(ring.emplace_back(job_t{}));
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
    auto ring = std::vector<std_func_t>();
    for (std::size_t i = 0; i < jobs_per_iteration; ++i)
    {
      ring.emplace_back(job_t{});
    }

    b.run("std::vector<function>",
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
    auto ring = fho::ring_buffer<fho_func_t, jobs_per_iteration>();
    for (std::size_t i = 0; i < ring.max_size(); ++i)
    {
      ring.emplace_back(job_t{});
    }

    b.run("fho::ring_buffer<function>",
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
    auto ring = std::vector<std_func_t>();
    for (std::size_t i = 0; i < jobs_per_iteration; ++i)
    {
      ring.emplace_back(job_t{});
    }

    b.run("std::vector<function>",
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
    auto ring = fho::ring_buffer<fho_func_t, jobs_per_iteration>();
    for (std::size_t i = 0; i < ring.max_size(); ++i)
    {
      ring.emplace_back(job_t{});
    }

    b.run("fho::ring_buffer<function>",
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
    auto ring = std::vector<std_func_t>();
    for (std::size_t i = 0; i < jobs_per_iteration; ++i)
    {
      ring.emplace_back(job_t{});
    }

    b.run("std::vector<function>",
          [&]
          {
            std::ranges::for_each(ring,
                                  [](auto& job)
                                  {
                                    job();
                                    bench::doNotOptimizeAway(job);
                                  });
          });
  }
  {
    auto ring = fho::ring_buffer<fho_func_t, jobs_per_iteration>();
    for (std::size_t i = 0; i < ring.max_size(); ++i)
    {
      ring.emplace_back(job_t{});
    }
    auto range = ring.consume();

    b.run("fho::ring_buffer<function>",
          [&]
          {
            std::ranges::for_each(range,
                                  [](auto& job)
                                  {
                                    job();
                                    bench::doNotOptimizeAway(job);
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
    auto ring = std::vector<std_func_t>();
    for (std::size_t i = 0; i < jobs_per_iteration; ++i)
    {
      ring.emplace_back(job_t{});
    }

    b.run("std::vector<function>",
          [&]
          {
            std::for_each(std::execution::par, std::begin(ring), std::end(ring),
                          [](auto& job)
                          {
                            job();
                            bench::doNotOptimizeAway(job);
                          });
          });
  }
  {
    auto ring = fho::ring_buffer<fho_func_t, jobs_per_iteration>();
    for (std::size_t i = 0; i < ring.max_size(); ++i)
    {
      ring.emplace_back(job_t{});
    }
    auto range = ring.consume();

    b.run("fho::ring_buffer<function>",
          [&]
          {
            std::for_each(std::execution::par, std::begin(range), std::end(range),
                          [](auto& job)
                          {
                            job();
                            bench::doNotOptimizeAway(job);
                          });
          });
  }
}
#endif
