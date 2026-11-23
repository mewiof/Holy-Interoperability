holy_add_system_library(holy_lib_d3d12_xs
  OUTPUT_NAME d3d12_xs
  SOURCES
    command_list.cpp
    d3d12_xs.cpp
    device.cpp
    device.h
    forward.cpp
    forward.h
    queue.cpp
  LIBS d3d12 dxgi)
