#pragma once

#include "empty_range.h"
#include "range_helpers.h"

#include <atomic>

namespace snmalloc
{
  /**
   * Used to measure memory usage.
   */
  struct StatsRange
  {
    template<typename ParentRange = EmptyRange<>>
    class Type : public ContainsParent<ParentRange>
    {
      using ContainsParent<ParentRange>::parent;

      static inline std::atomic<size_t> current_usage{};
      static inline std::atomic<size_t> peak_usage{};

    public:
      static constexpr bool Aligned = ParentRange::Aligned;

      static constexpr bool ConcurrencySafe = ParentRange::ConcurrencySafe;

      using ChunkBounds = typename ParentRange::ChunkBounds;

      constexpr Type() = default;

      Range alloc_range(SizeSpec size)
      {
        auto result = parent.alloc_range(size);
        if (result.base != nullptr)
        {
          auto prev = current_usage.fetch_add(result.length);
          auto curr = peak_usage.load();
          while (curr < prev + result.length)
          {
            if (peak_usage.compare_exchange_weak(curr, prev + result.length))
              break;
          }
        }
        return result;
      }

      bool dealloc_range(CapPtr<void, ChunkBounds> base, size_t size, bool force)
      {
        auto result = parent.dealloc_range(base, size, force);
        if (result)
          current_usage -= size;
        return result;
      }

      size_t get_current_usage()
      {
        return current_usage.load();
      }

      size_t get_peak_usage()
      {
        return peak_usage.load();
      }

      void flush()
      {
        parent.flush();
      }
    };
  };

  template<typename StatsR1, typename StatsR2>
  class StatsCombiner
  {
    StatsR1 r1{};
    StatsR2 r2{};

  public:
    size_t get_current_usage()
    {
      return r1.get_current_usage() + r2.get_current_usage();
    }

    size_t get_peak_usage()
    {
      return r1.get_peak_usage() + r2.get_peak_usage();
    }
  };
} // namespace snmalloc
