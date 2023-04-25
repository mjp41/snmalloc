#pragma once

#include "../ds/ds.h"
#include "empty_range.h"

namespace snmalloc
{
  /**
   * Stores a references to the parent range so that it can be shared
   * without `static` scope.
   *
   * This could be used to allow multiple allocators on a single region of
   * memory.
   */
  struct IndirectRange
  {
    template<typename ParentRange = EmptyRange<>>
    class Type : public RefParent<ParentRange>
    {
      using RefParent<ParentRange>::parent;

    public:
      static constexpr bool Aligned = ParentRange::Aligned;

      static_assert(
        ParentRange::ConcurrencySafe,
        "IndirectRange requires a concurrency safe parent.");

      static constexpr bool ConcurrencySafe = true;

      using ChunkBounds = typename ParentRange::ChunkBounds;

      constexpr Type() = default;

      Range alloc_range(SizeSpec size)
      {
        return parent->alloc_range(size);
      }

      bool dealloc_range(CapPtr<void, ChunkBounds> base, size_t size, bool force)
      {
        return parent->dealloc_range(base, size, force);
      }

      void flush()
      {
        parent.flush();
      }

      /**
       * Points the parent reference to the given range.
       */
      void set_parent(ParentRange* p)
      {
        parent = p;
      }
    };
  };
} // namespace snmalloc
