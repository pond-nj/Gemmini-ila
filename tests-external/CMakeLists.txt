# Differential test of the Gemmini ILA against libgemmini. See README.md.
# Oldest CMake whose features this file uses.
cmake_minimum_required(VERSION 3.15)
# Project name; only C++ is compiled here (Spike builds with its own configure/make).
project(gemmini_ila_tests_external LANGUAGES CXX)

# Compile everything as C++17 and fail instead of falling back to an older standard.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Write build/compile_commands.json for clangd; INTERNAL hides it from the cache editors.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

# Let find_package see cmake/FindZ3.cmake.
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
# Z3 solves/evaluates the ILA's expressions; ilang is the ILA library the model is written in.
find_package(Z3 REQUIRED)
find_package(ilang REQUIRED)

# The ILA version under test: a directory with src/gemmini.cc and include/gemmini.h.
# A default only; run.sh passes -DGEMMINI_ILA_DIR=<dir>.
set(GEMMINI_ILA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/.. CACHE PATH "Gemmini ILA version to test")
# libgemmini sources: the reference Gemmini model the ILA is compared against. Built with its own
# gemmini_params.h, so DIM is 16, the size libgemmini and the rocc-tests are written for.
set(LIBGEMMINI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/libgemmini)
# Spike sources and the directory inside the build where Spike gets installed.
set(SPIKE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/riscv-isa-sim)
set(SPIKE_INSTALL ${CMAKE_CURRENT_BINARY_DIR}/spike)

# Spike (riscv-isa-sim) provides the processor, MMU and trap classes libgemmini runs on.
include(ExternalProject)
# Number of CPU cores, used for Spike's parallel make.
cmake_host_system_information(RESULT NPROC QUERY NUMBER_OF_LOGICAL_CORES)
# Build Spike with its own autotools build (configure, make, make install into SPIKE_INSTALL).
# It is built once; later builds of this project reuse it.
ExternalProject_Add(spike
  SOURCE_DIR ${SPIKE_DIR}
  CONFIGURE_COMMAND ${SPIKE_DIR}/configure --prefix=${SPIKE_INSTALL}
  BUILD_COMMAND make -j${NPROC}
  INSTALL_COMMAND make install
  # Tells generators such as Ninja that this step produces libriscv.so.
  BUILD_BYPRODUCTS ${SPIKE_INSTALL}/lib/libriscv.so)
# The include directory must exist at configure time, before Spike installs into it,
# or CMake rejects it as an INTERFACE_INCLUDE_DIRECTORIES entry below.
file(MAKE_DIRECTORY ${SPIKE_INSTALL}/include)
# A target standing for the installed Spike library, so other targets can link to it.
add_library(spike_riscv SHARED IMPORTED)
set_target_properties(spike_riscv PROPERTIES
  # The library file to link.
  IMPORTED_LOCATION ${SPIKE_INSTALL}/lib/libriscv.so
  # Spike's library has no soname; link it by its path.
  IMPORTED_NO_SONAME TRUE
  # Anything linking spike_riscv gets Spike's headers (<riscv/...>) on its include path.
  INTERFACE_INCLUDE_DIRECTORIES ${SPIKE_INSTALL}/include)
# Build Spike before anything that links spike_riscv.
add_dependencies(spike_riscv spike)

# The reference model: libgemmini plus golden.cc, which wraps it in the shared GemminiModel
# interface.
add_library(libgemmini STATIC ${LIBGEMMINI_DIR}/gemmini.cc src/golden.cc)
# Find gemmini.h and gemmini_params.h.
target_include_directories(libgemmini PRIVATE ${LIBGEMMINI_DIR})
# Silence warnings from the third-party libgemmini code.
target_compile_options(libgemmini PRIVATE -w)
# libgemmini runs on Spike's classes; PUBLIC passes Spike on to whatever links libgemmini.
target_link_libraries(libgemmini PUBLIC spike_riscv)

# The model under test: the chosen ILA version plus ila_sim.cc, which simulates it behind the
# same GemminiModel interface.
add_library(gemmini_ila STATIC ${GEMMINI_ILA_DIR}/src/gemmini.cc src/ila_sim.cc)
# Find the chosen version's gemmini.h.
target_include_directories(gemmini_ila PRIVATE ${GEMMINI_ILA_DIR}/include)
# The ILA is built with ilang; PUBLIC passes ilang on to whatever links gemmini_ila.
target_link_libraries(gemmini_ila PUBLIC ilang::ilang)

# The test driver: replays a trace through both models and compares their states.
add_executable(gemmini_diff src/main.cc)
target_link_libraries(gemmini_diff PRIVATE libgemmini gemmini_ila)
