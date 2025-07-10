# threadable

## TLDR
`libthreadable` is a C++ library for cache-aware, concurrent task execution using  
a thread pool with lock-free queues. It simplifies parallel programming by managing  
threads and task distribution for performance-critical applications.

## Example
```cpp
#include <threadable/pool.hxx>

int main() {
  // thread pool:
  auto  pool  = fho::pool();
  auto& queue = pool.create();
  auto  token = queue.emplace_back( []() { cout << "task executed!\n"; });
  token.wait();

  // generic ring buffer:
  auto ring = fho::ring_buffer<int>();
  ring.emplace_back(1);
  ring.emplace_back(2);
  ring.emplace_back(3);
  assert(ring.size() == 3);

  for (auto v : ring)
  {
    cout << format("{}\n", v); // prints 1 2 3
  }
  assert(ring.size() == 3);

  for (auto v : ring.consume())
  {
    cout << format("{}\n", v); // prints 1 2 3
  }
  assert(ring.size() == 0);

  return 0;
}
```

## Design Overview

**Thread Pool** (`pool`) + **Worker Threads** (`executor`):  
_Manages multiple task queues and worker threads,  
orchestrating task distribution to executors running  
in dedicated threads via a scheduler._

**task Queues** (`ring_buffer`):  
_Lock-free multi-producer, single-consumer ring buffer  
to store tasks._

**Ring Iterator** (`ring_iterator`) + **Slot** (`ring_slot`):  
_Support the `ring_buffer` with efficient random access  
and state management for buffer elements._
