#pragma once

#include "bits.h"
#include "stdio.h"

#include <string>

namespace snmalloc
{
  class MiniStat
  {
  public:
    std::atomic<uint64_t> time_spent_waiting{0};
    std::atomic<uint64_t> max_wait{0};
    std::atomic<uint64_t> count{0};
    std::string op_name;

    MiniStat(std::string op_name) : op_name(op_name) {}

    void add(uint64_t wait)
    {
      time_spent_waiting += wait;
      count++;
      auto old_max = max_wait.load(std::memory_order_relaxed);
      if (old_max < wait)
        max_wait.compare_exchange_weak(old_max, wait);
    }

    ~MiniStat()
    {
      printf(
        "%s:\n  Times: %zu\n  Time spend waiting: %zu cycles\n  Max wait: %zu "
        "cycles\n",
        op_name.c_str(),
        count.load(),
        time_spent_waiting.load(),
        max_wait.load());
    }

    class Measure
    {
      MiniStat& stat;
      uint64_t start;

    public:
      Measure(MiniStat& stat) : stat(stat), start(Aal::tick()) {}

      ~Measure()
      {
        stat.add(Aal::tick() - start);
      }
    };
  };

  class FlagLock
  {
  private:
    std::atomic_flag& lock;

  public:
    FlagLock(std::atomic_flag& lock) : lock(lock)
    {
      static MiniStat stat("FlagLock");
      MiniStat::Measure a(stat);
      while (lock.test_and_set(std::memory_order_acquire))
        Aal::pause();
    }

    ~FlagLock()
    {
      lock.clear(std::memory_order_release);
    }
  };
} // namespace snmalloc
