/**
 * The first operation a thread performs takes a different path to every
 * subsequent operation as it must lazily initialise the thread local allocator.
 * This tests performs all sizes of allocation, and deallocation as the first
 * operation.
 */

#include "test/setup.h"

#include <snmalloc.h>
#include <thread>

void alloc1(size_t size)
{
  void* r = snmalloc::ThreadAlloc::get_noncachable()->alloc(size);
  snmalloc::ThreadAlloc::get_noncachable()->dealloc(r);
}

void alloc2(size_t size)
{
  auto a = snmalloc::ThreadAlloc::get_noncachable();
  void* r = a->alloc(size);
  a->dealloc(r);
}

void alloc3(size_t size)
{
  auto a = snmalloc::ThreadAlloc::get_noncachable();
  void* r = a->alloc(size);
  a->dealloc(r, size);
}

void alloc4(size_t size)
{
  auto a = snmalloc::ThreadAlloc::get();
  void* r = a->alloc(size);
  a->dealloc(r);
}

void dealloc1(void* p, size_t)
{
  snmalloc::ThreadAlloc::get_noncachable()->dealloc(p);
}

void dealloc2(void* p, size_t size)
{
  snmalloc::ThreadAlloc::get_noncachable()->dealloc(p, size);
}

void dealloc3(void* p, size_t)
{
  snmalloc::ThreadAlloc::get()->dealloc(p);
}

void dealloc4(void* p, size_t size)
{
  snmalloc::ThreadAlloc::get()->dealloc(p, size);
}

void f(size_t size)
{
  auto t1 = std::thread(alloc1, size);
  auto t2 = std::thread(alloc2, size);
  auto t3 = std::thread(alloc3, size);
  auto t4 = std::thread(alloc4, size);

  auto a = snmalloc::ThreadAlloc::get();
  auto p1 = a->alloc(size);
  auto p2 = a->alloc(size);
  auto p3 = a->alloc(size);
  auto p4 = a->alloc(size);

  auto t5 = std::thread(dealloc1, p1, size);
  auto t6 = std::thread(dealloc2, p2, size);
  auto t7 = std::thread(dealloc3, p3, size);
  auto t8 = std::thread(dealloc4, p4, size);

  t1.join();
  t2.join();
  t3.join();
  t4.join();
  t5.join();
  t6.join();
  t7.join();
  t8.join();
}

int main(int, char**)
{
  setup();

  f(0);
  f(1);
  f(3);
  f(5);
  f(7);
  for (size_t exp = 1; exp < snmalloc::SUPERSLAB_BITS; exp++)
  {
    f(1ULL << exp);
    f(3ULL << exp);
    f(5ULL << exp);
    f(7ULL << exp);
    f((1ULL << exp) + 1);
    f((3ULL << exp) + 1);
    f((5ULL << exp) + 1);
    f((7ULL << exp) + 1);
    f((1ULL << exp) - 1);
    f((3ULL << exp) - 1);
    f((5ULL << exp) - 1);
    f((7ULL << exp) - 1);
  }
}
