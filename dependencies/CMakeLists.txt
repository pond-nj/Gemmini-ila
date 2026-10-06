# Third-party code, built once and shared by every test suite:
#   ilang::ilang  ILAng, the library the ILA is written in (finds Z3 and passes it on)
#   spike_riscv   Spike, the RISC-V simulator libgemmini runs on (tests-external only)
# gemmini-rocc-tests and libgemmini are compiled by tests-external itself.

# Keep ILAng's find_package(glog ...) from picking up another ILAng build's glog registered in
# ~/.cmake/packages; ILAng then builds its bundled copy.
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)
add_subdirectory(ILAng)

# ILAng's flex scanner calls isatty() without including unistd.h, which current clang rejects.
target_compile_options(verilogparser PRIVATE -include unistd.h)

# Spike has its own autotools build (configure, make, make install into SPIKE_INSTALL).
# It is built only when a target linking spike_riscv is built.
set(SPIKE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/riscv-isa-sim)
set(SPIKE_INSTALL ${CMAKE_CURRENT_BINARY_DIR}/spike)
include(ExternalProject)
cmake_host_system_information(RESULT NPROC QUERY NUMBER_OF_LOGICAL_CORES)
ExternalProject_Add(spike
  SOURCE_DIR ${SPIKE_DIR}
  # Boost is only for Spike's debug socket server, which libgemmini doesn't use; leaving it out
  # also avoids linking a system Boost that may be broken.
  CONFIGURE_COMMAND ${SPIKE_DIR}/configure --prefix=${SPIKE_INSTALL} --without-boost
                    --without-boost-asio --without-boost-regex
  BUILD_COMMAND make -j${NPROC}
  INSTALL_COMMAND make install
  EXCLUDE_FROM_ALL TRUE
  # Tells generators such as Ninja that this step produces libriscv.so.
  BUILD_BYPRODUCTS ${SPIKE_INSTALL}/lib/libriscv.so)

# The include directory must exist at configure time, before Spike installs into it,
# or CMake rejects it as an INTERFACE_INCLUDE_DIRECTORIES entry below.
file(MAKE_DIRECTORY ${SPIKE_INSTALL}/include)
add_library(spike_riscv SHARED IMPORTED GLOBAL)
set_target_properties(spike_riscv PROPERTIES
  IMPORTED_LOCATION ${SPIKE_INSTALL}/lib/libriscv.so
  # Spike's library has no soname; link it by its path.
  IMPORTED_NO_SONAME TRUE
  INTERFACE_INCLUDE_DIRECTORIES ${SPIKE_INSTALL}/include)
add_dependencies(spike_riscv spike)
