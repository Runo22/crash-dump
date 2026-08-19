cmake_minimum_required(VERSION 3.20)

# Standalone build (e.g. `conan create`) needs its own project(); when pulled in
# via add_subdirectory() the parent already provides one, so skip it there.
if(NOT DEFINED PROJECT_NAME)
  project(crash_handler CXX)
endif()

# Static library. Link it from the exe only -- plugins do not need it; the
# module handler registers them on their behalf.
add_library(crash_handler STATIC
  crash_handler.cpp  crash_handler.hpp
  crash_io.cpp       crash_io.hpp
  crash_modules.cpp  crash_modules.hpp
  crash_report.cpp   crash_report.hpp
  crash_symbols.cpp  crash_symbols.hpp
)

target_include_directories(crash_handler PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_compile_features(crash_handler PUBLIC cxx_std_17)
target_link_libraries(crash_handler PUBLIC dbghelp psapi)

# Namespaced alias so consumers link the same name whether they pull this in via
# add_subdirectory() or via Conan's find_package (which exposes
# crash_handler::crash_handler). The :: form also fails loudly at configure time
# on a typo, instead of degrading to a bad -lcrash_handler link error.
add_library(crash_handler::crash_handler ALIAS crash_handler)

# Ship the .lib with embedded CodeView (/Z7) so a prebuilt crash_handler still
# resolves its own frames in the consumer's crash reports. Swap out /Zi first to
# avoid the shared-PDB dependency and a D9025 override warning. Scoped to this
# library's build only -- these variables do not propagate to a parent scope.
if(MSVC)
  foreach(cfg DEBUG RELWITHDEBINFO RELEASE MINSIZEREL)
    string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_${cfg} "${CMAKE_CXX_FLAGS_${cfg}}")
    if(NOT CMAKE_CXX_FLAGS_${cfg} MATCHES "/Z7")
      string(APPEND CMAKE_CXX_FLAGS_${cfg} " /Z7")
    endif()
  endforeach()
endif()

# Only crash_handler.hpp is meant to be included by users; the rest are
# internal and live in the same directory purely for build simplicity.
