#include <iostream>
#include <snmalloc/snmalloc.h>
#include <vector>

using namespace snmalloc;

int main()
{
  Alloc::Config::ensure_init();
  Alloc::Config::LocalState::GlobalR gr;

  for (size_t i = 0; i < 16384; i++)
  {
    auto start = Aal::benchmark_time_start();
    gr.alloc_range(MIN_CHUNK_SIZE);
    auto end = Aal::benchmark_time_end();
    std::cout << "Time: " << end - start << std::endl;
  }
}