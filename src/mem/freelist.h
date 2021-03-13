#pragma once

#include "../ds/address.h"
#include "../ds/cdllist.h"
#include "../ds/dllist.h"
#include "../ds/helpers.h"
#include "allocconfig.h"

#include <iostream>

namespace snmalloc
{
#ifdef CHECK_CLIENT
  inline static uintptr_t global_key = 0x9999'9999'9999'9999;

  inline static uintptr_t initial_key(void* p)
  {
    return address_cast(p) + 1;
  }
#endif

  static inline bool different_slab(uintptr_t p1, uintptr_t p2)
  {
    return ((p1 ^ p2) >= SLAB_SIZE);
  }

  static inline bool different_slab(uintptr_t p1, void* p2)
  {
    return different_slab(p1, address_cast(p2));
  }

  static inline bool different_slab(void* p1, void* p2)
  {
    return different_slab(address_cast(p1), address_cast(p2));
  }

  /**
   * Free objects within each slab point directly to the next (contrast
   * SlabLink, which chain different Slabs of the same sizeclass
   * together).
   */
  class FreeObject
  {
    FreeObject* next_object;

    static FreeObject* encode(uintptr_t local_key, FreeObject* next_object)
    {
#ifdef CHECK_CLIENT
      if constexpr (aal_supports<IntegerPointers>)
      {
        auto next = address_cast(next_object);
        constexpr uintptr_t MASK = bits::one_at_bit(bits::BITS / 2) - 1;
        // Mix in local_key
        auto key = local_key ^ global_key;
        next ^= (((next & MASK) + 1) * key) & ~MASK;
        next_object = reinterpret_cast<FreeObject*>(next);
      }
#else
      UNUSED(local_key);
#endif
      return next_object;
    }

  public:
    static FreeObject* make(void* p)
    {
      return static_cast<FreeObject*>(p);
    }

    FreeObject* read_next(uintptr_t key)
    {
      auto next = encode(key, next_object);
      return next;
    }

    void store_next(FreeObject* next, uintptr_t key)
    {
      next_object = encode(key, next);
      SNMALLOC_ASSERT(next == read_next(key));
    }
  };

  class FreeObjectCursor
  {
    FreeObject* curr = nullptr;
#ifdef CHECK_CLIENT
    uintptr_t prev = 0;
#endif

    uintptr_t get_prev()
    {
#ifdef CHECK_CLIENT
      return prev;
#else
      return 0;
#endif
    }

    void update_cursor(FreeObject* next)
    {
#ifdef CHECK_CLIENT
#  ifndef NDEBUG
      if (next != nullptr)
      {
        if (unlikely(different_slab(prev, next)))
        {
          error("Heap corruption - free list corrupted!");
        }
      }
#  endif
      prev = address_cast(curr);
#endif
      curr = next;
    }

  public:
    FreeObject* get_curr()
    {
      return curr;
    }

    void move_next()
    {
#ifdef CHECK_CLIENT
      if (unlikely(different_slab(prev, curr)))
      {
        error("Heap corruption - free list corrupted!");
      }
#endif
      update_cursor(curr->read_next(get_prev()));
    }

    void set_next(FreeObject* next)
    {
      curr->store_next(next, get_prev());
    }

    void set_next_and_move(FreeObject* next)
    {
      set_next(next);
      update_cursor(next);
    }

    void reset_cursor(FreeObject* next)
    {
#ifdef CHECK_CLIENT
      prev = initial_key(next);
#endif
      curr = next;
    }
  };

  /**
   * Used to iterate a free list in object space.
   *
   * Checks signing of pointers
   */
  class FreeListIter
  {
  protected:
    FreeObjectCursor front;

  public:
    void* peek()
    {
      return front.get_curr();
    }

    bool empty()
    {
      return peek() == nullptr;
    }

    void* take()
    {
      auto c = front.get_curr();
      front.move_next();
      return c;
    }
  };

  /**
   * Used to build a free list in object space.
   *
   * Checks signing of pointers
   */
  class FreeListBuilder : public FreeListIter
  {
    FreeObjectCursor end;

  public:
    void open(void* n)
    {
      SNMALLOC_ASSERT(empty());
      FreeObject* next = FreeObject::make(n);
      end.reset_cursor(next);
      front.reset_cursor(next);
    }

    void add(void* n)
    {
      SNMALLOC_ASSERT(!different_slab(end.get_curr(), n));
      FreeObject* next = FreeObject::make(n);
      end.set_next_and_move(next);
    }

    void terminate()
    {
      if (!empty())
        end.set_next(nullptr);
    }

    void close(FreeListIter& dst)
    {
      terminate();
      dst = *this;
      init();
    }

    void init()
    {
      front.reset_cursor(nullptr);
    }
  };
} // namespace snmalloc