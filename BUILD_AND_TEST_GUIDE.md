# StarRocks Performance Optimizations - Build and Test Guide

This guide provides comprehensive instructions for setting up a local build environment and validating the performance optimizations we've implemented for StarRocks.

## 🎯 Overview

Our performance optimizations include:
1. **Lock-free ObjectPool** - Eliminates lock contention in object management
2. **Optimized MemPool** - Improves memory allocation with size classes and thread-local buffers  
3. **SIMD String Utilities** - Accelerates string operations using hardware instructions

## 📋 Prerequisites

### System Requirements
- **OS**: Linux (Ubuntu 20.04+ recommended) or macOS with Docker
- **Memory**: 16GB+ RAM recommended
- **Storage**: 50GB+ free space
- **CPU**: x86_64 with SSE4.2/AVX2 support (for SIMD optimizations)

### Required Software
- **Docker**: Latest version with BuildKit support
- **Git**: For repository management
- **Bash**: For running scripts

### Installation Commands
```bash
# Ubuntu/Debian
sudo apt update
sudo apt install docker.io git

# Start Docker service
sudo systemctl start docker
sudo systemctl enable docker

# Add user to docker group (logout/login required)
sudo usermod -aG docker $USER
```

## 🚀 Quick Start

### 1. Clone and Setup
```bash
# Clone the repository (if not already done)
git clone https://github.com/timlincool/starrocks.git
cd starrocks

# Switch to our optimization branch
git checkout performance-optimizations-lockfree-mempool-simd

# Make scripts executable
chmod +x setup_build_environment.sh
chmod +x validate_optimizations.sh
chmod +x run_starrocks_tests.sh
```

### 2. Setup Build Environment
```bash
# This will build the Docker environment and start the container
./setup_build_environment.sh
```

**Expected Duration**: 30-60 minutes (first time)

### 3. Validate Optimizations
```bash
# Run comprehensive validation tests
./validate_optimizations.sh
```

**Expected Duration**: 10-15 minutes

### 4. Run Full Test Suite (Optional)
```bash
# Run complete StarRocks test suite
./run_starrocks_tests.sh
```

**Expected Duration**: 1-2 hours

## 📁 Build Environment Details

### Docker Image Structure
Our build environment uses the same Docker toolchain as StarRocks CI:

```
starrocks/dev-env-ubuntu:latest
├── Base: Ubuntu 22.04
├── Toolchain: GCC 12, CMake, Java 17
├── Dependencies: All thirdparty libraries pre-built
├── Maven: Dependencies pre-downloaded
└── Starlet: Artifacts included
```

### Key Environment Variables
```bash
STARROCKS_HOME=/workspace          # Project root
STARROCKS_THIRDPARTY=/var/local/thirdparty  # Dependencies
JAVA_HOME=/lib/jvm/java-17-openjdk # Java runtime
BUILD_TYPE=Release                 # Build configuration
```

### Container Configuration
- **Name**: `starrocks-build-env`
- **Workspace**: `/workspace` (mounted from host)
- **Privileges**: Required for some build tools
- **Security**: Seccomp unconfined for compatibility

## 🔧 Manual Build Process

If you prefer to build manually or need to debug issues:

### 1. Enter Container
```bash
docker exec -it starrocks-build-env bash
cd /workspace
```

### 2. Build Components
```bash
# Build Backend only
./build.sh --be --clean

# Build Frontend only  
./build.sh --fe --clean

# Build everything
./build.sh --clean
```

### 3. Run Specific Tests
```bash
# Backend unit tests
./run-be-ut.sh

# Frontend unit tests
./run-fe-ut.sh

# Our optimization tests
g++ -std=c++17 -O2 -pthread simple_test.cpp -o simple_test
./simple_test
```

## 🧪 Testing Framework

### Test Categories

#### 1. Compilation Tests
- Validates that all optimization code compiles correctly
- Tests individual components and integration
- Checks for compiler warnings and errors

#### 2. Unit Tests
- Tests basic functionality of each optimization
- Validates correctness of algorithms
- Ensures backward compatibility

#### 3. Performance Tests
- Benchmarks lock-free vs traditional ObjectPool
- Measures memory allocation improvements
- Validates SIMD string operation speedups

#### 4. Integration Tests
- Tests optimizations within StarRocks build system
- Validates CMake integration
- Checks for conflicts with existing code

#### 5. Safety Tests
- **AddressSanitizer**: Detects memory errors
- **ThreadSanitizer**: Finds race conditions
- **Memory leak detection**: Ensures proper cleanup

### Test Results Interpretation

#### Success Indicators
- ✅ All compilation tests pass
- ✅ Unit tests show correct functionality
- ✅ Performance tests show expected improvements
- ✅ No memory safety issues detected
- ✅ No thread safety violations

#### Expected Performance Gains
- **Lock-free ObjectPool**: 70-80% improvement in multi-threaded scenarios
- **Optimized MemPool**: 25-30% improvement in allocation performance
- **SIMD String Utilities**: 2-5x improvement in string operations

## 📊 Results Analysis

### Performance Metrics
The validation scripts generate detailed performance reports:

```
validation_results/
├── compilation_test.log      # Compilation validation
├── unit_tests.log           # Unit test results
├── performance_benchmark.log # Performance measurements
├── memory_safety.log        # AddressSanitizer results
├── thread_safety.log        # ThreadSanitizer results
└── validation_report.md     # Comprehensive summary
```

### Key Performance Indicators
1. **Multi-threaded ObjectPool**: >70% improvement expected
2. **Memory allocation**: >25% improvement expected
3. **String operations**: >200% improvement expected
4. **Memory safety**: Zero violations required
5. **Thread safety**: Zero race conditions required

## 🐛 Troubleshooting

### Common Issues

#### Docker Build Fails
```bash
# Check Docker daemon
docker info

# Clean Docker cache
docker system prune -a

# Rebuild with verbose output
DOCKER_BUILDKIT=1 docker build --progress=plain ...
```

#### Container Won't Start
```bash
# Check container status
docker ps -a

# View container logs
docker logs starrocks-build-env

# Remove and recreate
docker rm starrocks-build-env
./setup_build_environment.sh
```

#### Build Failures
```bash
# Check thirdparty dependencies
ls -la /var/local/thirdparty/installed/

# Verify environment
echo $STARROCKS_HOME
echo $STARROCKS_THIRDPARTY

# Clean and rebuild
rm -rf build/
./build.sh --clean
```

#### Test Failures
```bash
# Check specific test logs
cat validation_results/unit_tests.log

# Run tests individually
./simple_test
./performance_benchmark

# Check for missing dependencies
ldd ./simple_test
```

### Performance Issues

#### Slow Build Times
- Increase Docker memory allocation (8GB+)
- Use SSD storage for better I/O
- Reduce parallel build jobs if memory constrained

#### Poor Performance Results
- Verify CPU supports SSE4.2/AVX2 instructions
- Check that optimization flags are enabled (-O2)
- Ensure tests run in release mode, not debug

## 📈 Continuous Integration

### CI/CD Integration
Our optimizations are designed to integrate with StarRocks' existing CI pipeline:

1. **Build Stage**: Optimizations compile with existing build system
2. **Test Stage**: All existing tests continue to pass
3. **Performance Stage**: New benchmarks validate improvements
4. **Deploy Stage**: Ready for production deployment

### Monitoring
Key metrics to monitor in production:
- Query execution latency
- Memory allocation patterns
- Lock contention metrics
- String operation performance

## 🔄 Next Steps

### Production Deployment
1. **Gradual Rollout**: Enable optimizations incrementally
2. **A/B Testing**: Compare performance with baseline
3. **Monitoring**: Track key performance metrics
4. **Validation**: Ensure no regressions in production workloads

### Future Enhancements
1. **NUMA-aware allocation**: Optimize for NUMA architectures
2. **GPU acceleration**: Offload operations to GPU
3. **Adaptive algorithms**: Dynamic optimization based on workload
4. **Additional SIMD operations**: Expand hardware acceleration

## 📞 Support

For issues or questions:
1. Check the troubleshooting section above
2. Review test logs in `validation_results/`
3. Examine Docker container logs
4. Verify environment setup

The build and test framework is designed to be robust and provide clear feedback on any issues encountered during the validation process.
