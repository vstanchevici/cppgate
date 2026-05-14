#pragma once

#include <atomic>
#include <bit>
#include <cstdint>
#include <thread>

inline void cpu_relax()
{
    #if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        // x86/x64: pause instruction tells the CPU we are in a spin-loop
        #if defined(_MSC_VER)
            _mm_pause(); // MSVC intrinsic
        #else
            asm volatile("pause" ::: "memory"); // GCC/Clang
        #endif
    #elif defined(__arm__) || defined(__aarch64__)
        // ARM: YIELD instruction hints that the thread is not doing useful work
        #if defined(_MSC_VER)
            __yield();
        #else
            asm volatile("yield" ::: "memory");
        #endif
    #else
        // Fallback for other architectures
        std::this_thread::yield();
    #endif
}

//https://david.alvarezrosa.com/posts/optimizing-a-lock-free-ring-buffer/#lock-free-ring-buffer
template<typename T>
class RingBuffer {
    static_assert(std::is_trivially_copyable_v<T>, "T must be POD/Trivial for atomic_ref"); //Data Alignment (8 bytes)

    struct Slot {
        T data;
        std::atomic<uint64_t> sequence;
    };

    size_t Size;
    size_t Mask;
    Slot* buffer;

    //Cache Line Alignment (64 bytes)
    alignas(64) std::atomic<uint64_t> head{0};
    alignas(64) std::atomic<uint64_t> tail{0};

public:
    explicit RingBuffer(size_t req_size)
            : Size(std::bit_ceil(req_size)), Mask(Size - 1) {
        buffer = new Slot[Size];
        for (uint64_t i = 0; i < Size; ++i) {
            buffer[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    RingBuffer() = default;

    ~RingBuffer() { if (buffer) { delete[] buffer;  buffer= nullptr; } }

    void create(size_t req_size)
    {
        Size = std::bit_ceil(req_size);
        Mask = (Size - 1);

        buffer = new Slot[Size];
        for (uint64_t i = 0; i < Size; ++i) {
            buffer[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    bool empty() const {
        return head.load(std::memory_order_acquire) == tail.load(std::memory_order_acquire);
    }

    bool push(const T& item) {
        // Claim the next head position
        uint64_t pos = head.fetch_add(1, std::memory_order_relaxed);
        Slot& slot = buffer[pos & Mask];

        // Wait until sequence == pos (Producer's turn)
        while (slot.sequence.load(std::memory_order_acquire) != pos) {
            cpu_relax();
        }

        //std::atomic_ref<T>(slot.data).store(item, std::memory_order_relaxed);
        slot.data = item;

        // Signal Consumer: "It's now your turn (pos + 1)"
        slot.sequence.store(pos + 1, std::memory_order_release);

        return true;
    }

    bool pop(T& out) {
        uint64_t pos = tail.load(std::memory_order_relaxed);
        Slot& slot = buffer[pos & Mask];

        // If sequence == pos + 1, data is ready for us
        if (slot.sequence.load(std::memory_order_acquire) == pos + 1) {
            if (tail.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                //out = std::atomic_ref<T>(slot.data).load(std::memory_order_relaxed);
                out = slot.data;

                // Signal Producer: "Slot is free for the next LAP (pos + Size)"
                slot.sequence.store(pos + Size, std::memory_order_release);
                return true;
            }
        }
        return false;
    }
};