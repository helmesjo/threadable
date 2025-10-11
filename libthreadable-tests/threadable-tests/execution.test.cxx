#include <threadable-tests/doctest_include.hxx>
#include <threadable/execution.hxx>
#include <threadable/ring_buffer.hxx>

SCENARIO("executor: Submit callables")
{
  auto exec = fho::executor();
  GIVEN("a range of callables")
  {
    static constexpr auto size = std::size_t{1024};

    auto items    = std::vector<std::function<void()>>{};
    auto executed = std::vector<std::size_t>(size, 0);
    auto tokens   = fho::token_group{size};
    auto counter  = std::atomic_size_t{0};

    for (std::size_t i = 0; i < size; ++i)
    {
      items.emplace_back(
        [i, &executed, &counter]()
        {
          executed[i] = counter++;
        });
      // simulate interruptions
      if (i % 2 == 0)
      {
        tokens += exec.submit(items);
        items.clear();
        std::this_thread::yield();
      }
    }
    WHEN("submitted to the executor")
    {
      tokens.wait();
      THEN("all tasks are executed in order")
      {
        REQUIRE(executed.size() == size);
        for (std::size_t i = 0; i < items.size(); ++i)
        {
          REQUIRE(executed[i] == i);
        }
      }
    }
  }
}
