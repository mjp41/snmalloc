#pragma once

#include "../ds/ds.h"

namespace snmalloc
{
  template<typename Rep>
  class BuddyEntry
  {
    RBTree<Rep> tree{};
    typename Rep::Contents slots[2]{};
    size_t count{0};

  public:
    constexpr BuddyEntry() = default;

    void invariant()
    {
      SNMALLOC_ASSERT(count > 2 || tree.is_empty());
      SNMALLOC_ASSERT(count != 1 || slots[0] != Rep::null);
      SNMALLOC_ASSERT(count != 2 || slots[1] != Rep::null);
    }

    bool is_empty()
    {
      return count == 0 && tree.is_empty();
    }

    template<bool Consolidate>
    typename Rep::Contents add_block(typename Rep::Contents addr, size_t size)
    {
      SNMALLOC_ASSERT(Rep::align_down(addr, size) == addr);
      if (count == 0)
      {
        slots[0] = addr;
        count++;
        invariant();
        return Rep::null;
      }

      if (count == 1)
      {
        if (Rep::buddy(slots[0], size) == addr)
        {
          count = 0;
          slots[0] = Rep::null;
          addr = Rep::align_down(addr, size * 2);
          invariant();
          return addr;
        }
        slots[1] = addr;
        count++;
        invariant();
        return Rep::null;
      }

      if (count == 2)
      {
        tree.insert_elem(slots[0]);
        tree.insert_elem(slots[1]);
      }

      auto path = tree.get_root_path();
      auto buddy = Rep::buddy(addr, size);
      bool contains_buddy = tree.find(path, buddy);
      if (contains_buddy)
      {
        // Only check if we can consolidate after we know the buddy is in
        // the buddy allocator.  This is required to prevent possible
        // segfaults from looking at the buddies meta-data, which we only know
        // exists once we have found it in the red-black tree.
        if (Rep::can_consolidate(addr, size))
        {
          tree.remove_path(path);

          count--;
          if (count == 2)
          {
            slots[0] = tree.remove_min();
            slots[1] = tree.remove_min();
          }
          if (count == 1)
          {
            slots[0] = tree.remove_min();
          }

          // Add to next level cache
          invariant();
          return Rep::align_down(addr, size * 2);
        }

        // Re-traverse as the path was to the buddy,
        // but the representation says we cannot combine.
        // We must find the correct place for this element.
        // Something clever could be done here, but it's not worth it.
        path = tree.get_root_path();
        tree.find(path, addr);
      }
      count++;
      tree.insert_path(path, addr);
      invariant();
      return Rep::null;
    }

    typename Rep::Contents remove_block()
    {
      if (count == 0)
        return Rep::null;

      if (count < 3)
        return slots[--count];

      auto addr = tree.remove_min();
      count--;
      if (count == 2)
      {
        slots[1] = tree.remove_min();
        slots[0] = tree.remove_min();
      }
      invariant();
      return addr;
    }
  };

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
    std::array<BuddyEntry<Rep>, MAX_SIZE_BITS - MIN_SIZE_BITS> entries{};
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
      for (auto& entry : entries)
      {
        entry.invariant();
      }
      for (size_t i = empty_at_or_above; i < entries.size(); i++)
      {
        SNMALLOC_ASSERT(entries[i].is_empty());
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
    typename Rep::Contents add_block(typename Rep::Contents addr, size_t size)
    {
      auto idx = to_index(size);
      validate_block(addr, size);

      while (size < bits::one_at_bit(MAX_SIZE_BITS))
      {
        empty_at_or_above = bits::max(empty_at_or_above, idx + 1);
        addr = entries[idx].template add_block<Consolidate>(addr, size);
        if (addr == Rep::null)
          return Rep::null;

        size = size * 2;
        idx++;
      }

      return addr;
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

        addr = entries[idx].remove_block();
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

      invariant();
      return addr;
    }
  };
} // namespace snmalloc
