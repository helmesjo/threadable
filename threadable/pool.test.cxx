#include <threadable/pool.hxx>
#include <threadable/doctest_include.hxx>

#include <latch>

SCENARIO("pool:")
{
  auto threadPool = threadable::pool(std::thread::hardware_concurrency());
  
  std::size_t jobCount = 524288;
  std::latch jobLatch{static_cast<std::ptrdiff_t>(jobCount)};
  
  for(std::size_t i = 0; i < jobCount; ++i)
  {
    threadPool.push([&jobLatch]{
      jobLatch.count_down();
    });
  }
  
  jobLatch.wait();
}
