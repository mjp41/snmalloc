#pragma once

#include "../ds/cdllist.h"
#include "../ds/dllist.h"
#include "../ds/helpers.h"
#include "sizeclass.h"

namespace snmalloc
{
  class Slab;

  struct FreeListHead
  {
    // Use a value with bottom bit set for empty list.
    void* value = nullptr;
    void* prev = nullptr;
  };

  using SlabList = CDLLNode;
  using SlabLink = CDLLNode;

  SNMALLOC_FAST_PATH Slab* get_slab(SlabLink* sl)
  {
    return pointer_align_down<SLAB_SIZE, Slab>(sl);
  }

  SNMALLOC_FAST_PATH void* fake_prev(void* p)
  {
    return pointer_align_down<SLAB_SIZE, void>(p);
  }

  static_assert(
    sizeof(SlabLink) <= MIN_ALLOC_SIZE,
    "Need to be able to pack a SlabLink into any free small alloc");

  // The Metaslab represent the status of a single slab.
  // This can be either a short or a standard slab.
  class Metaslab : public SlabLink
  {
  public:
    /**
     *  Pointer to first free entry in this slab
     *
     *  The list will be (allocated - needed) long.
     */
    void* head = nullptr;

    // Form a queue
    void* end = nullptr;
    void* prev = nullptr;
    void* dummy;
    void* dummy2;

    /**
     *  How many entries are not in the free list of slab, i.e.
     *  how many entries are needed to fully free this slab.
     *
     *  In the case of a fully allocated slab, where prev==0 needed
     *  will be 1. This enables 'return_object' to detect the slow path
     *  case with a single operation subtract and test.
     */
    uint16_t needed = 0;

    /**
     *  How many entries have been allocated from this slab.
     */
    uint16_t allocated = 0;

    uint8_t sizeclass;
    // Initially zero to encode the superslabs relative list of slabs.
    uint8_t next = 0;

    /**
     * Updates statistics for adding an entry to the free list, if the
     * slab is either
     *  - empty adding the entry to the free list, or
     *  - was full before the subtraction
     * this returns true, otherwise returns false.
     */
    bool return_object()
    {
      return (--needed) == 0;
    }

    bool is_unused()
    {
      return needed == 0;
    }

    bool is_full()
    {
      auto result = get_prev() == nullptr;
      SNMALLOC_ASSERT(!result || head == nullptr);
      return result;
    }

    SNMALLOC_FAST_PATH void set_full()
    {
      SNMALLOC_ASSERT(head == nullptr);
      // Set needed to 1, so that "return_object" will return true after calling
      // set_full
      needed = 1;
      null_prev();
    }

    /// Value used to check for corruptions in a block
    static constexpr size_t POISON =
      static_cast<size_t>(bits::is64() ? 0xDEADBEEFDEADBEEF : 0xDEADBEEF);

    /// Store next pointer in a block. In Debug using magic value to detect some
    /// simple corruptions.
    static SNMALLOC_FAST_PATH void store_next(void* key, void* p, void* next)
    {
      *static_cast<void**>(p) =
        (void*)encode_next((uintptr_t)key, (uintptr_t)next);
    }

    inline static uintptr_t global_key = 0x9999'9999'9999'9999;
    static void* initial_key(void* p)
    {
      return (void*)(((uintptr_t)p) + SUPERSLAB_SIZE);
    }

    static uintptr_t encode_next(uintptr_t local_key, uintptr_t next)
    {
      if constexpr (aal_supports<IntegerPointers>)
      {
        constexpr uintptr_t MASK = bits::one_at_bit(bits::BITS / 2) - 1;
        // Mix in local_key
        auto key = local_key ^ global_key;
        next ^= (((next&MASK) + 1) * key) & ~MASK;
      }
      return next;
    }

    /// Accessor function for the next pointer in a block.
    /// In Debug checks for simple corruptions.
    static SNMALLOC_FAST_PATH void* follow_next(void* prev, void* node)
    {
      return (void*)encode_next(
        (uintptr_t)prev, (uintptr_t) * static_cast<void**>(node));
    }

    bool valid_head()
    {
      size_t size = sizeclass_to_size(sizeclass);
      size_t slab_end = (address_cast(head) | ~SLAB_MASK) + 1;
      uintptr_t allocation_start =
        remove_cache_friendly_offset(address_cast(head), sizeclass);

      return (slab_end - allocation_start) % size == 0;
    }

    static Slab* get_slab(const void* p)
    {
      return pointer_align_down<SLAB_SIZE, Slab>(const_cast<void*>(p));
    }

    static bool is_short(Slab* p)
    {
      return pointer_align_down<SUPERSLAB_SIZE>(p) == p;
    }

    bool is_start_of_object(void* p)
    {
      return is_multiple_of_sizeclass(
        sizeclass_to_size(sizeclass),
        pointer_diff(p, pointer_align_up<SLAB_SIZE>(pointer_offset(p, 1))));
    }

    /**
     * Takes a free list out of a slabs meta data.
     * Returns the link as the allocation, and places the free list into the
     * `fast_free_list` for further allocations.
     */
    template<ZeroMem zero_mem, SNMALLOC_CONCEPT(ConceptPAL) PAL>
    SNMALLOC_FAST_PATH void* alloc(FreeListHead& fast_free_list, size_t rsize)
    {
      SNMALLOC_ASSERT(rsize == sizeclass_to_size(sizeclass));
      SNMALLOC_ASSERT(!is_full());

      auto slab = get_slab(head);
      debug_slab_invariant(slab);

      // Use first element as the allocation
      void* p = head;
      // Put the rest in allocators small_class fast free list.
      fast_free_list.value = Metaslab::follow_next(nullptr, p);
      fast_free_list.prev = p;
      head = nullptr;

      // Terminate queue
      Metaslab::store_next(prev, end, nullptr);

      // Treat stealing the free list as allocating it all.
      needed = allocated;
      remove();
      set_full();

      p = remove_cache_friendly_offset(p, sizeclass);
      SNMALLOC_ASSERT(is_start_of_object(p));

      debug_slab_invariant(slab);

      if constexpr (zero_mem == YesZero)
      {
        if (rsize < PAGE_ALIGNED_SIZE)
          PAL::zero(p, rsize);
        else
          PAL::template zero<true>(p, rsize);
      }
      else
      {
        UNUSED(rsize);
      }

      return p;
    }

    void debug_slab_invariant(Slab* slab)
    {
#if !defined(NDEBUG) && !defined(SNMALLOC_CHEAP_CHECKS)
      bool is_short = Metaslab::is_short(slab);

      if (is_full())
      {
        // There is no free list to validate
        return;
      }

      if (is_unused())
        return;

      size_t size = sizeclass_to_size(sizeclass);
      size_t offset = get_initial_offset(sizeclass, is_short);
      size_t accounted_for = needed * size + offset;

      // Block is not full
      SNMALLOC_ASSERT(SLAB_SIZE > accounted_for);

      // Walk bump-free-list-segment accounting for unused space
      void* curr = head;
      void* prev = nullptr;
      while (prev != end)
      {
        // Check we are looking at a correctly aligned block
        void* start = remove_cache_friendly_offset(curr, sizeclass);
        SNMALLOC_ASSERT(((pointer_diff(slab, start) - offset) % size) == 0);

        // Account for free elements in free list
        accounted_for += size;
        SNMALLOC_ASSERT(SLAB_SIZE >= accounted_for);

        // Iterate bump/free list segment
        auto next = follow_next(prev, curr);
        prev = curr;
        curr = next;
      }

      auto bumpptr = (allocated * size) + offset;
      // Check we haven't allocaated more than gits in a slab
      SNMALLOC_ASSERT(bumpptr <= SLAB_SIZE);

      // Account for to be bump allocated space
      accounted_for += SLAB_SIZE - bumpptr;

      SNMALLOC_ASSERT(!is_full());

      // All space accounted for
      SNMALLOC_ASSERT(SLAB_SIZE == accounted_for);
#else
      UNUSED(slab);
#endif
    }
  };
} // namespace snmalloc
