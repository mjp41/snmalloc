#pragma once
#include "../pal/pal.h"
#include "empty_range.h"
#include "range_helpers.h"

namespace snmalloc
{
  template<typename PAL>
  struct CommitRange
  {
    template<typename ParentRange>
    class Type : public ContainsParent<ParentRange>
    {
      using ContainsParent<ParentRange>::parent;

    public:
      static constexpr bool Aligned = ParentRange::Aligned;

      static constexpr bool ConcurrencySafe = ParentRange::ConcurrencySafe;

      using ChunkBounds = typename ParentRange::ChunkBounds;
      static_assert(
        ChunkBounds::address_space_control ==
        capptr::dimension::AddressSpaceControl::Full);

      constexpr Type() = default;

      Range alloc_range(SizeSpec size)
      {
        SNMALLOC_ASSERT_MSG(
          (size.desired % PAL::page_size) == 0,
          "size ({}) must be a multiple of page size ({})",
          size.desired,
          PAL::page_size);
        SNMALLOC_ASSERT_MSG(
          (size.required % PAL::page_size) == 0,
          "size ({}) must be a multiple of page size ({})",
          size.required,
          PAL::page_size);

        auto range = parent.alloc_range(size);
        if (range.base != nullptr)
          PAL::template notify_using<NoZero>(range.base.unsafe_ptr(), range.length);
        return range;
      }

      bool dealloc_range(CapPtr<void, ChunkBounds> base, size_t size, bool force)
      {
        SNMALLOC_ASSERT_MSG(
          (size % PAL::page_size) == 0,
          "size ({}) must be a multiple of page size ({})",
          size,
          PAL::page_size);
        PAL::notify_not_using(base.unsafe_ptr(), size);
        bool completed = parent.dealloc_range(base, size, force);
        if (!completed)
        {
          PAL::template notify_using<NoZero>(base.unsafe_ptr(), size);
        }
        return completed;
      }

      void flush()
      {
        parent.flush();
      }
    };
  };
} // namespace snmalloc
