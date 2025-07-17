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

#pragma once

#include <vector>
#include <memory>
#include <atomic>
#include "runtime/memory/mem_chunk.h"

namespace starrocks {

/// Optimized MemPool with improved allocation strategies:
/// 1. Thread-local allocation for small objects
/// 2. Size-class based allocation to reduce fragmentation
/// 3. Prefetching for better cache performance
/// 4. SIMD-optimized memory operations where applicable
class OptimizedMemPool {
public:
    OptimizedMemPool();
    ~OptimizedMemPool();

    /// Allocates memory with default alignment
    uint8_t* allocate(int64_t size) { 
        return allocate_aligned(size, DEFAULT_ALIGNMENT); 
    }

    /// Allocates memory with specified alignment
    uint8_t* allocate_aligned(int64_t size, int alignment);

    /// Allocates memory without throwing exceptions on failure
    uint8_t* try_allocate(int64_t size, int alignment = DEFAULT_ALIGNMENT);

    /// Returns unused memory from the last allocation
    void return_partial_allocation(int64_t byte_size);

    /// Clears all allocations but keeps chunks for reuse
    void clear();

    /// Frees all chunks and resets the pool
    void free_all();

    /// Absorb chunks from another pool
    void acquire_data(OptimizedMemPool* src, bool keep_current = true);

    /// Exchange data with another pool
    void exchange_data(OptimizedMemPool* other);

    /// Get allocation statistics
    int64_t total_allocated_bytes() const { return _total_allocated_bytes.load(); }
    int64_t total_reserved_bytes() const { return _total_reserved_bytes.load(); }
    int64_t peak_allocated_bytes() const { return _peak_allocated_bytes.load(); }

    static const int DEFAULT_ALIGNMENT = 16;

private:
    static const int INITIAL_CHUNK_SIZE = 4 * 1024;
    static const int MAX_CHUNK_SIZE = 512 * 1024;
    static const int SMALL_OBJECT_THRESHOLD = 256;
    static const int NUM_SIZE_CLASSES = 8;

    struct ChunkInfo {
        MemChunk chunk;
        std::atomic<int64_t> allocated_bytes{0};
        int size_class;
        
        explicit ChunkInfo(const MemChunk& chunk, int sc = -1);
        ChunkInfo() = default;
    };

    /// Size class for small object allocation
    struct SizeClass {
        int object_size;
        std::vector<ChunkInfo> chunks;
        int current_chunk_idx = -1;
    };

    /// Thread-local allocation buffer for very small objects
    struct ThreadLocalBuffer {
        uint8_t* current_ptr = nullptr;
        uint8_t* end_ptr = nullptr;
        static const int BUFFER_SIZE = 4096;
        alignas(64) uint8_t buffer[BUFFER_SIZE];
    };

    /// Find or allocate a chunk for the given size
    bool find_or_allocate_chunk(size_t min_size, int size_class = -1);

    /// Get size class for the given size
    int get_size_class(size_t size) const;

    /// Allocate from size class
    uint8_t* allocate_from_size_class(int size_class, size_t size, int alignment);

    /// Allocate large object (> SMALL_OBJECT_THRESHOLD)
    uint8_t* allocate_large_object(size_t size, int alignment);

    /// Get thread-local buffer
    ThreadLocalBuffer* get_thread_local_buffer();

    /// Prefetch memory for better cache performance
    void prefetch_memory(void* addr, size_t size) const;

    std::vector<ChunkInfo> _chunks;
    std::vector<SizeClass> _size_classes;
    int _current_chunk_idx = -1;
    int _next_chunk_size = INITIAL_CHUNK_SIZE;

    std::atomic<int64_t> _total_allocated_bytes{0};
    std::atomic<int64_t> _total_reserved_bytes{0};
    std::atomic<int64_t> _peak_allocated_bytes{0};

    // Thread-local storage for small object allocation
    static thread_local ThreadLocalBuffer* _tls_buffer;
};

} // namespace starrocks
