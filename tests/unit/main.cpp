#include <cstdio>
#include <vector>

#include "tests/unit/test.h"

namespace holy::test {
namespace {

struct Case {
  const char* name;
  void (*function)();
};

std::vector<Case>& cases() {
  static std::vector<Case> all;
  return all;
}

int g_failures_in_case = 0;

}  // namespace

Registrar::Registrar(const char* name, void (*function)()) {
  cases().push_back({name, function});
}

void fail(const char* file, int line, const char* expression) {
  ++g_failures_in_case;
  std::printf("    %s:%d: expectation failed: %s\n", file, line, expression);
}

}  // namespace holy::test

int main() {
  using namespace holy::test;
  int failed = 0;
  for (const Case& test_case : cases()) {
    g_failures_in_case = 0;
    test_case.function();
    std::printf("[%s] %s\n", g_failures_in_case == 0 ? " ok " : "FAIL", test_case.name);
    if (g_failures_in_case != 0) ++failed;
  }
  std::printf("%zu tests, %d failed\n", cases().size(), failed);
  return failed == 0 ? 0 : 1;
}
