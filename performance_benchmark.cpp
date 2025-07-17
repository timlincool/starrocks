// Performance benchmark comparing original vs optimized implementations

#include <atomic>
#include <memory>
#include <vector>
#include <iostream>
#include <thread>
#include <chrono>
#include <mutex>
#include <string>
#include <random>

// Original ObjectPool with SpinLock (simplified)
class SpinLock {
public:
    void lock() {
        while (flag_.test_and_set(std::memory_order_acquire)) {
            // Spin
        }
    }
    
    void unlock() {
        flag_.clear(std::memory_order_release);
    }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

class OriginalObjectPool {
public:
    template <class T>
    T* add(T* t) {
        std::lock_guard<SpinLock> l(_lock);
        _objects.emplace_back(Element{t, [](void* obj) { delete reinterpret_cast<T*>(obj); }});
        return t;
    }

    void clear() {
        std::lock_guard<SpinLock> l(_lock);
        for (auto i = _objects.rbegin(); i != _objects.rend(); ++i) {
            i->delete_fn(i->obj);
        }
        _objects.clear();
    }

private:
    using DeleteFn = void (*)(void*);
    
    struct Element {
        void* obj;
        DeleteFn delete_fn;
    };

    std::vector<Element> _objects;
    SpinLock _lock;
};

// Lock-free ObjectPool
class LockFreeObjectPool {
public:
    LockFreeObjectPool() : _head(nullptr) {}

    ~LockFreeObjectPool() { clear(); }

    template <class T>
    T* add(T* t) {
        auto* node = new Node{t, [](void* obj) { delete reinterpret_cast<T*>(obj); }, nullptr};
        
        Node* current_head = _head.load(std::memory_order_acquire);
        do {
            node->next = current_head;
        } while (!_head.compare_exchange_weak(current_head, node, 
                                              std::memory_order_release, 
                                              std::memory_order_acquire));
        
        return t;
    }

    void clear() {
        Node* current = _head.exchange(nullptr, std::memory_order_acq_rel);
        
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
    char data[64]; // Some data to make object non-trivial
    
    TestObject(int v) : value(v) {
        for (int i = 0; i < 63; ++i) {
            data[i] = 'a' + (v % 26);
        }
        data[63] = '\0';
    }
};

// Benchmark function template
template<typename PoolType>
double benchmark_pool(const std::string& name, int num_threads, int objects_per_thread) {
    std::cout << "Benchmarking " << name << " with " << num_threads 
              << " threads, " << objects_per_thread << " objects per thread..." << std::endl;
    
    PoolType pool;
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
    
    double time_ms = duration.count() / 1000.0;
    std::cout << name << " completed in " << time_ms << " ms" << std::endl;
    
    return time_ms;
}

// String operation benchmarks
double benchmark_string_comparison(const std::string& name, int iterations) {
    std::cout << "Benchmarking " << name << " string comparison..." << std::endl;
    
    std::vector<std::string> strings;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1, 1000);
    
    // Generate test strings
    for (int i = 0; i < 1000; ++i) {
        strings.push_back("test_string_" + std::to_string(dis(gen)));
    }
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    int matches = 0;
    for (int iter = 0; iter < iterations; ++iter) {
        for (size_t i = 0; i < strings.size(); ++i) {
            for (size_t j = i + 1; j < strings.size(); ++j) {
                // Case-insensitive comparison
                if (strings[i].size() == strings[j].size()) {
                    bool equal = true;
                    for (size_t k = 0; k < strings[i].size(); ++k) {
                        if (std::tolower(strings[i][k]) != std::tolower(strings[j][k])) {
                            equal = false;
                            break;
                        }
                    }
                    if (equal) matches++;
                }
            }
        }
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    
    double time_ms = duration.count() / 1000.0;
    std::cout << name << " completed in " << time_ms << " ms (matches: " << matches << ")" << std::endl;
    
    return time_ms;
}

int main() {
    std::cout << "=== Performance Optimization Benchmarks ===" << std::endl;
    
    // Object Pool Benchmarks
    std::cout << "\n--- Object Pool Benchmarks ---" << std::endl;
    
    // Single-threaded test
    std::cout << "\nSingle-threaded test (1 thread, 10000 objects):" << std::endl;
    double original_single = benchmark_pool<OriginalObjectPool>("OriginalObjectPool", 1, 10000);
    double lockfree_single = benchmark_pool<LockFreeObjectPool>("LockFreeObjectPool", 1, 10000);
    
    double single_improvement = ((original_single - lockfree_single) / original_single) * 100;
    std::cout << "Single-threaded improvement: " << single_improvement << "%" << std::endl;
    
    // Multi-threaded test
    std::cout << "\nMulti-threaded test (8 threads, 1000 objects per thread):" << std::endl;
    double original_multi = benchmark_pool<OriginalObjectPool>("OriginalObjectPool", 8, 1000);
    double lockfree_multi = benchmark_pool<LockFreeObjectPool>("LockFreeObjectPool", 8, 1000);
    
    double multi_improvement = ((original_multi - lockfree_multi) / original_multi) * 100;
    std::cout << "Multi-threaded improvement: " << multi_improvement << "%" << std::endl;
    
    // String operation benchmarks
    std::cout << "\n--- String Operation Benchmarks ---" << std::endl;
    double string_time = benchmark_string_comparison("Case-insensitive comparison", 10);
    
    // Summary
    std::cout << "\n=== Performance Summary ===" << std::endl;
    std::cout << "Object Pool (single-threaded): " << single_improvement << "% improvement" << std::endl;
    std::cout << "Object Pool (multi-threaded): " << multi_improvement << "% improvement" << std::endl;
    std::cout << "String operations baseline: " << string_time << " ms" << std::endl;
    
    if (multi_improvement > 0) {
        std::cout << "\n✅ Lock-free ObjectPool shows performance improvement!" << std::endl;
    } else {
        std::cout << "\n⚠️  Lock-free ObjectPool performance needs investigation" << std::endl;
    }
    
    return 0;
}
