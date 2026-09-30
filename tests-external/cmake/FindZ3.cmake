# ilangConfig asks for Z3 but does not ship a finder; this defines z3::z3.
find_path(Z3_INCLUDE_DIR NAMES z3++.h PATH_SUFFIXES z3)
find_library(Z3_LIBRARY NAMES z3)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Z3 DEFAULT_MSG Z3_INCLUDE_DIR Z3_LIBRARY)
if(Z3_FOUND AND NOT TARGET z3::z3)
  add_library(z3::z3 UNKNOWN IMPORTED)
  set_target_properties(z3::z3 PROPERTIES IMPORTED_LOCATION ${Z3_LIBRARY} INTERFACE_INCLUDE_DIRECTORIES ${Z3_INCLUDE_DIR})
endif()
