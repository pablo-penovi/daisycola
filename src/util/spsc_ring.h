// A lock-free ring for one producer and one consumer, safe to use from a signal handler.
//
// "One producer" means one context at a time: when the firmware thread produces from both main
// and interrupt context, it must mask interrupts around the write (mcu::Critical).
#pragma once

#include <atomic>
#include <cstddef>

namespace daisycola
{
template <typename T, size_t N>
class SpscRing
{
    static_assert(N > 0 && (N & (N - 1)) == 0, "capacity must be a power of two");

  public:
    size_t Readable() const { return head_.load(std::memory_order_acquire) - tail_.load(); }
    size_t Writable() const { return N - (head_.load() - tail_.load(std::memory_order_acquire)); }

    /** Writes up to n items; returns how many fit. */
    size_t Write(const T* data, size_t n)
    {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t room = N - (head - tail_.load(std::memory_order_acquire));
        if(n > room)
            n = room;
        for(size_t i = 0; i < n; i++)
            buf_[(head + i) & (N - 1)] = data[i];
        head_.store(head + n, std::memory_order_release);
        return n;
    }

    /** Reads up to n items; returns how many there were. */
    size_t Read(T* data, size_t n)
    {
        const size_t tail  = tail_.load(std::memory_order_relaxed);
        const size_t avail = head_.load(std::memory_order_acquire) - tail;
        if(n > avail)
            n = avail;
        for(size_t i = 0; i < n; i++)
            data[i] = buf_[(tail + i) & (N - 1)];
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }

    /** Consumer side: drops everything readable. */
    void Clear() { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }

  private:
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
    T                   buf_[N];
};

} // namespace daisycola
