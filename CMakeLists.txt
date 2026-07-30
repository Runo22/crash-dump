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

# Only crash_handler.hpp is meant to be included by users; the rest are
# internal and live in the same directory purely for build simplicity.
