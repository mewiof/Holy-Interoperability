holy_add_system_library(holy_lib_d3d12_xs
  OUTPUT_NAME d3d12_xs
  SOURCES
    command_list.cpp
    d3d12_xs.cpp
    descriptors.cpp
    descriptors.h
    device.cpp
    device.h
    dxgi.cpp
    forward.cpp
    forward.h
    frame_pacing.cpp
    frame_pacing.h
    gpu_memory.cpp
    gpu_memory.h
    resource.cpp
    shader_container.cpp
    shader_container.h
    shader_dump.cpp
    shader_dump.h
    views.cpp
    views.h
    queue.cpp
  LIBS d3d12 dxgi holy_lib_xmem)
