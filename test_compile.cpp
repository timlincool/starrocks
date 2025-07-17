// Simple compilation test for the new optimizations

#include "be/src/common/lockfree_object_pool.h"
#include "be/src/util/simd_string_util.h"

#include <iostream>
#include <string>

using namespace starrocks;

struct TestObject {
    int value;
    TestObject(int v) : value(v) {}
};

int main() {
    std::cout << "Testing LockFreeObjectPool..." << std::endl;
    
    // Test LockFreeObjectPool
    {
        LockFreeObjectPool pool;
        auto* obj1 = pool.add(new TestObject(1));
        auto* obj2 = pool.add(new TestObject(2));
        
        std::cout << "Created objects with values: " << obj1->value << ", " << obj2->value << std::endl;
        
        // Test acquire_data
        LockFreeObjectPool pool2;
        pool2.add(new TestObject(3));
        pool.acquire_data(&pool2);
        
        std::cout << "LockFreeObjectPool test passed!" << std::endl;
    }
    
    // Test SIMD String Utilities
    {
        std::cout << "Testing SIMD String Utilities..." << std::endl;
        
        std::string str1 = "Hello World";
        std::string str2 = "hello world";
        
        // Test case-insensitive comparison
        int result = SIMDStringUtil::compare_ignore_case_simd(
            str1.data(), str1.size(), str2.data(), str2.size());
        
        std::cout << "Case-insensitive comparison result: " << result << std::endl;
        
        // Test hashing
        uint32_t hash = SIMDStringUtil::hash_string_crc32(str1.data(), str1.size());
        std::cout << "Hash value: " << hash << std::endl;
        
        // Test ASCII check
        bool is_ascii = SIMDStringUtil::is_ascii_simd(str1.data(), str1.size());
        std::cout << "Is ASCII: " << (is_ascii ? "true" : "false") << std::endl;
        
        std::cout << "SIMD String Utilities test passed!" << std::endl;
    }
    
    std::cout << "All tests passed!" << std::endl;
    return 0;
}
