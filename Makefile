# Thin wrapper around CMake.
#
#   make              release build in ./build
#   make test         build and run the tests
#   make run          run the demo
#   make bench WRITES=1000000 READS=1000000 THREADS=4
#   make asan / tsan  run the tests under a sanitizer

BUILD ?= build
TYPE ?= Release
SANITIZE ?=

WRITES ?= 100000
READS ?= 100000
THREADS ?= 4

.PHONY: all test run bench asan tsan clean

all:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=$(TYPE) -DVELOCITYDB_SANITIZE=$(SANITIZE)
	cmake --build $(BUILD) -j

test: all
	ctest --test-dir $(BUILD) --output-on-failure

run: all
	./$(BUILD)/velocitydb_demo

bench: all
	./$(BUILD)/velocitydb_demo $(WRITES) $(READS) $(THREADS)

asan:
	$(MAKE) test BUILD=build-asan TYPE=Debug SANITIZE=address

tsan:
	$(MAKE) test BUILD=build-tsan TYPE=Debug SANITIZE=thread

clean:
	rm -rf build build-asan build-tsan velocitydb_demo_data
