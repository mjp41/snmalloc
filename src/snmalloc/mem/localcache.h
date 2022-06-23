#pragma once

#include "../ds/ds.h"
#include "freelist.h"
#include "remotecache.h"
#include "sizeclasstable.h"

#include <string.h>

namespace snmalloc
{
  inline static SNMALLOC_FAST_PATH capptr::Alloc<void>
  finish_alloc_no_zero(freelist::HeadPtr p, smallsizeclass_t sizeclass)
  {
    SNMALLOC_ASSERT(is_start_of_object(
      sizeclass_t::from_small_class(sizeclass), address_cast(p)));
    UNUSED(sizeclass);

    return p.as_void();
  }

  template<ZeroMem zero_mem, typename Config>
  inline static SNMALLOC_FAST_PATH capptr::Alloc<void>
  finish_alloc(freelist::HeadPtr p, smallsizeclass_t sizeclass)
  {
    auto r = finish_alloc_no_zero(p, sizeclass);

    if constexpr (zero_mem == YesZero)
      Config::Pal::zero(r.unsafe_ptr(), sizeclass_to_size(sizeclass));

    // TODO: Should this be zeroing the free Object state, in the non-zeroing
    // case?

    return r;
  }

  // This is defined on its own, so that it can be embedded in the
  // thread local fast allocator, but also referenced from the
  // thread local core allocator.
  struct LocalCache
  {
    // Free list per small size class.  These are used for
    // allocation on the fast path. This part of the code is inspired by
    // mimalloc.
    freelist::Iter<> small_fast_free_lists[NUM_SMALL_SIZECLASSES] = {};

    // This is the entropy for a particular thread.
    LocalEntropy entropy;

    // Pointer to the remote allocator message_queue, used to check
    // if a deallocation is local.
    RemoteAllocator* remote_allocator;

    std::atomic<size_t> in_use{false};
    static inline std::atomic<size_t> block_use{0};

    void acquire()
    {
#ifdef SNMALLOC_ATOMIC_PAUSE
      while (SNMALLOC_UNLIKELY(in_use.exchange(1) == 1))
        Aal::pause();
#elif defined (SNMALLOC_ATOMIC_PAUSE_INCREMENT) || true
      while (SNMALLOC_UNLIKELY(in_use.fetch_add(1) != 0))
      {
        Aal::pause();
        in_use--;
      }  
#else
      in_use.store(1, std::memory_order_relaxed);
      std::atomic_signal_fence(std::memory_order_seq_cst);
      while (SNMALLOC_UNLIKELY(block_use.load(std::memory_order_relaxed)) != 0)
        Aal::pause();
#endif
    }

    void release()
    {
      in_use.store(0, std::memory_order_relaxed);
    }

    /**
     * Remote deallocations for other threads
     */
    RemoteDeallocCache remote_dealloc_cache;

    constexpr LocalCache(RemoteAllocator* remote_allocator)
    : remote_allocator(remote_allocator)
    {}

    /**
     * Return all the free lists to the allocator.  Used during thread teardown.
     */
    template<size_t allocator_size, typename Config, typename DeallocFun>
    bool flush(typename Config::LocalState* local_state, DeallocFun dealloc)
    {
      auto& key = entropy.get_free_list_key();
      auto domesticate = [local_state](freelist::QueuePtr p)
                           SNMALLOC_FAST_PATH_LAMBDA {
                             return capptr_domesticate<Config>(local_state, p);
                           };

      for (size_t i = 0; i < NUM_SMALL_SIZECLASSES; i++)
      {
        // TODO could optimise this, to return the whole list in one append
        // call.
        while (!small_fast_free_lists[i].empty())
        {
          auto p = small_fast_free_lists[i].take(key, domesticate);
          SNMALLOC_ASSERT(is_start_of_object(
            sizeclass_t::from_small_class(i), address_cast(p)));
          dealloc(p.as_void());
        }
      }

      return remote_dealloc_cache.post<allocator_size, Config>(
        local_state, remote_allocator->trunc_id(), key_global);
    }

    template<
      ZeroMem zero_mem,
      typename Config,
      typename Slowpath,
      typename Domesticator>
    SNMALLOC_FAST_PATH capptr::Alloc<void>
    alloc(Domesticator domesticate, size_t size, Slowpath slowpath)
    {
      auto& key = entropy.get_free_list_key();
      smallsizeclass_t sizeclass = size_to_sizeclass(size);
      auto& fl = small_fast_free_lists[sizeclass];
      if (SNMALLOC_LIKELY(!fl.empty()))
      {
        auto p = fl.take(key, domesticate);
        release();
        return finish_alloc<zero_mem, Config>(p, sizeclass);
      }
      return slowpath(sizeclass, &fl);
    }
  };

} // namespace snmalloc
