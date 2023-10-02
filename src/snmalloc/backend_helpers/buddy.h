#pragma once

#include "../ds/ds.h"

namespace snmalloc
{
  /**
   * Class representing a buddy allocator
   *
   * Underlying node `Rep` representation is passed in.
   *
   * The allocator can handle blocks between inclusive MIN_SIZE_BITS and
   * exclusive MAX_SIZE_BITS.
   */
  template<typename Rep, size_t MIN_SIZE_BITS, size_t MAX_SIZE_BITS>
  class Buddy
  {
    std::array<RBTree<Rep>, MAX_SIZE_BITS - MIN_SIZE_BITS> trees{};
    // All RBtrees at or above this index should be empty.
    size_t empty_at_or_above{0};

    size_t to_index(size_t size)
    {
      SNMALLOC_ASSERT(size != 0);
      SNMALLOC_ASSERT(bits::is_pow2(size));
      auto log = snmalloc::bits::next_pow2_bits(size);
      SNMALLOC_ASSERT_MSG(
        log >= MIN_SIZE_BITS, "Size too big: {} log {}.", size, log);
      SNMALLOC_ASSERT_MSG(
        log < MAX_SIZE_BITS, "Size too small: {} log {}.", size, log);

      return log - MIN_SIZE_BITS;
    }

    void validate_block(typename Rep::Contents addr, size_t size)
    {
      SNMALLOC_ASSERT(bits::is_pow2(size));
      SNMALLOC_ASSERT(addr == Rep::align_down(addr, size));
      UNUSED(addr, size);
    }

    void invariant()
    {
#ifndef NDEBUG
      for (size_t i = empty_at_or_above; i < trees.size(); i++)
      {
        SNMALLOC_ASSERT(trees[i].is_empty());
      }
#endif
    }

  public:
    constexpr Buddy() = default;
    /**
     * Add a block to the buddy allocator.
     *
     * Blocks needs to be power of two size and aligned to the same power of
     * two.
     *
     * Returns null, if the block is successfully added. Otherwise, returns the
     * consolidated block that is MAX_SIZE_BITS big, and hence too large for
     * this allocator.
     */
    template<bool Consolidate>
    typename Rep::Contents
    add_block(typename Rep::Contents addr, size_t size)
    {
      auto idx = to_index(size);
      empty_at_or_above = bits::max(empty_at_or_above, idx + 1);

      validate_block(addr, size);

      auto path = trees[idx].get_root_path();

      if constexpr (Consolidate)
      {
        auto buddy = Rep::buddy(addr, size);
        bool contains_buddy = trees[idx].find(path, buddy);
        if (contains_buddy)
        {
          // Only check if we can consolidate after we know the buddy is in
          // the buddy allocator.  This is required to prevent possible
          // segfaults from looking at the buddies meta-data, which we only know
          // exists once we have found it in the red-black tree.
          if (Rep::can_consolidate(addr, size))
          {
            trees[idx].remove_path(path);

            // Add to next level cache
            size *= 2;
            addr = Rep::align_down(addr, size);
            if (size == bits::one_at_bit(MAX_SIZE_BITS))
            {
              // Invariant should be checked on all non-tail return paths.
              // Holds trivially here with current design.
              invariant();
              // Too big for this buddy allocator.
              return addr;
            }
            return add_block<Consolidate>(addr, size);
          }

          // Re-traverse as the path was to the buddy,
          // but the representation says we cannot combine.
          // We must find the correct place for this element.
          // Something clever could be done here, but it's not worth it.
          //        path = trees[idx].get_root_path();
          trees[idx].find(path, addr);
        }
      }
      else
      {
        trees[idx].find(path, addr);
      }
      trees[idx].insert_path(path, addr);
      invariant();
      return Rep::null;
    }

    /**
     * Removes a block of size from the buddy allocator.
     *
     * Return Rep::null if this cannot be satisfied.
     */
    typename Rep::Contents remove_block(size_t request_size)
    {
      size_t size = request_size;
      auto first_idx = to_index(size);
      invariant();
      auto idx = first_idx;
      typename Rep::Contents addr;

      // Search for a large enough block
      while (true)
      {
        if (idx >= empty_at_or_above)
        {
          empty_at_or_above = bits::min(empty_at_or_above, first_idx);
          return Rep::null;
        }

        addr = trees[idx].remove_min();
        if (addr != Rep::null)
        {
          validate_block(addr, size);
          break;
        }
        size *= 2;
        idx++;
      }

      // Split the block to the correct size.
      for (; idx > first_idx; idx--)
      {
        size = size >> 1;
        add_block<false>(addr, size);
        addr = Rep::offset(addr, size);
      }

      return addr;
    }
  };
} // namespace snmalloc
