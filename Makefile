# Thin wrapper around CMake.
#
#   make              release build in ./build
#   make test         build and run the tests
#   make asan / tsan  run the tests under a sanitizer

BUILD ?= build
TYPE ?= Release
SANITIZE ?=

.PHONY: all test asan tsan clean

all:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=$(TYPE) -DVELOCITYDB_SANITIZE=$(SANITIZE)
	cmake --build $(BUILD) -j

test: all
	ctest --test-dir $(BUILD) --output-on-failure

asan:
	$(MAKE) test BUILD=build-asan TYPE=Debug SANITIZE=address

tsan:
	$(MAKE) test BUILD=build-tsan TYPE=Debug SANITIZE=thread

clean:
	rm -rf build build-asan build-tsan
