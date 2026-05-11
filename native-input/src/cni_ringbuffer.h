// cni_ringbuffer.h
// Lock-free single-producer / single-consumer ring buffer for float audio samples.
// The audio device callback is the producer; the Unity audio thread is the consumer.
// Uses acquire/release atomics — safe on ARM64 (macOS Apple Silicon + Android) and x86_64.
// Capacity must be a power of two; use cni_rb_create() which enforces this.

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>

struct CniRingBuffer
{
    float*              data;
    uint32_t            capacity;   // always a power of two
    uint32_t            mask;       // capacity - 1
    std::atomic<uint32_t> writeHead;
    std::atomic<uint32_t> readHead;
};

// Creates a ring buffer with capacity rounded up to the next power of two >= minCapacity.
// Returns nullptr on allocation failure.
inline CniRingBuffer* cni_rb_create(uint32_t minCapacity)
{
    if (minCapacity == 0) minCapacity = 1;
    uint32_t cap = 1;
    while (cap < minCapacity) cap <<= 1;

    CniRingBuffer* rb = (CniRingBuffer*)malloc(sizeof(CniRingBuffer));
    if (!rb) return nullptr;

    rb->data = (float*)calloc(cap, sizeof(float));
    if (!rb->data) { free(rb); return nullptr; }

    rb->capacity = cap;
    rb->mask     = cap - 1;
    rb->writeHead.store(0, std::memory_order_relaxed);
    rb->readHead.store(0,  std::memory_order_relaxed);
    return rb;
}

inline void cni_rb_destroy(CniRingBuffer* rb)
{
    if (!rb) return;
    free(rb->data);
    free(rb);
}

inline void cni_rb_reset(CniRingBuffer* rb)
{
    rb->writeHead.store(0, std::memory_order_relaxed);
    rb->readHead.store(0,  std::memory_order_relaxed);
}

// Returns the number of samples available for reading.
inline uint32_t cni_rb_available(const CniRingBuffer* rb)
{
    uint32_t w = rb->writeHead.load(std::memory_order_acquire);
    uint32_t r = rb->readHead.load(std::memory_order_relaxed);
    return (w - r) & rb->mask;
}

// Returns free space (samples that can be written without overwriting unread data).
inline uint32_t cni_rb_free_space(const CniRingBuffer* rb)
{
    uint32_t w = rb->writeHead.load(std::memory_order_relaxed);
    uint32_t r = rb->readHead.load(std::memory_order_acquire);
    return (rb->mask - ((w - r) & rb->mask));
}

// Producer: writes up to `count` interleaved samples from `src`.
// Returns the number of samples actually written (may be less if buffer is full).
inline uint32_t cni_rb_write(CniRingBuffer* rb, const float* src, uint32_t count)
{
    uint32_t w   = rb->writeHead.load(std::memory_order_relaxed);
    uint32_t r   = rb->readHead.load(std::memory_order_acquire);
    uint32_t free = (rb->mask - ((w - r) & rb->mask));
    if (count > free) count = free;
    if (count == 0)   return 0;

    uint32_t wIdx    = w & rb->mask;
    uint32_t toEnd   = rb->capacity - wIdx;
    uint32_t first   = (count < toEnd) ? count : toEnd;
    uint32_t second  = count - first;

    memcpy(rb->data + wIdx, src,          first  * sizeof(float));
    if (second) memcpy(rb->data,          src + first, second * sizeof(float));

    rb->writeHead.store(w + count, std::memory_order_release);
    return count;
}

// Consumer: reads up to `count` samples into `dst`.
// Unread slots are zero-filled on underrun.
// Returns the number of samples actually read from the buffer
// (dst will always have `count` valid samples; remainder is zero).
inline uint32_t cni_rb_read(CniRingBuffer* rb, float* dst, uint32_t count)
{
    uint32_t r        = rb->readHead.load(std::memory_order_relaxed);
    uint32_t avail    = cni_rb_available(rb);
    uint32_t toRead   = (count < avail) ? count : avail;
    uint32_t zeroed   = count - toRead;

    if (toRead > 0)
    {
        uint32_t rIdx   = r & rb->mask;
        uint32_t toEnd  = rb->capacity - rIdx;
        uint32_t first  = (toRead < toEnd) ? toRead : toEnd;
        uint32_t second = toRead - first;

        memcpy(dst,          rb->data + rIdx, first  * sizeof(float));
        if (second) memcpy(dst + first, rb->data,    second * sizeof(float));

        rb->readHead.store(r + toRead, std::memory_order_release);
    }

    // Zero-fill any frames we couldn't supply (underrun — silent gap, not stale data).
    if (zeroed > 0)
        memset(dst + toRead, 0, zeroed * sizeof(float));

    return toRead;
}
