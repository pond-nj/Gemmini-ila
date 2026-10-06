# Builds ilang (target ilang::ilang) from the ILAng submodule. Every Gemmini-ila build that needs
# ilang includes this directory, so they all use the same ILAng.

# Keep ILAng's find_package(glog ...) from picking up another ILAng build's glog registered in
# ~/.cmake/packages; ILAng then builds its bundled copy.
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)
add_subdirectory(ILAng)

# ILAng's flex scanner calls isatty() without including unistd.h, which current clang rejects.
target_compile_options(verilogparser PRIVATE -include unistd.h)
