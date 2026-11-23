// Import-library donor for a library that nothing provides at run time. The
// sample title imports from it to exercise the unimplemented-import path.
extern "C" __declspec(dllexport) int HolySampleMissingFunction(int value) {
  return value;
}
