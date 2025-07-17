#!/bin/bash
# Quick test of the build environment setup
# This script validates that our Docker-based build environment works correctly

set -euo pipefail

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Logging functions
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Test basic Docker functionality
test_docker() {
    log_info "Testing Docker functionality..."
    
    # Test basic Docker commands
    docker --version || {
        log_error "Docker not available"
        return 1
    }
    
    # Test if we can run a simple container
    docker run --rm ubuntu:22.04 echo "Docker test successful" || {
        log_error "Cannot run Docker containers"
        return 1
    }
    
    log_success "Docker functionality test passed"
}

# Test toolchain image
test_toolchain_image() {
    log_info "Testing toolchain image..."
    
    # Check if our toolchain image exists
    if ! docker image inspect toolchains-ubuntu:test &> /dev/null; then
        log_warning "Toolchain image not found, building it..."
        DOCKER_BUILDKIT=1 docker build --rm=true -f docker/dockerfiles/toolchains/toolchains-ubuntu.Dockerfile -t toolchains-ubuntu:test docker/dockerfiles/toolchains/ || {
            log_error "Failed to build toolchain image"
            return 1
        }
    fi
    
    # Test basic tools in the toolchain
    docker run --rm toolchains-ubuntu:test bash -c "
        echo 'Testing basic tools...'
        gcc --version | head -1
        g++ --version | head -1
        cmake --version | head -1
        java -version 2>&1 | head -1
        python3 --version
        echo 'All tools available!'
    " || {
        log_error "Toolchain image test failed"
        return 1
    }
    
    log_success "Toolchain image test passed"
}

# Test compilation of our optimizations
test_optimization_compilation() {
    log_info "Testing optimization code compilation..."
    
    # Create a temporary container to test compilation
    docker run --rm -v "$(pwd):/workspace" -w /workspace toolchains-ubuntu:test bash -c "
        echo 'Testing LockFreeObjectPool compilation...'
        g++ -std=c++17 -I. -I./be/src -c -x c++ - -o /tmp/lockfree_test.o <<'EOF'
#include \"be/src/common/lockfree_object_pool.h\"
struct TestObj { int val; TestObj(int v) : val(v) {} };
int main() {
    starrocks::LockFreeObjectPool pool;
    pool.add(new TestObj(42));
    return 0;
}
EOF
        
        echo 'Testing SIMD String Utilities compilation...'
        g++ -std=c++17 -I. -I./be/src -msse4.2 -mavx2 -c be/src/util/simd_string_util.cpp -o /tmp/simd_test.o
        
        echo 'Testing simple test compilation and execution...'
        g++ -std=c++17 -O2 -pthread simple_test.cpp -o /tmp/simple_test
        /tmp/simple_test
        
        echo 'All optimization tests passed!'
    " || {
        log_error "Optimization compilation test failed"
        return 1
    }
    
    log_success "Optimization compilation test passed"
}

# Test performance benchmark
test_performance_benchmark() {
    log_info "Testing performance benchmark..."
    
    docker run --rm -v "$(pwd):/workspace" -w /workspace toolchains-ubuntu:test bash -c "
        echo 'Compiling performance benchmark...'
        g++ -std=c++17 -O2 -pthread performance_benchmark.cpp -o /tmp/performance_benchmark
        
        echo 'Running performance benchmark...'
        /tmp/performance_benchmark | head -20
        
        echo 'Performance benchmark completed!'
    " || {
        log_error "Performance benchmark test failed"
        return 1
    }
    
    log_success "Performance benchmark test passed"
}

# Test StarRocks environment variables
test_starrocks_env() {
    log_info "Testing StarRocks environment setup..."
    
    docker run --rm -v "$(pwd):/workspace" -w /workspace -e STARROCKS_HOME=/workspace toolchains-ubuntu:test bash -c "
        source env.sh
        echo 'STARROCKS_HOME: \$STARROCKS_HOME'
        echo 'STARROCKS_THIRDPARTY: \$STARROCKS_THIRDPARTY'
        echo 'JAVA_HOME: \$JAVA_HOME'
        echo 'Python version: \$(\$PYTHON --version)'
        echo 'Environment setup successful!'
    " || {
        log_error "StarRocks environment test failed"
        return 1
    }
    
    log_success "StarRocks environment test passed"
}

# Main test execution
main() {
    echo "=============================================="
    echo "StarRocks Build Environment Test Suite"
    echo "=============================================="
    
    local failed_tests=0
    
    # Run all tests
    test_docker || ((failed_tests++))
    test_toolchain_image || ((failed_tests++))
    test_optimization_compilation || ((failed_tests++))
    test_performance_benchmark || ((failed_tests++))
    test_starrocks_env || ((failed_tests++))
    
    echo
    echo "=============================================="
    echo "Test Results Summary"
    echo "=============================================="
    
    if [[ $failed_tests -eq 0 ]]; then
        log_success "All tests passed! ✅"
        log_success "Build environment is ready for StarRocks development."
        echo
        log_info "Next steps:"
        log_info "1. Run ./setup_build_environment.sh to create the full dev environment"
        log_info "2. Run ./validate_optimizations.sh to test your optimizations"
        log_info "3. Run ./run_starrocks_tests.sh for complete validation"
    else
        log_error "$failed_tests tests failed ❌"
        log_info "Please check the error messages above and fix any issues."
        return 1
    fi
    
    return 0
}

# Run main function
main "$@"
