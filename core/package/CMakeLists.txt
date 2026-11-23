add_library(holy_package STATIC
  manifest.cpp
  manifest.h)
holy_target_defaults(holy_package)
target_link_libraries(holy_package PUBLIC holy_common)
