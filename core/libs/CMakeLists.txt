# Clean-room reimplementations of console system libraries.
#
# One directory per library, named after the DLL titles import. Nothing here is
# derived from console SDK headers or binaries: interfaces are declared from
# public documentation and from what titles observably import and call.
add_subdirectory(xmem)
add_subdirectory(d3d12_xs)
add_subdirectory(xgameruntime)
add_subdirectory(gameinput)
add_subdirectory(xcrdapi)
add_subdirectory(xg_xs)
add_subdirectory(acphal)
add_subdirectory(xapu)
add_subdirectory(chad)
