#include <threadable-benchmarks/util.hxx>
#include <threadable/circular_iterator.hxx>

#include <iterator>
#include <ranges>
#include <vector>

#include <doctest/doctest.h>

#include <nanobench.h>

namespace bench = ankerl::nanobench;

namespace
{
  constexpr auto buffer_size = 1 << 20;
  constexpr auto count       = buffer_size - 1u;
  using iter_t               = threadable::circular_iterator<int, buffer_size>;
}

TEST_CASE("circular_iterator: dereference")
{
  bench::Bench b;
  b.warmup(10).relative(true);

  auto buffer = std::vector<int>();
  buffer.resize(count);

  b.unit("*it");
  b.title("dereference");
  {
    auto it = buffer.begin();
    b.run("std::vector",
          [&]
          {
            bench::doNotOptimizeAway(*it);
          });
  }
  {
    auto it = iter_t(buffer.data(), 0);
    b.run("threadable::circular_iterator",
          [&]
          {
            bench::doNotOptimizeAway(*it);
          });
  }
  b.unit("it[]");
  b.title("indexing");
  {
    auto it    = buffer.begin();
    auto index = iter_t::difference_type{0};
    b.run("std::vector",
          [&]
          {
            bench::doNotOptimizeAway(it[++index]);
            // simulate bounds-checking
            // for a realistic comparison
            if (index >= buffer.size()) [[unlikely]]
            {
              index = 0;
            }
          });
  }
  {
    auto it    = iter_t(buffer.data(), 0);
    auto index = iter_t::difference_type{0};
    b.run("threadable::circular_iterator",
          [&]
          {
            bench::doNotOptimizeAway(it[++index]);
          });
  }
}

TEST_CASE("circular_iterator: traversing")
{
  bench::Bench b;
  b.warmup(10).relative(true);

  auto buffer = std::vector<int>();
  buffer.resize(buffer_size);

  b.title("traverse - forward");
  b.unit("++it");
  {
    auto const begin = buffer.begin();
    auto const end   = buffer.end();
    auto       it    = begin;
    b.run("std::vector",
          [&]
          {
            bench::doNotOptimizeAway(++it);
            // simulate bounds-checking
            // for a realistic comparison
            if (it == buffer.end()) [[unlikely]]
            {
              it = begin;
            }
          });
  }
  {
    auto it = iter_t(buffer.data(), 0);
    b.run("threadable::circular_iterator",
          [&]
          {
            bench::doNotOptimizeAway(++it);
          });
  }
  b.title("traverse - backward");
  b.unit("--it");
  {
    auto const begin = buffer.begin();
    auto const end   = buffer.end();
    auto       it    = end;
    b.run("std::vector",
          [&]
          {
            bench::doNotOptimizeAway(--it);
            // simulate bounds-checking
            // for a realistic comparison
            if (it == begin) [[unlikely]]
            {
              it = end;
            }
          });
  }
  {
    auto it = iter_t(buffer.data(), buffer_size);
    b.run("threadable::circular_iterator",
          [&]
          {
            bench::doNotOptimizeAway(--it);
          });
  }
}
