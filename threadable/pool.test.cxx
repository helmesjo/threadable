#include <threadable/pool.hxx>
#include <threadable/doctest_include.hxx>

#include <cstddef>
#include <thread>
#include <vector>

SCENARIO("pool: stress-test")
{
  static constexpr std::size_t nr_of_jobs = 1 << 16;
  auto pool = threadable::pool<nr_of_jobs>(std::thread::hardware_concurrency());
  GIVEN("pool with multiple threads")
  {
    WHEN("a large amount of jobs are pushed")
    {
      std::atomic_size_t counter{0};
      std::vector<threadable::job_token> tokens;
      tokens.reserve(nr_of_jobs);

      for(std::size_t i = 0; i < nr_of_jobs; ++i)
      {
        tokens.push_back(pool.push([&counter]{ ++counter; }));
      }

      for(const auto& token : tokens)
      {
        token.wait();
      }

      THEN("all gets executed")
      {
        REQUIRE(counter.load() == nr_of_jobs);
      }
    }
  }
}