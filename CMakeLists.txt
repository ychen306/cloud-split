cmake_minimum_required(VERSION 3.4.3)
project(Split CXX C)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(LLVM_BUILD_TOOLS ON)

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
add_definitions(-fno-rtti)
add_definitions(-std=c++11)

add_executable(split split.cpp)
install(TARGETS split DESTINATION ${CMAKE_INSTALL_PREFIX})

llvm_map_components_to_libnames(llvm_libs support core irreader bitwriter transformutils)

target_link_libraries(split ${llvm_libs})
