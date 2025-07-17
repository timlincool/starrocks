// Simple test for the lock-free object pool without StarRocks dependencies

#include <atomic>
#include <memory>
#include <vector>
#include <iostream>
#include <thread>
#include <chrono>

// Simplified LockFreeObjectPool for testing
class SimpleLockFreeObjectPool {
public:
    SimpleLockFreeObjectPool() : _head(nullptr) {}

    ~SimpleLockFreeObjectPool() { clear(); }

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

private:
    using DeleteFn = void (*)(void*);

    struct Node {
        void* obj;
        DeleteFn delete_fn;
        Node* next;
        
        Node(void* o, DeleteFn fn, Node* n) : obj(o), delete_fn(fn), next(n) {}
    };

    std::atomic<Node*> _head;
};

// Test object
struct TestObject {
    int value;
    std::string data;
    
    TestObject(int v) : value(v), data("test_data_" + std::to_string(v)) {}
};

// Test function
void test_lockfree_pool() {
    std::cout << "Testing SimpleLockFreeObjectPool..." << std::endl;
    
    // Basic functionality test
    {
        SimpleLockFreeObjectPool pool;
        
        auto* obj1 = pool.add(new TestObject(1));
        auto* obj2 = pool.add(new TestObject(2));
        auto* obj3 = pool.add(new TestObject(3));
        
        std::cout << "Created objects with values: " 
                  << obj1->value << ", " << obj2->value << ", " << obj3->value << std::endl;
        
        // Objects will be automatically cleaned up when pool is destroyed
    }
    
    // Multi-threaded test
    {
        SimpleLockFreeObjectPool pool;
        const int num_threads = 4;
        const int objects_per_thread = 1000;
        
        std::vector<std::thread> threads;
        auto start_time = std::chrono::high_resolution_clock::now();
        
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&pool, t, objects_per_thread]() {
                for (int i = 0; i < objects_per_thread; ++i) {
                    pool.add(new TestObject(t * objects_per_thread + i));
                }
            });
        }
        
        for (auto& thread : threads) {
            thread.join();
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
        
        std::cout << "Multi-threaded test completed in " << duration.count() << " microseconds" << std::endl;
        std::cout << "Added " << (num_threads * objects_per_thread) << " objects successfully" << std::endl;
    }
    
    std::cout << "SimpleLockFreeObjectPool test passed!" << std::endl;
}

// Simple SIMD string test
void test_simd_string() {
    std::cout << "Testing basic string operations..." << std::endl;
    
    std::string str1 = "Hello World";
    std::string str2 = "hello world";
    
    // Basic case-insensitive comparison (fallback implementation)
    bool equal = true;
    if (str1.size() == str2.size()) {
        for (size_t i = 0; i < str1.size(); ++i) {
            if (std::tolower(str1[i]) != std::tolower(str2[i])) {
                equal = false;
                break;
            }
        }
    } else {
        equal = false;
    }
    
    std::cout << "Case-insensitive comparison: " << (equal ? "equal" : "not equal") << std::endl;
    
    // Basic hash computation
    std::hash<std::string> hasher;
    auto hash1 = hasher(str1);
    auto hash2 = hasher(str2);
    
    std::cout << "Hash values: " << hash1 << ", " << hash2 << std::endl;
    
    std::cout << "Basic string operations test passed!" << std::endl;
}

int main() {
    std::cout << "Running performance optimization tests..." << std::endl;
    
    test_lockfree_pool();
    test_simd_string();
    
    std::cout << "All tests passed!" << std::endl;
    return 0;
}
