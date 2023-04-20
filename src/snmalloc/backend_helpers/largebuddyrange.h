#pragma once

#include "../ds/ds.h"
#include "../mem/mem.h"
#include "buddy.h"
#include "empty_range.h"
#include "range_helpers.h"

#include <string>

namespace snmalloc
{
  /**
   * Class for using the pagemap entries for the buddy allocator.
   */
  template<SNMALLOC_CONCEPT(IsWritablePagemap) Pagemap>
  class BuddyChunkRep
  {
  public:
    /*
     * The values we store in our rbtree are the addresses of (combined spans
     * of) chunks of the address space; as such, bits in (MIN_CHUNK_SIZE - 1)
     * are unused and so the RED_BIT is packed therein.  However, in practice,
     * these are not "just any" uintptr_t-s, but specifically the uintptr_t-s
     * inside the Pagemap's BackendAllocator::Entry structures.
     *
     * The BackendAllocator::Entry provides us with helpers that guarantee that
     * we use only the bits that we are allowed to.
     * @{
     */
    using Handle = MetaEntryBase::BackendStateWordRef;
    using Contents = uintptr_t;
    ///@}

    /**
     * The bit that we will use to mark an entry as red.
     * This has constraints in two directions, it must not be one of the
     * reserved bits from the perspective of the meta entry and it must not be
     * a bit that is a valid part of the address of a chunk.
     * @{
     */
    static constexpr address_t RED_BIT = 1 << 8;

    static_assert(RED_BIT < MIN_CHUNK_SIZE);
    static_assert(MetaEntryBase::is_backend_allowed_value(
      MetaEntryBase::Word::One, RED_BIT));
    static_assert(MetaEntryBase::is_backend_allowed_value(
      MetaEntryBase::Word::Two, RED_BIT));
    ///@}

    /// The value of a null node, as returned by `get`
    static constexpr Contents null = 0;
    /// The value of a null node, as stored in a `uintptr_t`.
    static constexpr Contents root = 0;

    /**
     * Set the value.  Preserve the red/black colour.
     */
    static void set(Handle ptr, Contents r)
    {
      ptr = r | (static_cast<address_t>(ptr.get()) & RED_BIT);
    }

    /**
     * Returns the value, stripping out the red/black colour.
     */
    static Contents get(const Handle ptr)
    {
      return ptr.get() & ~RED_BIT;
    }

    /**
     * Returns a pointer to the tree node for the specified address.
     */
    static Handle ref(bool direction, Contents k)
    {
      // Special case for accessing the null entry.  We want to make sure
      // that this is never modified by the back end, so we make it point to
      // a constant entry and use the MMU to trap even in release modes.
      static const Contents null_entry = 0;
      if (SNMALLOC_UNLIKELY(address_cast(k) == 0))
      {
        return {const_cast<Contents*>(&null_entry)};
      }
      auto& entry = Pagemap::template get_metaentry_mut<false>(address_cast(k));
      if (direction)
        return entry.get_backend_word(Pagemap::Entry::Word::One);

      return entry.get_backend_word(Pagemap::Entry::Word::Two);
    }

    static bool is_red(Contents k)
    {
      return (ref(true, k).get() & RED_BIT) == RED_BIT;
    }

    static void set_red(Contents k, bool new_is_red)
    {
      if (new_is_red != is_red(k))
      {
        auto v = ref(true, k);
        v = v.get() ^ RED_BIT;
      }
      SNMALLOC_ASSERT(is_red(k) == new_is_red);
    }

    static Contents offset(Contents k, size_t size)
    {
      return k + size;
    }

    static Contents buddy(Contents k, size_t size)
    {
      return k ^ size;
    }

    static Contents align_down(Contents k, size_t size)
    {
      return k & ~(size - 1);
    }

    static bool compare(Contents k1, Contents k2)
    {
      return k1 > k2;
    }

    static bool equal(Contents k1, Contents k2)
    {
      return k1 == k2;
    }

    static uintptr_t printable(Contents k)
    {
      return k;
    }

    /**
     * Convert the pointer wrapper into something that the snmalloc debug
     * printing code can print.
     */
    static address_t printable(Handle k)
    {
      return k.printable_address();
    }

    /**
     * Returns the name for use in debugging traces.  Not used in normal builds
     * (release or debug), only when tracing is enabled.
     */
    static const char* name()
    {
      return "BuddyChunkRep";
    }

    static bool can_consolidate(Contents k, size_t size)
    {
      // Need to know both entries exist in the pagemap.
      // This must only be called if that has already been
      // ascertained.
      // The buddy could be in a part of the pagemap that has
      // not been registered and thus could segfault on access.
      auto larger = bits::max(k, buddy(k, size));
      auto& entry =
        Pagemap::template get_metaentry_mut<false>(address_cast(larger));
      return !entry.is_boundary();
    }
  };

  /**
   * Used to represent a consolidating range of memory.  Uses a buddy allocator
   * to consolidate adjacent blocks.
   *
   * ParentRange - Represents the range to get memory from to fill this range.
   *
   * REFILL_SIZE_BITS - Maximum size of a refill, may ask for less during warm
   * up phase.
   *
   * MAX_SIZE_BITS - Maximum size that this range will store.
   *
   * Pagemap - How to access the pagemap, which is used to store the red black
   * tree nodes for the buddy allocators.
   *
   * MIN_REFILL_SIZE_BITS - The minimum size that the ParentRange can be asked
   * for
   */
  template<
    size_t REFILL_SIZE_BITS,
    size_t MAX_SIZE_BITS,
    SNMALLOC_CONCEPT(IsWritablePagemap) Pagemap,
    size_t MIN_REFILL_SIZE_BITS = 0>
  class LargeBuddyRange
  {
    static_assert(
      REFILL_SIZE_BITS <= MAX_SIZE_BITS, "REFILL_SIZE_BITS > MAX_SIZE_BITS");
    static_assert(
      MIN_REFILL_SIZE_BITS <= REFILL_SIZE_BITS,
      "MIN_REFILL_SIZE_BITS > REFILL_SIZE_BITS");

    /**
     * Maximum size of a refill
     */
    static constexpr size_t REFILL_SIZE = bits::one_at_bit(REFILL_SIZE_BITS);

    /**
     * Minimum size of a refill
     */
    static constexpr size_t MIN_REFILL_SIZE =
      bits::one_at_bit(MIN_REFILL_SIZE_BITS);

  public:
    template<typename ParentRange = EmptyRange<>>
    class Type : public ContainsParent<ParentRange>
    {
      using ContainsParent<ParentRange>::parent;

      /**
       * The size of memory requested so far.
       *
       * This is used to determine the refill size.
       */
      size_t requested_total = 0;

      /**
       * The size of memory provided so far.
       *
       * This is used to determine how much memory is in the
       * buddy allocator, and if some should be returned.
       */
      size_t provided_total = 0;

      /**
       * Buddy allocator used to represent this range of memory.
       */
      Buddy<BuddyChunkRep<Pagemap>, MIN_CHUNK_BITS, MAX_SIZE_BITS> buddy_large;

      /**
       * The parent might not support deallocation if this buddy allocator
       * covers the whole range.  Uses template insanity to make this work.
       */
      template<bool exists = MAX_SIZE_BITS != (bits::BITS - 1)>
      std::enable_if_t<exists, bool>
      parent_dealloc_range(capptr::Arena<void> base, size_t size, bool force)
      {
        static_assert(
          MAX_SIZE_BITS != (bits::BITS - 1), "Don't set SFINAE parameter");
        auto result = parent.dealloc_range(base, size, force);
        if (result)
          requested_total -= size;
        return result;
      }

      Range parent_alloc_range(SizeSpec size)
      {
        auto result = parent.alloc_range(size);
        if (result.base != nullptr)
        {
          requested_total += result.length;
        }
        return result;
      }

      void dealloc_overflow(capptr::Arena<void> overflow)
      {
        if constexpr (MAX_SIZE_BITS != (bits::BITS - 1))
        {
          if (overflow != nullptr)
          {
            parent_dealloc_range(overflow, bits::one_at_bit(MAX_SIZE_BITS), true);
          }
          else
          {
            auto threshold = bits::max(provided_total * 8, 16 * MIN_CHUNK_SIZE);
            // It is very unlikely to hit provided_total == 0 during execution of a thread,
            // so this is probably the thread is shutting down, return all memory at
            // this point.
            if (provided_total == 0)
              threshold = 0;
            while (requested_total > threshold)
            {
              invariant();
              auto [ptr, size] = buddy_large.remove_largest();
              auto capptr = capptr::Arena<void>::unsafe_from(reinterpret_cast<void*>(ptr));
              if (capptr == nullptr)
              {
                message<1024>("Error no memory. Requested total = {}, provided total = {}  @{}", requested_total, provided_total, this);
                error("Unreachable.");
              }
              auto result = parent_dealloc_range(
                capptr,
                size,
                false);
              if (!result)
              {
                buddy_large.add_block(ptr, size);
                break;
              }
            }
          }
        }
        else
        {
          if (overflow != nullptr)
            abort();
        }
      }

      /**
       * Add a range of memory to the address space.
       * Divides blocks into power of two sizes with natural alignment
       */
      void add_range(capptr::Arena<void> base, size_t length)
      {
        auto lost = range_to_pow_2_blocks<MIN_CHUNK_BITS>(
          base, length, [this](capptr::Arena<void> base, size_t align, bool) {
            auto overflow =
              capptr::Arena<void>::unsafe_from(reinterpret_cast<void*>(
                buddy_large.add_block(base.unsafe_uintptr(), align)));
            if (overflow != nullptr)
              dealloc_overflow(overflow);
          });

        // If the underlying range was not aligned, then we simply drop the memory
        // TODO is the correct thing to do.
        requested_total -= lost;
      }

      Range refill(SizeSpec size)
      {
        if (ParentRange::Aligned)
        {
          // Use amount currently requested to determine refill size.
          // This will gradually increase the usage of the parent range.
          // So small examples can grow local caches slowly, and larger
          // examples will grow them by the refill size.
          //
          // The heuristic is designed to allocate the following sequence for
          // 16KiB requests 16KiB, 16KiB, 32Kib, 64KiB, ..., REFILL_SIZE/2,
          // REFILL_SIZE, REFILL_SIZE, ... Hence if this if they are coming from
          // a contiguous aligned range, then they could be consolidated.  This
          // depends on the ParentRange behaviour.
          size_t refill_size = bits::min(REFILL_SIZE, requested_total);
          refill_size = bits::max(refill_size, MIN_REFILL_SIZE);
          refill_size = bits::max(refill_size, size.required);
          refill_size = bits::next_pow2(refill_size);

          auto refill_range = parent_alloc_range({refill_size, size.required});
          if (refill_range.base != nullptr)
          {
            add_range(
              pointer_offset(refill_range.base, size.required),
              refill_range.length - size.required);
          }
          return {refill_range.base, size.required};
        }

        // Note the unaligned parent path does not use
        // requested_total in the heuristic for the initial size
        // this is because the request needs to introduce alignment.
        // Currently the unaligned variant is not used as a local cache.
        // So the gradual growing of refill_size is not needed.

        // Need to overallocate to get the alignment right.
        bool overflow = false;
        size_t needed_size = bits::umul(size.required, 2, overflow);
        if (overflow)
        {
          return {nullptr, 0};
        }

        auto refill_size = bits::max(needed_size, REFILL_SIZE);
        while (needed_size <= refill_size)
        {
          auto refill = parent_alloc_range({refill_size});

          if (refill.base != nullptr)
          {
            add_range(refill.base, refill_size);

            SNMALLOC_ASSERT(refill_size < bits::one_at_bit(MAX_SIZE_BITS));
            static_assert(
              (REFILL_SIZE < bits::one_at_bit(MAX_SIZE_BITS)) ||
                ParentRange::Aligned,
              "Required to prevent overflow.");

            return alloc_range_impl(size);
          }

          refill_size >>= 1;
        }

        return {nullptr, 0};
      }

      SNMALLOC_FAST_PATH Range alloc_range_impl(SizeSpec spec)
      {
        SNMALLOC_ASSERT(spec.required >= MIN_CHUNK_SIZE);
        SNMALLOC_ASSERT(bits::is_pow2(spec.required));

        Range result{nullptr, 0};
        if (spec.required >= (bits::one_at_bit(MAX_SIZE_BITS) - 1))
        {
          if (ParentRange::Aligned)
            result = parent_alloc_range(spec.required);
        }
        else
        {
          auto [p, size] = buddy_large.remove_block(spec);
          auto base =
            capptr::Arena<void>::unsafe_from(reinterpret_cast<void*>(p));
          if (base == nullptr)
            result = refill(spec.required);
          else
            result = {base, size};
        }

        return result;
      }

    public:
      static constexpr bool Aligned = true;

      static constexpr bool ConcurrencySafe = false;

      /* The large buddy allocator always deals in Arena-bounded pointers. */
      using ChunkBounds = capptr::bounds::Arena;
      static_assert(
        std::is_same_v<typename ParentRange::ChunkBounds, ChunkBounds>);

      constexpr Type() = default;

      Range alloc_range(SizeSpec size)
      {
        auto result = alloc_range_impl(size);
        if (result.base != nullptr)
        {
          provided_total += result.length;
        }
        invariant();
        return result;
      }

      bool dealloc_range(capptr::Arena<void> base, size_t size, bool)
      {
        SNMALLOC_ASSERT(size >= MIN_CHUNK_SIZE);
        SNMALLOC_ASSERT(bits::is_pow2(size));

        provided_total -= size;

        if constexpr (MAX_SIZE_BITS != (bits::BITS - 1))
        {
          if (size >= (bits::one_at_bit(MAX_SIZE_BITS) - 1))
          {
            parent_dealloc_range(base, size, true);
            return true;
          }
        }

        auto overflow =
          capptr::Arena<void>::unsafe_from(reinterpret_cast<void*>(
            buddy_large.add_block(base.unsafe_uintptr(), size)));
        dealloc_overflow(overflow);
        invariant();
        return true;
      }

      void invariant()
      {
#ifndef NDEBUG
        size_t contains_bytes = buddy_large.contains_bytes();
        if (requested_total != (provided_total + contains_bytes))
        {
          message<1024>(
            "LargeBuddyInvariant failed: {} - ({} + {}) = {} @{}",
            requested_total,
            provided_total,
            contains_bytes,
            requested_total - (provided_total + contains_bytes),
            this);
          abort();
        }
#endif
      }
    };
  };
} // namespace snmalloc
