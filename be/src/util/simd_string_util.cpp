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

#include "util/simd_string_util.h"

#include <algorithm>
#include <cstring>
#include <cctype>

#ifdef __SSE4_2__
#include <cpuid.h>
#endif

namespace starrocks {

bool SIMDStringUtil::has_sse42_support() {
#ifdef __SSE4_2__
    static bool checked = false;
    static bool supported = false;

    if (!checked) {
        unsigned int eax, ebx, ecx, edx;
        if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
            // SSE4.2 is bit 20 in ECX
            supported = (ecx & (1 << 20)) != 0;
        }
        checked = true;
    }

    return supported;
#else
    return false;
#endif
}

bool SIMDStringUtil::has_avx2_support() {
#ifdef __AVX2__
    static bool checked = false;
    static bool supported = false;

    if (!checked) {
        unsigned int eax, ebx, ecx, edx;
        if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
            // AVX2 is bit 5 in EBX
            supported = (ebx & (1 << 5)) != 0;
        }
        checked = true;
    }

    return supported;
#else
    return false;
#endif
}

int SIMDStringUtil::compare_ignore_case_simd(const char* s1, size_t len1, const char* s2, size_t len2) {
#ifdef __SSE2__
    if (len1 != len2) {
        return len1 < len2 ? -1 : 1;
    }
    
    size_t len = len1;
    const char* end = s1 + len;
    
    // Process 16 bytes at a time
    while (s1 + 16 <= end) {
        __m128i chunk1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s1));
        __m128i chunk2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s2));
        
        // Convert to lowercase using SIMD
        __m128i lower1 = _mm_or_si128(chunk1, _mm_set1_epi8(0x20));
        __m128i lower2 = _mm_or_si128(chunk2, _mm_set1_epi8(0x20));
        
        // Check for uppercase letters and apply conversion only to them
        __m128i is_upper1 = _mm_and_si128(_mm_cmpgt_epi8(chunk1, _mm_set1_epi8('A' - 1)),
                                         _mm_cmplt_epi8(chunk1, _mm_set1_epi8('Z' + 1)));
        __m128i is_upper2 = _mm_and_si128(_mm_cmpgt_epi8(chunk2, _mm_set1_epi8('A' - 1)),
                                         _mm_cmplt_epi8(chunk2, _mm_set1_epi8('Z' + 1)));
        
        lower1 = _mm_blendv_epi8(chunk1, lower1, is_upper1);
        lower2 = _mm_blendv_epi8(chunk2, lower2, is_upper2);
        
        // Compare
        __m128i cmp = _mm_cmpeq_epi8(lower1, lower2);
        int mask = _mm_movemask_epi8(cmp);
        
        if (mask != 0xFFFF) {
            // Find first differing byte
            for (int i = 0; i < 16; ++i) {
                if ((mask & (1 << i)) == 0) {
                    char c1 = std::tolower(s1[i]);
                    char c2 = std::tolower(s2[i]);
                    return c1 < c2 ? -1 : (c1 > c2 ? 1 : 0);
                }
            }
        }
        
        s1 += 16;
        s2 += 16;
    }
    
    // Process remaining bytes
    while (s1 < end) {
        char c1 = std::tolower(*s1);
        char c2 = std::tolower(*s2);
        if (c1 != c2) {
            return c1 < c2 ? -1 : 1;
        }
        ++s1;
        ++s2;
    }
    
    return 0;
#else
    return compare_ignore_case_fallback(s1, len1, s2, len2);
#endif
}

const char* SIMDStringUtil::find_substring_simd(const char* haystack, size_t haystack_len,
                                               const char* needle, size_t needle_len) {
#ifdef __SSE4_2__
    if (needle_len == 0) return haystack;
    if (needle_len > haystack_len) return nullptr;
    if (has_sse42_support()) {
        // Use SSE4.2 string instructions for pattern matching
        const char* end = haystack + haystack_len - needle_len + 1;
        
        if (needle_len <= 16) {
            __m128i needle_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(needle));
            
            for (const char* pos = haystack; pos < end; ++pos) {
                __m128i haystack_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pos));
                
                int result = _mm_cmpestri(needle_vec, needle_len, haystack_vec,
                                         std::min(static_cast<size_t>(16), static_cast<size_t>(end - pos)),
                                         _SIDD_CMP_EQUAL_ORDERED);
                
                if (result == 0) {
                    return pos;
                }
            }
        }
    }
#endif
    return find_substring_fallback(haystack, haystack_len, needle, needle_len);
}

void SIMDStringUtil::to_lowercase_simd(const char* src, char* dst, size_t len) {
    convert_case_simd(src, dst, len, false);
}

void SIMDStringUtil::to_uppercase_simd(const char* src, char* dst, size_t len) {
    convert_case_simd(src, dst, len, true);
}

void SIMDStringUtil::convert_case_simd(const char* src, char* dst, size_t len, bool to_upper) {
#ifdef __SSE2__
    const char* end = src + len;
    
    // Process 16 bytes at a time
    while (src + 16 <= end) {
        __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
        
        if (to_upper) {
            // Convert lowercase to uppercase
            __m128i is_lower = _mm_and_si128(_mm_cmpgt_epi8(chunk, _mm_set1_epi8('a' - 1)),
                                           _mm_cmplt_epi8(chunk, _mm_set1_epi8('z' + 1)));
            __m128i upper = _mm_sub_epi8(chunk, _mm_set1_epi8(0x20));
            chunk = _mm_blendv_epi8(chunk, upper, is_lower);
        } else {
            // Convert uppercase to lowercase
            __m128i is_upper = _mm_and_si128(_mm_cmpgt_epi8(chunk, _mm_set1_epi8('A' - 1)),
                                           _mm_cmplt_epi8(chunk, _mm_set1_epi8('Z' + 1)));
            __m128i lower = _mm_add_epi8(chunk, _mm_set1_epi8(0x20));
            chunk = _mm_blendv_epi8(chunk, lower, is_upper);
        }
        
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), chunk);
        src += 16;
        dst += 16;
    }
    
    // Process remaining bytes
    while (src < end) {
        *dst = to_upper ? std::toupper(*src) : std::tolower(*src);
        ++src;
        ++dst;
    }
#else
    // Fallback implementation
    for (size_t i = 0; i < len; ++i) {
        dst[i] = to_upper ? std::toupper(src[i]) : std::tolower(src[i]);
    }
#endif
}

size_t SIMDStringUtil::count_char_simd(const char* data, size_t len, char target) {
#ifdef __SSE2__
    size_t count = 0;
    const char* end = data + len;
    __m128i target_vec = _mm_set1_epi8(target);
    
    // Process 16 bytes at a time
    while (data + 16 <= end) {
        __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data));
        __m128i cmp = _mm_cmpeq_epi8(chunk, target_vec);
        int mask = _mm_movemask_epi8(cmp);
        count += __builtin_popcount(mask);
        data += 16;
    }
    
    // Process remaining bytes
    while (data < end) {
        if (*data == target) {
            ++count;
        }
        ++data;
    }
    
    return count;
#else
    size_t count = 0;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == target) {
            ++count;
        }
    }
    return count;
#endif
}

bool SIMDStringUtil::starts_with_simd(const char* str, size_t str_len, 
                                     const char* prefix, size_t prefix_len) {
    if (prefix_len > str_len) {
        return false;
    }
    return memcmp_simd(str, prefix, prefix_len) == 0;
}

bool SIMDStringUtil::ends_with_simd(const char* str, size_t str_len,
                                   const char* suffix, size_t suffix_len) {
    if (suffix_len > str_len) {
        return false;
    }
    return memcmp_simd(str + str_len - suffix_len, suffix, suffix_len) == 0;
}

int SIMDStringUtil::memcmp_simd(const void* s1, const void* s2, size_t n) {
#ifdef __SSE2__
    const char* p1 = static_cast<const char*>(s1);
    const char* p2 = static_cast<const char*>(s2);
    const char* end = p1 + n;
    
    // Process 16 bytes at a time
    while (p1 + 16 <= end) {
        __m128i chunk1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p1));
        __m128i chunk2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p2));
        __m128i cmp = _mm_cmpeq_epi8(chunk1, chunk2);
        int mask = _mm_movemask_epi8(cmp);
        
        if (mask != 0xFFFF) {
            // Find first differing byte
            for (int i = 0; i < 16; ++i) {
                if ((mask & (1 << i)) == 0) {
                    return static_cast<unsigned char>(p1[i]) - static_cast<unsigned char>(p2[i]);
                }
            }
        }
        
        p1 += 16;
        p2 += 16;
    }
    
    // Process remaining bytes
    return std::memcmp(p1, p2, end - p1);
#else
    return std::memcmp(s1, s2, n);
#endif
}

void SIMDStringUtil::memcpy_simd(void* dst, const void* src, size_t n) {
#ifdef __SSE2__
    char* d = static_cast<char*>(dst);
    const char* s = static_cast<const char*>(src);
    const char* end = s + n;
    
    // Process 16 bytes at a time
    while (s + 16 <= end) {
        __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d), chunk);
        s += 16;
        d += 16;
    }
    
    // Process remaining bytes
    std::memcpy(d, s, end - s);
#else
    std::memcpy(dst, src, n);
#endif
}

// Fallback implementations
int SIMDStringUtil::compare_ignore_case_fallback(const char* s1, size_t len1, const char* s2, size_t len2) {
    size_t min_len = std::min(len1, len2);
    for (size_t i = 0; i < min_len; ++i) {
        char c1 = std::tolower(s1[i]);
        char c2 = std::tolower(s2[i]);
        if (c1 != c2) {
            return c1 < c2 ? -1 : 1;
        }
    }
    return len1 < len2 ? -1 : (len1 > len2 ? 1 : 0);
}

const char* SIMDStringUtil::find_substring_fallback(const char* haystack, size_t haystack_len,
                                                   const char* needle, size_t needle_len) {
    if (needle_len == 0) return haystack;
    if (needle_len > haystack_len) return nullptr;
    
    const char* end = haystack + haystack_len - needle_len + 1;
    for (const char* pos = haystack; pos < end; ++pos) {
        if (std::memcmp(pos, needle, needle_len) == 0) {
            return pos;
        }
    }
    return nullptr;
}

} // namespace starrocks
