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

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef __SSE4_2__
#include <nmmintrin.h>
#endif

#ifdef __AVX2__
#include <immintrin.h>
#endif

namespace starrocks {

/// SIMD-optimized string utilities for high-performance string operations
class SIMDStringUtil {
public:
    /// Fast case-insensitive string comparison using SIMD instructions
    static int compare_ignore_case_simd(const char* s1, size_t len1, const char* s2, size_t len2);

    /// Fast string search using SIMD instructions (Boyer-Moore-like algorithm)
    static const char* find_substring_simd(const char* haystack, size_t haystack_len, 
                                          const char* needle, size_t needle_len);

    /// Fast string hash computation using CRC32 instruction
    static uint32_t hash_string_crc32(const char* data, size_t len);

    /// Fast string to lowercase conversion using SIMD
    static void to_lowercase_simd(const char* src, char* dst, size_t len);

    /// Fast string to uppercase conversion using SIMD
    static void to_uppercase_simd(const char* src, char* dst, size_t len);

    /// Fast memory comparison using SIMD (optimized memcmp)
    static int memcmp_simd(const void* s1, const void* s2, size_t n);

    /// Fast memory copy using SIMD (optimized memcpy)
    static void memcpy_simd(void* dst, const void* src, size_t n);

    /// Check if string contains only ASCII characters
    static bool is_ascii_simd(const char* data, size_t len);

    /// Count occurrences of a character in string using SIMD
    static size_t count_char_simd(const char* data, size_t len, char target);

    /// Fast string prefix check using SIMD
    static bool starts_with_simd(const char* str, size_t str_len, const char* prefix, size_t prefix_len);

    /// Fast string suffix check using SIMD
    static bool ends_with_simd(const char* str, size_t str_len, const char* suffix, size_t suffix_len);

private:
    /// Helper function to convert case using SIMD
    static void convert_case_simd(const char* src, char* dst, size_t len, bool to_upper);

    /// Helper function for SIMD-based character search
    static const char* find_char_simd(const char* data, size_t len, char target);

    /// Check if CPU supports required SIMD instructions
    static bool has_sse42_support();
    static bool has_avx2_support();

    /// Fallback implementations for non-SIMD CPUs
    static int compare_ignore_case_fallback(const char* s1, size_t len1, const char* s2, size_t len2);
    static const char* find_substring_fallback(const char* haystack, size_t haystack_len,
                                              const char* needle, size_t needle_len);
};

/// Optimized string hasher using SIMD instructions
struct SIMDStringHasher {
    std::size_t operator()(const std::string& str) const {
        return SIMDStringUtil::hash_string_crc32(str.data(), str.size());
    }
};

/// Optimized case-insensitive string hasher
struct SIMDStringCaseHasher {
    std::size_t operator()(const std::string& str) const {
        // Create lowercase version and hash it
        std::string lower_str(str.size(), '\0');
        SIMDStringUtil::to_lowercase_simd(str.data(), lower_str.data(), str.size());
        return SIMDStringUtil::hash_string_crc32(lower_str.data(), lower_str.size());
    }
};

/// Optimized case-insensitive string equality
struct SIMDStringCaseEqual {
    bool operator()(const std::string& lhs, const std::string& rhs) const {
        if (lhs.size() != rhs.size()) {
            return false;
        }
        return SIMDStringUtil::compare_ignore_case_simd(lhs.data(), lhs.size(), 
                                                       rhs.data(), rhs.size()) == 0;
    }
};

/// Inline implementations for performance-critical functions
inline uint32_t SIMDStringUtil::hash_string_crc32(const char* data, size_t len) {
#ifdef __SSE4_2__
    if (has_sse42_support()) {
        uint32_t crc = 0xFFFFFFFF;
        const char* end = data + len;

        // Process 8 bytes at a time
        while (data + 8 <= end) {
            uint64_t chunk = *reinterpret_cast<const uint64_t*>(data);
            crc = _mm_crc32_u64(crc, chunk);
            data += 8;
        }

        // Process remaining bytes
        while (data < end) {
            crc = _mm_crc32_u8(crc, *data);
            ++data;
        }

        return crc ^ 0xFFFFFFFF;
    }
#endif
    // Fallback to standard hash
    return std::hash<std::string_view>{}(std::string_view(data, len));
}

inline bool SIMDStringUtil::is_ascii_simd(const char* data, size_t len) {
#ifdef __SSE2__
    const char* end = data + len;
    const __m128i* vec_data = reinterpret_cast<const __m128i*>(data);
    const __m128i* vec_end = reinterpret_cast<const __m128i*>(end - 15);

    while (vec_data < vec_end) {
        __m128i chunk = _mm_loadu_si128(vec_data);
        if (_mm_movemask_epi8(chunk) != 0) {
            return false; // Found non-ASCII character
        }
        ++vec_data;
    }

    // Check remaining bytes
    data = reinterpret_cast<const char*>(vec_data);
    while (data < end) {
        if (static_cast<unsigned char>(*data) > 127) {
            return false;
        }
        ++data;
    }
    return true;
#else
    for (size_t i = 0; i < len; ++i) {
        if (static_cast<unsigned char>(data[i]) > 127) {
            return false;
        }
    }
    return true;
#endif
}

} // namespace starrocks
