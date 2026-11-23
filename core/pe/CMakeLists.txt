add_library(holy_pe STATIC
  image.cpp
  image.h
  mapped_file.cpp
  mapped_file.h)
holy_target_defaults(holy_pe)
target_link_libraries(holy_pe PUBLIC holy_common)
