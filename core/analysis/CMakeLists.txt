add_library(holy_analysis STATIC
  import_report.cpp
  import_report.h)
holy_target_defaults(holy_analysis)
target_link_libraries(holy_analysis PUBLIC holy_common holy_pe)
