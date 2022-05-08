#pragma once

namespace snmalloc
{
  /**
   * RangeName is an integer to specify which range is being logged. Strings can be used as template parameters.
   * 
   * ParentRange is what the range is logging calls to.
   */
  template<size_t RangeName, typename ParentRange>
  class LogRange
  {
    ParentRange parent{};

  public:
    static constexpr bool Aligned = ParentRange::Aligned;

    static constexpr bool ConcurrencySafe = ParentRange::ConcurrencySafe;

    constexpr LogRange() = default;

    capptr::Chunk<void> alloc_range(size_t size)
    {
      message<1024>("Call alloc_range({}) on {}", size, RangeName);
      auto range = parent.alloc_range(size);
      message<1024>("{} = alloc_range({}) in {}", range.unsafe_ptr(), size, RangeName);
      return range;
    }

    void dealloc_range(capptr::Chunk<void> base, size_t size)
    {
      message<1024>(
        "dealloc_range({}, {}}) on {}", base.unsafe_ptr(), size, RangeName);
      parent.dealloc_range(base, size);
      message<1024>(
        "Done dealloc_range({}, {}})! on {}",
        base.unsafe_ptr(),
        size,
        RangeName);
    }
  };
} // namespace snmalloc