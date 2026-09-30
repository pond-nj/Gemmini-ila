# Differential test of the Gemmini ILA against libgemmini. See README.md.
cmake_minimum_required(VERSION 3.15)
project(gemmini_ila_tests_external LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

# Systolic array size for both models. The rocc-tests traces need 16 (their gemmini_params.h).
set(GEMMINI_DIM 16 CACHE STRING "Gemmini DIM for libgemmini and the ILA")

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
find_package(Z3 REQUIRED)
find_package(ilang REQUIRED)

set(ILA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(LIBGEMMINI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/libgemmini)
set(SPIKE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/riscv-isa-sim)
set(SPIKE_INSTALL ${CMAKE_CURRENT_BINARY_DIR}/spike)

# Spike (riscv-isa-sim) provides the processor, MMU and trap classes libgemmini runs on.
include(ExternalProject)
cmake_host_system_information(RESULT NPROC QUERY NUMBER_OF_LOGICAL_CORES)
ExternalProject_Add(spike
  SOURCE_DIR ${SPIKE_DIR}
  CONFIGURE_COMMAND ${SPIKE_DIR}/configure --prefix=${SPIKE_INSTALL}
  BUILD_COMMAND make -j${NPROC}
  INSTALL_COMMAND make install
  BUILD_BYPRODUCTS ${SPIKE_INSTALL}/lib/libriscv.so)
file(MAKE_DIRECTORY ${SPIKE_INSTALL}/include)
add_library(spike_riscv SHARED IMPORTED)
set_target_properties(spike_riscv PROPERTIES
  IMPORTED_LOCATION ${SPIKE_INSTALL}/lib/libriscv.so
  IMPORTED_NO_SONAME TRUE
  INTERFACE_INCLUDE_DIRECTORIES ${SPIKE_INSTALL}/include)
add_dependencies(spike_riscv spike)

# gemmini.h includes "gemmini_params.h" from its own directory, so copy libgemmini next to
# a gemmini_params.h with DIM replaced.
set(LIBGEMMINI_BUILD ${CMAKE_CURRENT_BINARY_DIR}/libgemmini)
configure_file(${LIBGEMMINI_DIR}/gemmini.cc ${LIBGEMMINI_BUILD}/gemmini.cc COPYONLY)
configure_file(${LIBGEMMINI_DIR}/gemmini.h ${LIBGEMMINI_BUILD}/gemmini.h COPYONLY)
file(READ ${LIBGEMMINI_DIR}/gemmini_params.h PARAMS)
string(REGEX REPLACE "#define DIM [0-9]+" "#define DIM ${GEMMINI_DIM}" PARAMS "${PARAMS}")
file(GENERATE OUTPUT ${LIBGEMMINI_BUILD}/gemmini_params.h CONTENT "${PARAMS}")

add_library(libgemmini STATIC ${LIBGEMMINI_BUILD}/gemmini.cc src/golden.cc)
target_include_directories(libgemmini PRIVATE ${LIBGEMMINI_BUILD})
target_compile_options(libgemmini PRIVATE -w)
target_link_libraries(libgemmini PUBLIC spike_riscv)

add_library(gemmini_ila STATIC ${ILA_DIR}/src/gemmini.cc src/ila_sim.cc)
target_include_directories(gemmini_ila PRIVATE ${ILA_DIR}/include)
target_link_libraries(gemmini_ila PUBLIC ilang::ilang)

add_executable(gemmini_diff src/main.cc)
target_link_libraries(gemmini_diff PRIVATE libgemmini gemmini_ila)
