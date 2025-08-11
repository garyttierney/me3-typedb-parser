cmake_minimum_required(VERSION 3.30)
project(me3-typedb-parser)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include_directories(
        ${LLVM_INCLUDE_DIRS}
        ${CLANG_INCLUDE_DIRS}
)

add_definitions(${LLVM_DEFINITIONS})

add_executable(me3-typedb-parser main.cpp typedb_builder.cpp typedb_json.cpp)

target_link_libraries(me3-typedb-parser
        PRIVATE
        clang-cpp
        ${CLANG_LIBS}
        LLVM
)