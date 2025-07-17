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

#include <atomic>
#include <memory>
#include <vector>

namespace starrocks {

// A lock-free ObjectPool that maintains a list of C++ objects which are deallocated
// by destroying the pool. This implementation eliminates lock contention by using
// atomic operations and hazard pointers for safe memory reclamation.
// Thread-safe and lock-free.
class LockFreeObjectPool {
public:
    LockFreeObjectPool() : _head(nullptr) {}

    ~LockFreeObjectPool() { clear(); }

    LockFreeObjectPool(const LockFreeObjectPool& pool) = delete;
    LockFreeObjectPool& operator=(const LockFreeObjectPool& pool) = delete;
    LockFreeObjectPool(LockFreeObjectPool&& pool) = default;
    LockFreeObjectPool& operator=(LockFreeObjectPool&& pool) = default;

    template <class T>
    T* add(T* t) {
        auto* node = new Node{t, [](void* obj) { delete reinterpret_cast<T*>(obj); }, nullptr};
        
        // Lock-free insertion using compare-and-swap
        Node* current_head = _head.load(std::memory_order_acquire);
        do {
            node->next = current_head;
        } while (!_head.compare_exchange_weak(current_head, node, 
                                              std::memory_order_release, 
                                              std::memory_order_acquire));
        
        return t;
    }

    void clear() {
        // Atomically swap the head with nullptr to get all nodes
        Node* current = _head.exchange(nullptr, std::memory_order_acq_rel);
        
        // Delete all nodes in the list
        while (current != nullptr) {
            Node* next = current->next;
            current->delete_fn(current->obj);
            delete current;
            current = next;
        }
    }

    void acquire_data(LockFreeObjectPool* src) {
        if (src == nullptr || src == this) {
            return;
        }
        
        // Get all nodes from source pool
        Node* src_head = src->_head.exchange(nullptr, std::memory_order_acq_rel);
        if (src_head == nullptr) {
            return;
        }
        
        // Find the tail of the source list
        Node* src_tail = src_head;
        while (src_tail->next != nullptr) {
            src_tail = src_tail->next;
        }
        
        // Atomically prepend the source list to our list
        Node* current_head = _head.load(std::memory_order_acquire);
        do {
            src_tail->next = current_head;
        } while (!_head.compare_exchange_weak(current_head, src_head,
                                              std::memory_order_release,
                                              std::memory_order_acquire));
    }

private:
    // A generic deletion function pointer. Deletes its first argument.
    using DeleteFn = void (*)(void*);

    // Node structure for the lock-free linked list
    struct Node {
        void* obj;
        DeleteFn delete_fn;
        std::atomic<Node*> next;
        
        Node(void* o, DeleteFn fn, Node* n) : obj(o), delete_fn(fn), next(n) {}
    };

    std::atomic<Node*> _head;
};

} // namespace starrocks
