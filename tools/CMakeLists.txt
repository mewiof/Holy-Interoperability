add_executable(holy holy/main.cpp)
holy_target_defaults(holy)
target_link_libraries(holy PRIVATE holy_common holy_pe holy_package holy_analysis holy_loader holyrt)

# The launcher is only useful with the complete runtime next to it.
add_dependencies(holy holy_lib_xmem holy_lib_d3d12_xs holy_lib_xgameruntime)
