#pragma once

#include "../ds/mpmcstack.h"
#include "allocconfig.h"

namespace snmalloc
{
  class GuardPage
  {
#ifndef SNMALLOC_USE_SMALL_CHUNKS
    alignas(OS_PAGE_SIZE) char guards[OS_PAGE_SIZE];
#endif

  public:
    void protect()
    {
#ifndef SNMALLOC_USE_SMALL_CHUNKS
      //TODO just getting an idea on Linux.
      mprotect(&guards, OS_PAGE_SIZE, PROT_NONE);
#endif
    }

    void unprotect()
    {
#ifndef SNMALLOC_USE_SMALL_CHUNKS
      //TODO just getting an idea on Linux.
      mprotect(&guards, OS_PAGE_SIZE, PROT_READ | PROT_WRITE);
#endif
    }
  };

  enum SlabKind
  {
    Fresh = 0,
    Large,
    Medium,
    Super,
    /**
     * If the decommit policy is lazy, slabs are moved to this state when all
     * pages other than the first one have been decommitted.
     */
    Decommitted
  };

  class Baseslab
  {
  protected:
    GuardPage pre_guard;
    SlabKind kind;

  public:
    SlabKind get_kind()
    {
      return kind;
    }
  };
} // namespace snmalloc
