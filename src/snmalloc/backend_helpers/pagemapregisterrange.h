#pragma once

#include "../mem/metadata.h"
#include "../pal/pal.h"
#include "empty_range.h"
#include "range_helpers.h"

namespace snmalloc
{
  template<SNMALLOC_CONCEPT(IsPagemapWithRegister) Pagemap>
  struct PagemapRegisterRange
  {
    template<typename ParentRange = EmptyRange<>>
    class Type : public ContainsParent<ParentRange>
    {
      using ContainsParent<ParentRange>::parent;

    public:
      constexpr Type() = default;

      static constexpr bool Aligned = ParentRange::Aligned;

      static constexpr bool ConcurrencySafe = ParentRange::ConcurrencySafe;

      using ChunkBounds = typename ParentRange::ChunkBounds;

      Range alloc_range(SizeSpec size)
      {
        auto range = parent.alloc_range(size);

        if (range.base != nullptr)
        {
          Pagemap::register_range(range.base, range.length);
        }

        return range;
      }

      void flush()
      {
        parent.flush();
      }
    };
  };
} // namespace snmalloc
