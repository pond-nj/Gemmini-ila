# Unit tests: prove properties of each ILA instruction with Z3.
add_executable(test_gemmini_ila
  ${GEMMINI_ILA_DIR}/src/main.cc
  ${GEMMINI_ILA_DIR}/src/gemmini.cc
  ${GEMMINI_ILA_DIR}/src/test_helpers.cc
  test_config.cc
  test_mv.cc
  test_matmul.cc)

target_include_directories(test_gemmini_ila PRIVATE ${GEMMINI_ILA_DIR}/include)
target_link_libraries(test_gemmini_ila PRIVATE ilang::ilang)
