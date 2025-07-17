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

#include "common/object_pool.h"
#include "common/lockfree_object_pool.h"

namespace starrocks {

// Test object for benchmarking
struct TestObject {
    int value;
    std::string data;
    
    TestObject(int v) : value(v), data("test_data_" + std::to_string(v)) {}
};

// Benchmark original ObjectPool with single thread
static void BM_ObjectPool_SingleThread(benchmark::State& state) {
    for (auto _ : state) {
        ObjectPool pool;
        for (int i = 0; i < state.range(0); ++i) {
            pool.add(new TestObject(i));
        }
        // pool destructor will clean up
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

// Benchmark lock-free ObjectPool with single thread
static void BM_LockFreeObjectPool_SingleThread(benchmark::State& state) {
    for (auto _ : state) {
        LockFreeObjectPool pool;
        for (int i = 0; i < state.range(0); ++i) {
            pool.add(new TestObject(i));
        }
        // pool destructor will clean up
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

// Benchmark original ObjectPool with multiple threads
static void BM_ObjectPool_MultiThread(benchmark::State& state) {
    const int num_threads = state.range(1);
    const int objects_per_thread = state.range(0) / num_threads;
    
    for (auto _ : state) {
        ObjectPool pool;
        std::vector<std::thread> threads;
        
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&pool, objects_per_thread, t]() {
                for (int i = 0; i < objects_per_thread; ++i) {
                    pool.add(new TestObject(t * objects_per_thread + i));
                }
            });
        }
        
        for (auto& thread : threads) {
            thread.join();
        }
        // pool destructor will clean up
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

// Benchmark lock-free ObjectPool with multiple threads
static void BM_LockFreeObjectPool_MultiThread(benchmark::State& state) {
    const int num_threads = state.range(1);
    const int objects_per_thread = state.range(0) / num_threads;
    
    for (auto _ : state) {
        LockFreeObjectPool pool;
        std::vector<std::thread> threads;
        
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&pool, objects_per_thread, t]() {
                for (int i = 0; i < objects_per_thread; ++i) {
                    pool.add(new TestObject(t * objects_per_thread + i));
                }
            });
        }
        
        for (auto& thread : threads) {
            thread.join();
        }
        // pool destructor will clean up
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

// Register benchmarks
BENCHMARK(BM_ObjectPool_SingleThread)->Range(100, 10000);
BENCHMARK(BM_LockFreeObjectPool_SingleThread)->Range(100, 10000);

BENCHMARK(BM_ObjectPool_MultiThread)->Ranges({{1000, 10000}, {2, 16}});
BENCHMARK(BM_LockFreeObjectPool_MultiThread)->Ranges({{1000, 10000}, {2, 16}});

} // namespace starrocks

BENCHMARK_MAIN();
