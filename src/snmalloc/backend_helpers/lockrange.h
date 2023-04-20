#pragma once

#include "../ds/ds.h"
#include "empty_range.h"

namespace snmalloc
{
  /**
   * Protect the ParentRange with a spin lock.
   *
   * Accesses via the ancestor() mechanism will bypass the lock and so
   * should be used only where the resulting data races are acceptable.
   */
  struct LockRange
  {
    template<typename ParentRange = EmptyRange<>>
    class Type : public ContainsParent<ParentRange>
    {
      using ContainsParent<ParentRange>::parent;

      /**
       * This is infrequently used code, a spin lock simplifies the code
       * considerably, and should never be on the fast path.
       */
      FlagWord spin_lock{};

    public:
      static constexpr bool Aligned = ParentRange::Aligned;

      using ChunkBounds = typename ParentRange::ChunkBounds;

      static constexpr bool ConcurrencySafe = true;

      constexpr Type() = default;

      Range alloc_range(SizeSpec size)
      {
        FlagLock lock(spin_lock);
        return parent.alloc_range(size);
      }

      bool dealloc_range(CapPtr<void, ChunkBounds> base, size_t size, bool force)
      {
        if (force)
        {
          FlagLock lock(spin_lock);
          return parent.dealloc_range(base, size, true);
        }

        return try_flag_lock(spin_lock, [this, base, size](){
          parent.dealloc_range(base, size, true);
        }); 
      }
    };
  };
} // namespace snmalloc
