#pragma once

#include "flaglock.h"

namespace snmalloc
{
  template<typename... Args>
  class ThreadLocalLog
  {
    inline static constexpr size_t BufferSize{128};
    using ElemType =
      std::tuple<const char*, DefaultPal::ThreadIdentity, std::tuple<Args...>>;
    inline static constexpr size_t ElemSize = sizeof(ElemType);

    SNMALLOC_REQUIRE_CONSTINIT inline static thread_local char
      messages_raw[BufferSize * ElemSize];
    SNMALLOC_REQUIRE_CONSTINIT inline static thread_local size_t index{0};
    SNMALLOC_REQUIRE_CONSTINIT inline static thread_local size_t length{0};

    SNMALLOC_SLOW_PATH static void init_check()
    {
      static thread_local OnDestruct on_destruct{[]() {
        static FlagWord spin_lock;
        FlagLock lock(
          spin_lock); // Don't interleave different threads messages.
        if (length > BufferSize)
          length = BufferSize;
        for (size_t i = 0; i < length; i++)
        {
          if (index == 0)
            index = BufferSize;
          index--;
          ElemType* messages = reinterpret_cast<ElemType*>(messages_raw);
          std::apply(
            [messages](Args... args) {
              message_tid<1024>(
                std::get<1>(messages[index]),
                std::get<0>(messages[index]),
                args...);
            },
            std::get<2>(messages[index]));
        }
      }};
    }

  public:
    SNMALLOC_SLOW_PATH static void add(const char* msg, Args... args)
    {
      if (index == 0)
        init_check();
      new (&(messages_raw[index * ElemSize]))
        ElemType(msg, DefaultPal::get_tid(), std::make_tuple(args...));
      index = (index + 1) % BufferSize;
      length++;
    }
  };

  class MeasureTime
  {
    size_t start;
    const char* msg;
    bool running{true};

    inline static std::atomic<size_t> base{0};

  public:
    MeasureTime(const char* msg) : start(Aal::benchmark_time_start()), msg(msg)
    {
      size_t zero = 0;
      if (base == 0)
      {
        base.compare_exchange_strong(zero, start);
      }
    }

    void stop()
    {
      if (running)
      {
        auto end = Aal::benchmark_time_end();
        ThreadLocalLog<const char*, size_t, size_t, size_t>::add(
          "{}: {} \t\t\t({} -> {})",
          msg,
          end - start,
          start - base,
          end - base);
        running = false;
      }
    }

    ~MeasureTime()
    {
      stop();
    }
  };
} // namespace snmalloc
