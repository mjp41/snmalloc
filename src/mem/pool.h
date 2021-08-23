#pragma once

#include "../ds/flaglock.h"
#include "../ds/mpmcstack.h"
#include "../pal/pal_concept.h"
#include "pooled.h"
#include "slaballocator.h"

namespace snmalloc
{
  /**
   * Pool of a particular type of object.
   *
   * This pool will never return objects to the OS.  It maintains a list of all
   * objects ever allocated that can be iterated (not concurrency safe).  Pooled
   * types can be acquired from the pool, and released back to the pool. This is
   * concurrency safe.
   *
   * This is used to bootstrap the allocation of allocators.
   */
  template<class T>
  class PoolState
  {
    template<
      typename TT,
      typename SharedStateHandle,
      PoolState<TT>& get_state()>
    friend class Pool;

  private:
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    MPMCStack<T, PreZeroed> stack;
    T* list{nullptr};

  public:
    constexpr PoolState() = default;
  };

  /**
   * Class used to instantiate a global non-allocator PoolState.
   */
  template<typename T>
  class SingletonPoolState
  {
    static inline PoolState<T> state;

  public:
    static PoolState<T>& pool()
    {
      return state;
    }
  };

  template<typename T, typename SharedStateHandle, PoolState<T>& get_state()>
  class Pool
  {
  public:
    template<typename... Args>
    static T* acquire(Args&&... args)
    {
      PoolState<T>& pool = get_state();
      T* p = pool.stack.pop();

      if (p != nullptr)
      {
        p->set_in_use();
        return p;
      }

      p = ChunkAllocator::alloc_meta_data<T, SharedStateHandle>(
        nullptr, std::forward<Args>(args)...);

      if (p == nullptr)
      {
        SharedStateHandle::Pal::error(
          "Failed to initialise thread local allocator.");
      }

      FlagLock f(pool.lock);
      p->list_next = pool.list;
      pool.list = p;

      p->set_in_use();
      return p;
    }

    /**
     * Return to the pool an object previously retrieved by `acquire`
     *
     * Do not return objects from `extract`.
     */
    static void release(T* p)
    {
      // The object's destructor is not run. If the object is "reallocated", it
      // is returned without the constructor being run, so the object is reused
      // without re-initialisation.
      p->reset_in_use();
      get_state().stack.push(p);
    }

    static T* extract(T* p = nullptr)
    {
      // Returns a linked list of all objects in the stack, emptying the stack.
      if (p == nullptr)
        return get_state().stack.pop_all();

      return p->next;
    }

    /**
     * Return to the pool a list of object previously retrieved by `extract`
     *
     * Do not return objects from `acquire`.
     */
    static void restore(T* first, T* last)
    {
      // Pushes a linked list of objects onto the stack. Use to put a linked
      // list returned by extract back onto the stack.
      get_state().stack.push(first, last);
    }

    static T* iterate(T* p = nullptr)
    {
      if (p == nullptr)
        return get_state().list;

      return p->list_next;
    }
  };
} // namespace snmalloc
