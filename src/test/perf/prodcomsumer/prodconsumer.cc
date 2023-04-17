#include "test/opt.h"
#include "test/setup.h"
#include "test/usage.h"
#include "test/xoroshiro.h"

#include <iomanip>
#include <iostream>
#include <snmalloc/snmalloc.h>
#include <thread>
#include <vector>

using namespace snmalloc;

int main()
{
  ScopedAllocator prod_alloc;
  ScopedAllocator consumer_alloc;

  for (int i = 0; i < 1000; i++)
  {
    std::vector<void*> allocs;
    for (int j = 0; j < 1000; j++)
      allocs.push_back(prod_alloc->alloc(128));

    std::cout << "remote_inflight "
              << snmalloc::RemoteDeallocCache::remote_inflight << std::endl;
    for (auto a : allocs)
    {
      consumer_alloc->dealloc(a);
    }
    std::cout << "remote_inflight "
              << snmalloc::RemoteDeallocCache::remote_inflight << std::endl;

    allocs.clear();
  }
  return 0;
}
