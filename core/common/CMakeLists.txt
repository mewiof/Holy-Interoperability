add_library(holy_common STATIC
  boot_block.h
  log.cpp
  log.h
  paths.cpp
  paths.h
  provider.cpp
  provider.h
  strings.cpp
  strings.h
  vfs.cpp
  vfs.h
  xml.cpp
  xml.h)
holy_target_defaults(holy_common)
