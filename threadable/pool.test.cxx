#include <threadable/pool.hxx>
#include <threadable/doctest_include.hxx>

#include <latch>

SCENARIO("pool:")
{
  constexpr std::size_t nr_of_jobs = 1 << 16;
  auto threadPool = threadable::pool<nr_of_jobs>(std::thread::hardware_concurrency());

  std::latch jobLatch{static_cast<std::ptrdiff_t>(nr_of_jobs)};

  for(std::size_t i = 0; i < nr_of_jobs; ++i)
  {
    threadPool.push([&jobLatch]{
      jobLatch.count_down();
    });
  }

  jobLatch.wait();
}
