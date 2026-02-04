cmake_minimum_required(VERSION 3.16)
project(VelocityDB VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler flags for high performance
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native -flto")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined")

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)

# VelocityDB library
add_library(velocitydb STATIC
    src/velocitydb.cpp
)

target_include_directories(velocitydb PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# Link pthread for threading
find_package(Threads REQUIRED)
target_link_libraries(velocitydb PUBLIC Threads::Threads)

# Main demo executable
add_executable(velocitydb_demo
    src/main.cpp
)

target_link_libraries(velocitydb_demo PRIVATE velocitydb)

# Install targets
install(TARGETS velocitydb velocitydb_demo
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/ DESTINATION include)

# Option to build tests
option(BUILD_TESTS "Build tests" OFF)

if(BUILD_TESTS)
    enable_testing()
    add_executable(velocitydb_tests tests/test_main.cpp)
    target_link_libraries(velocitydb_tests PRIVATE velocitydb)
    add_test(NAME velocitydb_tests COMMAND velocitydb_tests)
endif()
