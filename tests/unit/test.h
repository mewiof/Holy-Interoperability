// Minimal test harness: no dependencies, one registration macro, one assertion.
#pragma once

namespace holy::test {

struct Registrar {
  Registrar(const char* name, void (*function)());
};

void fail(const char* file, int line, const char* expression);

}  // namespace holy::test

#define TEST(name)                                                                      \
  static void holy_test_##name();                                                       \
  static const ::holy::test::Registrar holy_registrar_##name(#name, &holy_test_##name); \
  static void holy_test_##name()

#define EXPECT(condition)                                                 \
  do {                                                                    \
    if (!(condition)) ::holy::test::fail(__FILE__, __LINE__, #condition); \
  } while (0)
