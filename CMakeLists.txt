# Builds both test suites in one build directory, so the dependencies (ILAng, Spike) are compiled
# once and shared. Use run.sh to build and run a suite.
cmake_minimum_required(VERSION 3.15)
project(gemmini_ila LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

# The ILA version under test: a directory with src/ and include/ (this one, or new/).
set(GEMMINI_ILA_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE PATH "Gemmini ILA version to test")

add_subdirectory(dependencies)
add_subdirectory(tests)
add_subdirectory(tests-external)
