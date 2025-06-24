#include <threadable-tests/doctest_include.hxx>
#include <threadable/execution.hxx>
#include <threadable/function.hxx>
#include <threadable/ring_buffer.hxx>

namespace
{
  using func_t = fho::function<fho::details::slot_size>;
}

SCENARIO("ring_buffer: execution order")
{
  GIVEN("a ring buffer")
  {
    auto ring = fho::ring_buffer<func_t, 32>();

    auto order = std::vector<std::size_t>{};
    auto m     = std::mutex{};
    for (std::size_t i = 0; i < ring.max_size(); ++i)
    {
      ring.push(
        [&order, &m, i]
        {
          auto _ = std::scoped_lock{m};
          order.push_back(i);
        });
    }
    WHEN("executing jobs sequentially")
    {
      REQUIRE(fho::execute(ring.consume(), fho::execution::seq) == ring.max_size());
      THEN("jobs are executed FIFO")
      {
        REQUIRE(order.size() == ring.max_size());
        for (std::size_t i = 0; i < ring.max_size(); ++i)
        {
          REQUIRE(order[i] == i);
        }
      }
    }
  }
}
