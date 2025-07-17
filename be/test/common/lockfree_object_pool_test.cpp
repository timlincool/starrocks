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

#include "common/lockfree_object_pool.h"

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>

namespace starrocks {

class LockFreeObjectPoolTest : public ::testing::Test {
public:
    void SetUp() override {}
    void TearDown() override {}
};

// Test object for tracking destruction
class TestObject {
public:
    TestObject(int value, std::atomic<int>* counter) : _value(value), _counter(counter) {}
    
    ~TestObject() {
        if (_counter) {
            _counter->fetch_add(1);
        }
    }
    
    int value() const { return _value; }

private:
    int _value;
    std::atomic<int>* _counter;
};

TEST_F(LockFreeObjectPoolTest, BasicFunctionality) {
    std::atomic<int> destruction_count{0};
    
    {
        LockFreeObjectPool pool;
        
        // Add some objects
        auto* obj1 = pool.add(new TestObject(1, &destruction_count));
        auto* obj2 = pool.add(new TestObject(2, &destruction_count));
        auto* obj3 = pool.add(new TestObject(3, &destruction_count));
        
        EXPECT_EQ(obj1->value(), 1);
        EXPECT_EQ(obj2->value(), 2);
        EXPECT_EQ(obj3->value(), 3);
        
        EXPECT_EQ(destruction_count.load(), 0);
    }
    
    // All objects should be destroyed when pool is destroyed
    EXPECT_EQ(destruction_count.load(), 3);
}

TEST_F(LockFreeObjectPoolTest, ClearFunctionality) {
    std::atomic<int> destruction_count{0};
    
    LockFreeObjectPool pool;
    
    // Add some objects
    pool.add(new TestObject(1, &destruction_count));
    pool.add(new TestObject(2, &destruction_count));
    pool.add(new TestObject(3, &destruction_count));
    
    EXPECT_EQ(destruction_count.load(), 0);
    
    // Clear should destroy all objects
    pool.clear();
    EXPECT_EQ(destruction_count.load(), 3);
    
    // Add more objects after clear
    pool.add(new TestObject(4, &destruction_count));
    pool.add(new TestObject(5, &destruction_count));
    
    // Only the new objects should exist
    EXPECT_EQ(destruction_count.load(), 3);
}

TEST_F(LockFreeObjectPoolTest, AcquireDataFunctionality) {
    std::atomic<int> destruction_count{0};
    
    LockFreeObjectPool pool1;
    LockFreeObjectPool pool2;
    
    // Add objects to both pools
    pool1.add(new TestObject(1, &destruction_count));
    pool1.add(new TestObject(2, &destruction_count));
    
    pool2.add(new TestObject(3, &destruction_count));
    pool2.add(new TestObject(4, &destruction_count));
    
    EXPECT_EQ(destruction_count.load(), 0);
    
    // pool1 acquires data from pool2
    pool1.acquire_data(&pool2);
    
    // Clear pool2 - should not destroy any objects since they were moved
    pool2.clear();
    EXPECT_EQ(destruction_count.load(), 0);
    
    // Clear pool1 - should destroy all 4 objects
    pool1.clear();
    EXPECT_EQ(destruction_count.load(), 4);
}

TEST_F(LockFreeObjectPoolTest, MultiThreadedAccess) {
    const int num_threads = 8;
    const int objects_per_thread = 1000;
    std::atomic<int> destruction_count{0};
    
    {
        LockFreeObjectPool pool;
        std::vector<std::thread> threads;
        
        // Launch multiple threads to add objects concurrently
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&pool, &destruction_count, t, objects_per_thread]() {
                for (int i = 0; i < objects_per_thread; ++i) {
                    pool.add(new TestObject(t * objects_per_thread + i, &destruction_count));
                }
            });
        }
        
        // Wait for all threads to complete
        for (auto& thread : threads) {
            thread.join();
        }
        
        EXPECT_EQ(destruction_count.load(), 0);
    }
    
    // All objects should be destroyed
    EXPECT_EQ(destruction_count.load(), num_threads * objects_per_thread);
}

TEST_F(LockFreeObjectPoolTest, MultiThreadedAcquireData) {
    const int num_source_pools = 4;
    const int objects_per_pool = 100;
    std::atomic<int> destruction_count{0};
    
    LockFreeObjectPool main_pool;
    std::vector<std::unique_ptr<LockFreeObjectPool>> source_pools;
    
    // Create source pools with objects
    for (int p = 0; p < num_source_pools; ++p) {
        auto pool = std::make_unique<LockFreeObjectPool>();
        for (int i = 0; i < objects_per_pool; ++i) {
            pool->add(new TestObject(p * objects_per_pool + i, &destruction_count));
        }
        source_pools.push_back(std::move(pool));
    }
    
    // Concurrently acquire data from source pools
    std::vector<std::thread> threads;
    for (int p = 0; p < num_source_pools; ++p) {
        threads.emplace_back([&main_pool, &source_pools, p]() {
            main_pool.acquire_data(source_pools[p].get());
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    EXPECT_EQ(destruction_count.load(), 0);
    
    // Clear main pool should destroy all objects
    main_pool.clear();
    EXPECT_EQ(destruction_count.load(), num_source_pools * objects_per_pool);
}

} // namespace starrocks
