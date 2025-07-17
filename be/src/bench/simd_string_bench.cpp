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
#include <algorithm>
#include <cctype>
#include <cstring>
#include <random>
#include <vector>

#include "bench/bench_util.h"
#include "util/simd_string_util.h"

namespace starrocks {

class SIMDStringBench {
public:
    SIMDStringBench() {
        // Generate test data
        std::random_device rd;
        std::mt19937 gen(rd());
        
        // Create strings of various lengths
        for (int len : {8, 16, 32, 64, 128, 256, 512, 1024}) {
            auto strings = BenchUtil::create_random_string(1000, len, len);
            test_strings_[len] = std::move(strings);
        }
        
        // Create mixed case strings for comparison tests
        mixed_case_strings_ = BenchUtil::create_random_string(1000, 32, 64);
        for (auto& str : mixed_case_strings_) {
            // Randomly uppercase some characters
            std::uniform_int_distribution<> dis(0, 1);
            for (char& c : str) {
                if (dis(gen)) {
                    c = std::toupper(c);
                }
            }
        }
        
        // Create search patterns
        search_patterns_ = {"SELECT", "FROM", "WHERE", "GROUP", "ORDER", "HAVING", "LIMIT"};
        
        // Create haystack for substring search
        haystack_ = "SELECT col1, col2 FROM table1 WHERE col1 > 100 GROUP BY col2 ORDER BY col1 LIMIT 1000";
    }
    
    std::unordered_map<int, std::vector<std::string>> test_strings_;
    std::vector<std::string> mixed_case_strings_;
    std::vector<std::string> search_patterns_;
    std::string haystack_;
};

static SIMDStringBench bench_data;

// Benchmark case-insensitive comparison - SIMD vs standard
static void BM_CaseInsensitiveCompare_SIMD(benchmark::State& state) {
    const auto& strings = bench_data.mixed_case_strings_;
    size_t idx = 0;
    
    for (auto _ : state) {
        const auto& str1 = strings[idx % strings.size()];
        const auto& str2 = strings[(idx + 1) % strings.size()];
        
        int result = SIMDStringUtil::compare_ignore_case_simd(
            str1.data(), str1.size(), str2.data(), str2.size());
        
        benchmark::DoNotOptimize(result);
        idx++;
    }
    state.SetItemsProcessed(state.iterations());
}

static void BM_CaseInsensitiveCompare_Standard(benchmark::State& state) {
    const auto& strings = bench_data.mixed_case_strings_;
    size_t idx = 0;
    
    for (auto _ : state) {
        const auto& str1 = strings[idx % strings.size()];
        const auto& str2 = strings[(idx + 1) % strings.size()];
        
        // Standard implementation
        int result = 0;
        if (str1.size() != str2.size()) {
            result = str1.size() < str2.size() ? -1 : 1;
        } else {
            for (size_t i = 0; i < str1.size(); ++i) {
                char c1 = std::tolower(str1[i]);
                char c2 = std::tolower(str2[i]);
                if (c1 != c2) {
                    result = c1 < c2 ? -1 : 1;
                    break;
                }
            }
        }
        
        benchmark::DoNotOptimize(result);
        idx++;
    }
    state.SetItemsProcessed(state.iterations());
}

// Benchmark string hashing - CRC32 vs standard
static void BM_StringHash_CRC32(benchmark::State& state) {
    const auto& strings = bench_data.test_strings_[state.range(0)];
    size_t idx = 0;
    
    for (auto _ : state) {
        const auto& str = strings[idx % strings.size()];
        uint32_t hash = SIMDStringUtil::hash_string_crc32(str.data(), str.size());
        benchmark::DoNotOptimize(hash);
        idx++;
    }
    state.SetItemsProcessed(state.iterations());
}

static void BM_StringHash_Standard(benchmark::State& state) {
    const auto& strings = bench_data.test_strings_[state.range(0)];
    size_t idx = 0;
    std::hash<std::string> hasher;
    
    for (auto _ : state) {
        const auto& str = strings[idx % strings.size()];
        auto hash = hasher(str);
        benchmark::DoNotOptimize(hash);
        idx++;
    }
    state.SetItemsProcessed(state.iterations());
}

// Benchmark substring search - SIMD vs standard
static void BM_SubstringSearch_SIMD(benchmark::State& state) {
    const auto& patterns = bench_data.search_patterns_;
    const auto& haystack = bench_data.haystack_;
    size_t idx = 0;
    
    for (auto _ : state) {
        const auto& pattern = patterns[idx % patterns.size()];
        const char* result = SIMDStringUtil::find_substring_simd(
            haystack.data(), haystack.size(), pattern.data(), pattern.size());
        benchmark::DoNotOptimize(result);
        idx++;
    }
    state.SetItemsProcessed(state.iterations());
}

static void BM_SubstringSearch_Standard(benchmark::State& state) {
    const auto& patterns = bench_data.search_patterns_;
    const auto& haystack = bench_data.haystack_;
    size_t idx = 0;
    
    for (auto _ : state) {
        const auto& pattern = patterns[idx % patterns.size()];
        auto pos = haystack.find(pattern);
        const char* result = (pos != std::string::npos) ? haystack.data() + pos : nullptr;
        benchmark::DoNotOptimize(result);
        idx++;
    }
    state.SetItemsProcessed(state.iterations());
}

// Benchmark case conversion - SIMD vs standard
static void BM_ToLowercase_SIMD(benchmark::State& state) {
    const auto& strings = bench_data.test_strings_[state.range(0)];
    std::vector<char> buffer(state.range(0) + 1);
    size_t idx = 0;
    
    for (auto _ : state) {
        const auto& str = strings[idx % strings.size()];
        SIMDStringUtil::to_lowercase_simd(str.data(), buffer.data(), str.size());
        benchmark::DoNotOptimize(buffer.data());
        idx++;
    }
    state.SetItemsProcessed(state.iterations());
}

static void BM_ToLowercase_Standard(benchmark::State& state) {
    const auto& strings = bench_data.test_strings_[state.range(0)];
    std::vector<char> buffer(state.range(0) + 1);
    size_t idx = 0;
    
    for (auto _ : state) {
        const auto& str = strings[idx % strings.size()];
        for (size_t i = 0; i < str.size(); ++i) {
            buffer[i] = std::tolower(str[i]);
        }
        benchmark::DoNotOptimize(buffer.data());
        idx++;
    }
    state.SetItemsProcessed(state.iterations());
}

// Benchmark character counting - SIMD vs standard
static void BM_CharCount_SIMD(benchmark::State& state) {
    const auto& strings = bench_data.test_strings_[state.range(0)];
    size_t idx = 0;
    char target = 'a';
    
    for (auto _ : state) {
        const auto& str = strings[idx % strings.size()];
        size_t count = SIMDStringUtil::count_char_simd(str.data(), str.size(), target);
        benchmark::DoNotOptimize(count);
        idx++;
    }
    state.SetItemsProcessed(state.iterations());
}

static void BM_CharCount_Standard(benchmark::State& state) {
    const auto& strings = bench_data.test_strings_[state.range(0)];
    size_t idx = 0;
    char target = 'a';
    
    for (auto _ : state) {
        const auto& str = strings[idx % strings.size()];
        size_t count = std::count(str.begin(), str.end(), target);
        benchmark::DoNotOptimize(count);
        idx++;
    }
    state.SetItemsProcessed(state.iterations());
}

// Benchmark ASCII validation - SIMD vs standard
static void BM_IsASCII_SIMD(benchmark::State& state) {
    const auto& strings = bench_data.test_strings_[state.range(0)];
    size_t idx = 0;
    
    for (auto _ : state) {
        const auto& str = strings[idx % strings.size()];
        bool is_ascii = SIMDStringUtil::is_ascii_simd(str.data(), str.size());
        benchmark::DoNotOptimize(is_ascii);
        idx++;
    }
    state.SetItemsProcessed(state.iterations());
}

static void BM_IsASCII_Standard(benchmark::State& state) {
    const auto& strings = bench_data.test_strings_[state.range(0)];
    size_t idx = 0;
    
    for (auto _ : state) {
        const auto& str = strings[idx % strings.size()];
        bool is_ascii = std::all_of(str.begin(), str.end(), 
                                   [](unsigned char c) { return c <= 127; });
        benchmark::DoNotOptimize(is_ascii);
        idx++;
    }
    state.SetItemsProcessed(state.iterations());
}

// Register benchmarks with different string lengths
BENCHMARK(BM_CaseInsensitiveCompare_SIMD);
BENCHMARK(BM_CaseInsensitiveCompare_Standard);

BENCHMARK(BM_StringHash_CRC32)->Arg(32)->Arg(64)->Arg(128)->Arg(256);
BENCHMARK(BM_StringHash_Standard)->Arg(32)->Arg(64)->Arg(128)->Arg(256);

BENCHMARK(BM_SubstringSearch_SIMD);
BENCHMARK(BM_SubstringSearch_Standard);

BENCHMARK(BM_ToLowercase_SIMD)->Arg(32)->Arg(64)->Arg(128)->Arg(256);
BENCHMARK(BM_ToLowercase_Standard)->Arg(32)->Arg(64)->Arg(128)->Arg(256);

BENCHMARK(BM_CharCount_SIMD)->Arg(32)->Arg(64)->Arg(128)->Arg(256);
BENCHMARK(BM_CharCount_Standard)->Arg(32)->Arg(64)->Arg(128)->Arg(256);

BENCHMARK(BM_IsASCII_SIMD)->Arg(32)->Arg(64)->Arg(128)->Arg(256);
BENCHMARK(BM_IsASCII_Standard)->Arg(32)->Arg(64)->Arg(128)->Arg(256);

} // namespace starrocks

BENCHMARK_MAIN();
