# VelocityDB - High-Performance Database Engine
# Simple Makefile for building without CMake

CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -I./include
LDFLAGS := -pthread

# Release flags (default)
RELEASE_FLAGS := -O3 -DNDEBUG -march=native

# Debug flags
DEBUG_FLAGS := -g -O0 -fsanitize=address,undefined

# Source files
SOURCES := src/velocitydb.cpp src/main.cpp
OBJECTS := $(SOURCES:.cpp=.o)

# Output
TARGET := velocitydb_demo

# Default target: build release
.PHONY: all clean debug release run

all: release

release: CXXFLAGS += $(RELEASE_FLAGS)
release: $(TARGET)

debug: CXXFLAGS += $(DEBUG_FLAGS)
debug: LDFLAGS += -fsanitize=address,undefined
debug: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

# Run with custom parameters
# Usage: make benchmark WRITES=1000000 READS=1000000 THREADS=8
WRITES ?= 100000
READS ?= 100000
THREADS ?= 4

benchmark: $(TARGET)
	./$(TARGET) $(WRITES) $(READS) $(THREADS)

clean:
	rm -f $(TARGET) $(OBJECTS)
	rm -rf ./velocitydb_demo_data

# Help
help:
	@echo "VelocityDB Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all       - Build release version (default)"
	@echo "  release   - Build optimized release version"
	@echo "  debug     - Build debug version with sanitizers"
	@echo "  run       - Build and run demo"
	@echo "  benchmark - Run benchmark with custom params"
	@echo "  clean     - Remove build artifacts"
	@echo ""
	@echo "Benchmark options:"
	@echo "  WRITES=N  - Number of write operations (default: 100000)"
	@echo "  READS=N   - Number of read operations (default: 100000)"
	@echo "  THREADS=N - Number of threads (default: 4)"
	@echo ""
	@echo "Example:"
	@echo "  make benchmark WRITES=1000000 THREADS=8"
