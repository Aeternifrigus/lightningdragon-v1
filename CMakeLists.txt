cmake_minimum_required(VERSION 3.16)
project(VelocityDB VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(VELOCITYDB_BUILD_TESTS "Build the tests" ON)

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)

add_library(velocitydb STATIC
    src/bloom.cpp
    src/compressor.cpp
    src/db.cpp
    src/skiplist.cpp
    src/sstable.cpp
    src/transaction.cpp
    src/wal.cpp
)
target_include_directories(velocitydb
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(velocitydb PUBLIC Threads::Threads)

if(VELOCITYDB_BUILD_TESTS)
    enable_testing()
    add_executable(velocitydb_tests
        tests/main.cpp
        tests/test_bloom.cpp
        tests/test_compressor.cpp
        tests/test_db.cpp
        tests/test_skiplist.cpp
        tests/test_sstable.cpp
        tests/test_wal.cpp
    )
    target_include_directories(velocitydb_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(velocitydb_tests PRIVATE velocitydb)
    add_test(NAME velocitydb_tests COMMAND velocitydb_tests)
endif()

install(TARGETS velocitydb ARCHIVE DESTINATION lib)
install(FILES include/velocitydb.hpp DESTINATION include)
