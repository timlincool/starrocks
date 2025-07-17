// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "runtime/optimized_mem_pool.h"

#include <algorithm>
#include <cstring>
#include "runtime/memory/mem_chunk_allocator.h"
#include "util/bit_util.h"
#include "util/starrocks_metrics.h"

#ifdef __SSE2__
#include <emmintrin.h>
#endif

namespace starrocks {

thread_local OptimizedMemPool::ThreadLocalBuffer* OptimizedMemPool::_tls_buffer = nullptr;

OptimizedMemPool::ChunkInfo::ChunkInfo(const MemChunk& chunk, int sc) 
    : chunk(chunk), size_class(sc) {
    StarRocksMetrics::instance()->memory_pool_bytes_total.increment(chunk.size);
}

OptimizedMemPool::OptimizedMemPool() {
    // Initialize size classes for small objects
    _size_classes.resize(NUM_SIZE_CLASSES);
    for (int i = 0; i < NUM_SIZE_CLASSES; ++i) {
        _size_classes[i].object_size = 16 << i; // 16, 32, 64, 128, 256, 512, 1024, 2048
    }
}

OptimizedMemPool::~OptimizedMemPool() {
    free_all();
}

uint8_t* OptimizedMemPool::allocate_aligned(int64_t size, int alignment) {
    if (size <= 0) {
        return reinterpret_cast<uint8_t*>(&_chunks); // Return non-null for zero-size allocations
    }

    // For very small objects, try thread-local allocation first
    if (size <= 64 && alignment <= DEFAULT_ALIGNMENT) {
        auto* tls_buffer = get_thread_local_buffer();
        if (tls_buffer && tls_buffer->current_ptr + size <= tls_buffer->end_ptr) {
            uint8_t* result = tls_buffer->current_ptr;
            tls_buffer->current_ptr += size;
            _total_allocated_bytes.fetch_add(size, std::memory_order_relaxed);
            return result;
        }
    }

    // For small objects, use size class allocation
    if (size <= SMALL_OBJECT_THRESHOLD) {
        int size_class = get_size_class(size);
        if (size_class >= 0) {
            return allocate_from_size_class(size_class, size, alignment);
        }
    }

    // For large objects, use direct allocation
    return allocate_large_object(size, alignment);
}

uint8_t* OptimizedMemPool::try_allocate(int64_t size, int alignment) {
    try {
        return allocate_aligned(size, alignment);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

int OptimizedMemPool::get_size_class(size_t size) const {
    for (int i = 0; i < NUM_SIZE_CLASSES; ++i) {
        if (size <= _size_classes[i].object_size) {
            return i;
        }
    }
    return -1; // Too large for size classes
}

uint8_t* OptimizedMemPool::allocate_from_size_class(int size_class, size_t size, int alignment) {
    auto& sc = _size_classes[size_class];
    
    // Try to allocate from current chunk
    if (sc.current_chunk_idx >= 0 && sc.current_chunk_idx < sc.chunks.size()) {
        auto& chunk_info = sc.chunks[sc.current_chunk_idx];
        int64_t allocated = chunk_info.allocated_bytes.load(std::memory_order_relaxed);
        
        // Calculate aligned offset
        uint8_t* chunk_start = reinterpret_cast<uint8_t*>(chunk_info.chunk.data);
        uint8_t* current_pos = chunk_start + allocated;
        uint8_t* aligned_pos = reinterpret_cast<uint8_t*>(
            (reinterpret_cast<uintptr_t>(current_pos) + alignment - 1) & ~(alignment - 1));
        
        size_t aligned_size = aligned_pos - chunk_start + size;
        
        if (aligned_size <= chunk_info.chunk.size) {
            // Atomically update allocated bytes
            int64_t expected = allocated;
            while (!chunk_info.allocated_bytes.compare_exchange_weak(
                expected, aligned_size, std::memory_order_release, std::memory_order_relaxed)) {
                // Recalculate if another thread allocated in the meantime
                current_pos = chunk_start + expected;
                aligned_pos = reinterpret_cast<uint8_t*>(
                    (reinterpret_cast<uintptr_t>(current_pos) + alignment - 1) & ~(alignment - 1));
                aligned_size = aligned_pos - chunk_start + size;
                
                if (aligned_size > chunk_info.chunk.size) {
                    break; // Chunk is full
                }
            }
            
            if (aligned_size <= chunk_info.chunk.size) {
                _total_allocated_bytes.fetch_add(size, std::memory_order_relaxed);
                prefetch_memory(aligned_pos, size);
                return aligned_pos;
            }
        }
    }
    
    // Need a new chunk for this size class
    size_t chunk_size = std::max(static_cast<size_t>(sc.object_size * 64), 
                                static_cast<size_t>(INITIAL_CHUNK_SIZE));
    chunk_size = BitUtil::RoundUpToPowerOfTwo(chunk_size);
    
    MemChunk new_chunk;
    if (!MemChunkAllocator::allocate(chunk_size, &new_chunk)) {
        return nullptr;
    }
    
    sc.chunks.emplace_back(new_chunk, size_class);
    sc.current_chunk_idx = sc.chunks.size() - 1;
    
    _total_reserved_bytes.fetch_add(chunk_size, std::memory_order_relaxed);
    
    // Allocate from the new chunk
    return allocate_from_size_class(size_class, size, alignment);
}

uint8_t* OptimizedMemPool::allocate_large_object(size_t size, int alignment) {
    // For large objects, allocate a dedicated chunk
    size_t chunk_size = std::max(size + alignment, static_cast<size_t>(_next_chunk_size));
    chunk_size = BitUtil::RoundUpToPowerOfTwo(chunk_size);
    
    MemChunk new_chunk;
    if (!MemChunkAllocator::allocate(chunk_size, &new_chunk)) {
        return nullptr;
    }
    
    _chunks.emplace_back(new_chunk);
    _current_chunk_idx = _chunks.size() - 1;
    
    auto& chunk_info = _chunks[_current_chunk_idx];
    uint8_t* chunk_start = reinterpret_cast<uint8_t*>(chunk_info.chunk.data);
    uint8_t* aligned_pos = reinterpret_cast<uint8_t*>(
        (reinterpret_cast<uintptr_t>(chunk_start) + alignment - 1) & ~(alignment - 1));
    
    chunk_info.allocated_bytes.store(aligned_pos - chunk_start + size, std::memory_order_relaxed);
    
    _total_allocated_bytes.fetch_add(size, std::memory_order_relaxed);
    _total_reserved_bytes.fetch_add(chunk_size, std::memory_order_relaxed);
    
    // Update next chunk size
    _next_chunk_size = std::min(static_cast<int>(chunk_size * 2), MAX_CHUNK_SIZE);
    
    prefetch_memory(aligned_pos, size);
    return aligned_pos;
}

OptimizedMemPool::ThreadLocalBuffer* OptimizedMemPool::get_thread_local_buffer() {
    if (_tls_buffer == nullptr) {
        _tls_buffer = new ThreadLocalBuffer();
        _tls_buffer->current_ptr = _tls_buffer->buffer;
        _tls_buffer->end_ptr = _tls_buffer->buffer + ThreadLocalBuffer::BUFFER_SIZE;
    }
    return _tls_buffer;
}

void OptimizedMemPool::prefetch_memory(void* addr, size_t size) const {
#ifdef __SSE2__
    // Prefetch memory for better cache performance
    const size_t cache_line_size = 64;
    uint8_t* ptr = reinterpret_cast<uint8_t*>(addr);
    for (size_t i = 0; i < size; i += cache_line_size) {
        _mm_prefetch(reinterpret_cast<const char*>(ptr + i), _MM_HINT_T0);
    }
#endif
}

void OptimizedMemPool::clear() {
    // Reset all chunks but keep them allocated
    for (auto& chunk_info : _chunks) {
        chunk_info.allocated_bytes.store(0, std::memory_order_relaxed);
    }
    
    for (auto& sc : _size_classes) {
        for (auto& chunk_info : sc.chunks) {
            chunk_info.allocated_bytes.store(0, std::memory_order_relaxed);
        }
    }
    
    _total_allocated_bytes.store(0, std::memory_order_relaxed);
    _current_chunk_idx = -1;
    
    // Reset thread-local buffer
    if (_tls_buffer) {
        _tls_buffer->current_ptr = _tls_buffer->buffer;
    }
}

void OptimizedMemPool::free_all() {
    int64_t total_bytes_released = 0;
    
    // Free main chunks
    for (auto& chunk_info : _chunks) {
        total_bytes_released += chunk_info.chunk.size;
        MemChunkAllocator::free(chunk_info.chunk);
    }
    _chunks.clear();
    
    // Free size class chunks
    for (auto& sc : _size_classes) {
        for (auto& chunk_info : sc.chunks) {
            total_bytes_released += chunk_info.chunk.size;
            MemChunkAllocator::free(chunk_info.chunk);
        }
        sc.chunks.clear();
        sc.current_chunk_idx = -1;
    }
    
    _next_chunk_size = INITIAL_CHUNK_SIZE;
    _current_chunk_idx = -1;
    _total_allocated_bytes.store(0, std::memory_order_relaxed);
    _total_reserved_bytes.store(0, std::memory_order_relaxed);
    
    StarRocksMetrics::instance()->memory_pool_bytes_total.increment(-total_bytes_released);
    
    // Clean up thread-local buffer
    if (_tls_buffer) {
        delete _tls_buffer;
        _tls_buffer = nullptr;
    }
}

} // namespace starrocks
