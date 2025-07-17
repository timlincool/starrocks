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

#include <benchmark/benchmark.h>
#include <thread>
#include <vector>
#include <random>

#include "runtime/mem_pool.h"
#include "runtime/optimized_mem_pool.h"

namespace starrocks {

// Test allocation patterns
class AllocationPattern {
public:
    static std::vector<size_t> create_small_allocations(int count) {
        std::vector<size_t> sizes;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(8, 256);
        
        for (int i = 0; i < count; ++i) {
            sizes.push_back(dis(gen));
        }
        return sizes;
    }
    
    static std::vector<size_t> create_mixed_allocations(int count) {
        std::vector<size_t> sizes;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> small_dis(8, 256);
        std::uniform_int_distribution<> large_dis(1024, 8192);
        std::uniform_int_distribution<> choice(0, 9);
        
        for (int i = 0; i < count; ++i) {
            // 80% small allocations, 20% large allocations
            if (choice(gen) < 8) {
                sizes.push_back(small_dis(gen));
            } else {
                sizes.push_back(large_dis(gen));
            }
        }
        return sizes;
    }
    
    static std::vector<size_t> create_power_of_two_allocations(int count) {
        std::vector<size_t> sizes;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(3, 12); // 2^3 to 2^12 (8 to 4096)
        
        for (int i = 0; i < count; ++i) {
            sizes.push_back(1 << dis(gen));
        }
        return sizes;
    }
};

// Benchmark original MemPool - small allocations
static void BM_MemPool_SmallAllocations(benchmark::State& state) {
    auto sizes = AllocationPattern::create_small_allocations(state.range(0));
    
    for (auto _ : state) {
        MemPool pool;
        for (size_t size : sizes) {
            void* ptr = pool.allocate(size);
            benchmark::DoNotOptimize(ptr);
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

// Benchmark optimized MemPool - small allocations
static void BM_OptimizedMemPool_SmallAllocations(benchmark::State& state) {
    auto sizes = AllocationPattern::create_small_allocations(state.range(0));
    
    for (auto _ : state) {
        OptimizedMemPool pool;
        for (size_t size : sizes) {
            void* ptr = pool.allocate(size);
            benchmark::DoNotOptimize(ptr);
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

// Benchmark original MemPool - mixed allocations
static void BM_MemPool_MixedAllocations(benchmark::State& state) {
    auto sizes = AllocationPattern::create_mixed_allocations(state.range(0));
    
    for (auto _ : state) {
        MemPool pool;
        for (size_t size : sizes) {
            void* ptr = pool.allocate(size);
            benchmark::DoNotOptimize(ptr);
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

// Benchmark optimized MemPool - mixed allocations
static void BM_OptimizedMemPool_MixedAllocations(benchmark::State& state) {
    auto sizes = AllocationPattern::create_mixed_allocations(state.range(0));
    
    for (auto _ : state) {
        OptimizedMemPool pool;
        for (size_t size : sizes) {
            void* ptr = pool.allocate(size);
            benchmark::DoNotOptimize(ptr);
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

// Benchmark original MemPool - power of two allocations
static void BM_MemPool_PowerOfTwoAllocations(benchmark::State& state) {
    auto sizes = AllocationPattern::create_power_of_two_allocations(state.range(0));
    
    for (auto _ : state) {
        MemPool pool;
        for (size_t size : sizes) {
            void* ptr = pool.allocate(size);
            benchmark::DoNotOptimize(ptr);
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

// Benchmark optimized MemPool - power of two allocations
static void BM_OptimizedMemPool_PowerOfTwoAllocations(benchmark::State& state) {
    auto sizes = AllocationPattern::create_power_of_two_allocations(state.range(0));
    
    for (auto _ : state) {
        OptimizedMemPool pool;
        for (size_t size : sizes) {
            void* ptr = pool.allocate(size);
            benchmark::DoNotOptimize(ptr);
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

// Multi-threaded benchmarks
static void BM_MemPool_MultiThread(benchmark::State& state) {
    const int num_threads = state.range(1);
    const int allocations_per_thread = state.range(0) / num_threads;
    
    for (auto _ : state) {
        MemPool pool;
        std::vector<std::thread> threads;
        
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&pool, allocations_per_thread]() {
                auto sizes = AllocationPattern::create_small_allocations(allocations_per_thread);
                for (size_t size : sizes) {
                    void* ptr = pool.allocate(size);
                    (void)ptr; // Suppress unused variable warning
                }
            });
        }
        
        for (auto& thread : threads) {
            thread.join();
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_OptimizedMemPool_MultiThread(benchmark::State& state) {
    const int num_threads = state.range(1);
    const int allocations_per_thread = state.range(0) / num_threads;
    
    for (auto _ : state) {
        OptimizedMemPool pool;
        std::vector<std::thread> threads;
        
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&pool, allocations_per_thread]() {
                auto sizes = AllocationPattern::create_small_allocations(allocations_per_thread);
                for (size_t size : sizes) {
                    void* ptr = pool.allocate(size);
                    (void)ptr; // Suppress unused variable warning
                }
            });
        }
        
        for (auto& thread : threads) {
            thread.join();
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

// Benchmark aligned allocations
static void BM_MemPool_AlignedAllocations(benchmark::State& state) {
    const int alignment = state.range(1);
    auto sizes = AllocationPattern::create_small_allocations(state.range(0));
    
    for (auto _ : state) {
        MemPool pool;
        for (size_t size : sizes) {
            void* ptr = pool.allocate_aligned(size, alignment);
            benchmark::DoNotOptimize(ptr);
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_OptimizedMemPool_AlignedAllocations(benchmark::State& state) {
    const int alignment = state.range(1);
    auto sizes = AllocationPattern::create_small_allocations(state.range(0));
    
    for (auto _ : state) {
        OptimizedMemPool pool;
        for (size_t size : sizes) {
            void* ptr = pool.allocate_aligned(size, alignment);
            benchmark::DoNotOptimize(ptr);
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

// Benchmark clear and reuse patterns
static void BM_MemPool_ClearAndReuse(benchmark::State& state) {
    auto sizes = AllocationPattern::create_small_allocations(state.range(0));
    MemPool pool;
    
    for (auto _ : state) {
        for (size_t size : sizes) {
            void* ptr = pool.allocate(size);
            benchmark::DoNotOptimize(ptr);
        }
        pool.clear();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_OptimizedMemPool_ClearAndReuse(benchmark::State& state) {
    auto sizes = AllocationPattern::create_small_allocations(state.range(0));
    OptimizedMemPool pool;
    
    for (auto _ : state) {
        for (size_t size : sizes) {
            void* ptr = pool.allocate(size);
            benchmark::DoNotOptimize(ptr);
        }
        pool.clear();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

// Register benchmarks
BENCHMARK(BM_MemPool_SmallAllocations)->Range(100, 10000);
BENCHMARK(BM_OptimizedMemPool_SmallAllocations)->Range(100, 10000);

BENCHMARK(BM_MemPool_MixedAllocations)->Range(100, 10000);
BENCHMARK(BM_OptimizedMemPool_MixedAllocations)->Range(100, 10000);

BENCHMARK(BM_MemPool_PowerOfTwoAllocations)->Range(100, 10000);
BENCHMARK(BM_OptimizedMemPool_PowerOfTwoAllocations)->Range(100, 10000);

BENCHMARK(BM_MemPool_MultiThread)->Ranges({{1000, 10000}, {2, 8}});
BENCHMARK(BM_OptimizedMemPool_MultiThread)->Ranges({{1000, 10000}, {2, 8}});

BENCHMARK(BM_MemPool_AlignedAllocations)->Ranges({{1000, 10000}, {8, 64}});
BENCHMARK(BM_OptimizedMemPool_AlignedAllocations)->Ranges({{1000, 10000}, {8, 64}});

BENCHMARK(BM_MemPool_ClearAndReuse)->Range(100, 10000);
BENCHMARK(BM_OptimizedMemPool_ClearAndReuse)->Range(100, 10000);

} // namespace starrocks

BENCHMARK_MAIN();
