#include <threadable-tests/doctest_include.hxx>
#include <threadable/execution.hxx>
#include <threadable/ring_buffer.hxx>

SCENARIO("ring_buffer: execution order")
{
  GIVEN("a ring buffer")
  {
    auto ring = fho::ring_buffer<fho::job, 32>();

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
      REQUIRE(fho::execute(ring.consume(), fho::execution::sequential) == ring.max_size());
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
