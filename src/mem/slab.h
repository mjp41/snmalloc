#pragma once
#include "superslab.h"

namespace snmalloc
{
  class Slab
  {
  private:
    uint16_t pointer_to_index(void* p)
    {
      // Get the offset from the slab for a memory location.
      return static_cast<uint16_t>(pointer_diff(this, p));
    }

  public:
    Metaslab& get_meta()
    {
      Superslab* super = Superslab::get(this);
      return super->get_meta(this);
    }

    /**
     * Given a bumpptr and a fast_free_list head reference, builds a new free
     * list, and stores it in the fast_free_list. It will only create a page
     * worth of allocations, or one if the allocation size is larger than a
     * page.
     */
    static SNMALLOC_FAST_PATH void
    alloc_new_list(void*& bumpptr, FreeListHead& fast_free_list, size_t rsize)
    {
      void* slab_end = pointer_align_up<SLAB_SIZE>(pointer_offset(bumpptr, rsize));

      void* prev;
      void* currobject = nullptr;
      auto push = [&](void* next)
      {
        if (currobject == nullptr)
        {
          fast_free_list.value = next;
          prev = Metaslab::initial_key(next);
          fast_free_list.prev = prev;
        }
        else
        {
          Metaslab::store_next(prev, currobject, next);
          prev = currobject;
        }
        currobject = next;
      };

      auto finalbumpptr = bumpptr;
      size_t start_index[7] = {3, 5, 0, 2, 4, 1, 6};
      for (int i = 0; i < 7; i++)
      {
        void* newbumpptr = pointer_offset(bumpptr, rsize * start_index[i]);
        while (pointer_offset(newbumpptr, rsize) <= slab_end)
        {
          push(newbumpptr);
          newbumpptr = pointer_offset(newbumpptr, rsize * 7);
        }
        finalbumpptr = bits::max(finalbumpptr, pointer_offset(currobject, rsize));
      }
      bumpptr = finalbumpptr;

      push(nullptr);
    }

    // Returns true, if it deallocation can proceed without changing any status
    // bits. Note that this does remove the use from the meta slab, so it
    // doesn't need doing on the slow path.
    SNMALLOC_FAST_PATH bool dealloc_fast(Superslab* super, void* p)
    {
      Metaslab& meta = super->get_meta(this);
#ifdef CHECK_CLIENT
      if (meta.is_unused())
        error("Detected potential double free.");
#endif

      if (unlikely(meta.return_object()))
        return false;

      // Update the head and the next pointer in the free list.
      void* last = meta.end;
      void* prev = meta.prev;

      // Set the last element to point to the new element.
      Metaslab::store_next(prev, last, p);

      // Set the end to the memory being deallocated.
      meta.end = p;
      meta.prev = last;
      SNMALLOC_ASSERT(meta.valid_head());

      return true;
    }

    // If dealloc fast returns false, then call this.
    // This does not need to remove the "use" as done by the fast path.
    // Returns a complex return code for managing the superslab meta data.
    // i.e. This deallocation could make an entire superslab free.
    SNMALLOC_SLOW_PATH typename Superslab::Action
    dealloc_slow(SlabList* sl, Superslab* super, void* p)
    {
      Metaslab& meta = super->get_meta(this);
      meta.debug_slab_invariant(this);

      if (meta.is_full())
      {
        // We are not on the sizeclass list.
        if (meta.allocated == 1)
        {
          // Dealloc on the superslab.
          if (is_short())
            return super->dealloc_short_slab();

          return super->dealloc_slab(this);
        }
        SNMALLOC_ASSERT(meta.head == nullptr);
        meta.head = p;
        meta.end = p;
        meta.prev = Metaslab::initial_key(p);
        Metaslab::store_next(Metaslab::initial_key(p), p, nullptr);
        meta.needed = meta.allocated - 1;

        // Push on the list of slabs for this sizeclass.
        sl->insert_prev(&meta);
        meta.debug_slab_invariant(this);
        return Superslab::NoSlabReturn;
      }

      // Check free list is well-formed.
      auto prev = Metaslab::initial_key(meta.head);
      auto curr = meta.head;
      while (curr != nullptr)
      {
        if (unlikely(((address_cast(prev) ^ address_cast(curr)) >= SLAB_SIZE)))
        {
          error("Heap corruption detected!");
        }
        auto next = Metaslab::follow_next(prev, curr);
        prev = curr;
        curr = next;
      }

      // Remove from the sizeclass list and dealloc on the superslab.
      meta.remove();

      if (is_short())
        return super->dealloc_short_slab();

      return super->dealloc_slab(this);
    }

    bool is_short()
    {
      return Metaslab::is_short(this);
    }
  };
} // namespace snmalloc
