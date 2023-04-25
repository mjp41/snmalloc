#pragma once

#include "../ds/ds.h"
#include "empty_range.h"

namespace snmalloc
{
  /**
   * Makes the supplied ParentRange into a global variable.
   */
  struct StaticRange
  {
    template<typename ParentRange = EmptyRange<>>
    class Type : public StaticParent<ParentRange>
    {
      using StaticParent<ParentRange>::parent;

    public:
      static constexpr bool Aligned = ParentRange::Aligned;

      static_assert(
        ParentRange::ConcurrencySafe,
        "StaticRange requires a concurrency safe parent.");

      static constexpr bool ConcurrencySafe = true;

      using ChunkBounds = typename ParentRange::ChunkBounds;

      constexpr Type() = default;

      Range alloc_range(SizeSpec size)
      {
        return parent.alloc_range(size);
      }

      bool dealloc_range(CapPtr<void, ChunkBounds> base, size_t size, bool force)
      {
        return parent.dealloc_range(base, size, force);
      }

      void flush()
      {
        // Don't do anything as parent is shared.
      }
    };
  };
} // namespace snmalloc
