#pragma once
#include <atomic>
#include <cstdint>

/* Outstanding Map calls of one buffer (subresource 0), shared by every alias and context.
 *
 * The count orders nothing but itself: a thread that takes over a mapped buffer from another one
 * is synchronised by the guest's own hand-off, so the read-modify-write only has to be atomic.
 * A sequentially consistent CAS (casal) also waits until the store buffer has drained; right
 * after Map and right after the guest filled the mapped memory that buffer is full of writes to
 * cold cache lines, and the two waits per Map/Unmap pair were 7% of the render thread in a
 * developed city.  The relaxed CAS keeps the exact count under concurrent deferred contexts
 * (tests/buffer_map_count_fixture.cpp) without that wait. */
struct BufferMapCount {
    std::atomic<uint32_t> count{0};
    bool add() {
        uint32_t value=count.load(std::memory_order_relaxed);
        while(value!=UINT32_MAX) {
            if(count.compare_exchange_weak(value,value+1,std::memory_order_relaxed,std::memory_order_relaxed))return true;
        }
        return false;
    }
    bool remove() {
        uint32_t value=count.load(std::memory_order_relaxed);
        while(value) {
            if(count.compare_exchange_weak(value,value-1,std::memory_order_relaxed,std::memory_order_relaxed))return true;
        }
        return false;
    }
    bool any() const { return count.load()!=0; }
};
