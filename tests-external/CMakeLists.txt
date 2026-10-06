# Differential test of the Gemmini ILA against libgemmini. See README.md.

# libgemmini sources: the reference Gemmini model the ILA is compared against. Built with its own
# gemmini_params.h, so DIM is 16, the size libgemmini and the rocc-tests are written for.
set(LIBGEMMINI_DIR ${PROJECT_SOURCE_DIR}/dependencies/libgemmini)

# The reference model: libgemmini plus golden.cc, which wraps it in the shared GemminiModel
# interface.
add_library(libgemmini STATIC ${LIBGEMMINI_DIR}/gemmini.cc src/golden.cc)
target_include_directories(libgemmini PRIVATE ${LIBGEMMINI_DIR})
# Silence warnings from the third-party libgemmini code.
target_compile_options(libgemmini PRIVATE -w)
target_link_libraries(libgemmini PUBLIC spike_riscv)

# The model under test: the chosen ILA version plus ila_sim.cc, which simulates it behind the
# same GemminiModel interface.
add_library(gemmini_ila STATIC ${GEMMINI_ILA_DIR}/src/gemmini.cc src/ila_sim.cc)
target_include_directories(gemmini_ila PRIVATE ${GEMMINI_ILA_DIR}/include)
target_link_libraries(gemmini_ila PUBLIC ilang::ilang)

# The test driver: replays a trace through both models and compares their states.
add_executable(gemmini_diff src/main.cc)
target_link_libraries(gemmini_diff PRIVATE libgemmini gemmini_ila)
