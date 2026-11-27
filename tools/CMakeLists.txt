add_executable(holy holy/main.cpp)
holy_target_defaults(holy)
target_link_libraries(holy PRIVATE holy_common holy_pe holy_package holy_analysis holy_loader holyrt)

# The launcher is only useful with the complete runtime next to it.
add_dependencies(holy holy_lib_xmem holy_lib_d3d12_xs holy_lib_xgameruntime)

# Translates every shader in a directory as the runtime does and reports what
# does not translate and why, and how fast (tools/translate_corpus/main.cpp).
add_executable(holy_translate_corpus
  translate_corpus/main.cpp
  ${PROJECT_SOURCE_DIR}/core/libs/d3d12_xs/shader_container.cpp
  ${PROJECT_SOURCE_DIR}/core/libs/d3d12_xs/shader/dxbc.cpp
  ${PROJECT_SOURCE_DIR}/core/libs/d3d12_xs/shader/geometry.cpp
  ${PROJECT_SOURCE_DIR}/core/libs/d3d12_xs/shader/rdna.cpp
  ${PROJECT_SOURCE_DIR}/core/libs/d3d12_xs/shader/translate.cpp
  ${PROJECT_SOURCE_DIR}/core/libs/inflate.cpp)
holy_target_defaults(holy_translate_corpus)
target_link_libraries(holy_translate_corpus PRIVATE d3d12)
