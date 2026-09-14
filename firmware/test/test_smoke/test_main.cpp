// Trivial smoke test for `pio test -e native -f test_smoke` (DESIGN.md
// SS5's `test/` layout). Deliberately has no dependency on transit_core or
// transit_stats - those get their own test_core/test_stats suites from the
// agents that own those libraries; this one only proves the native Unity
// toolchain itself is wired up correctly in platformio.ini.
#include <unity.h>

#include <string>

void setUp(void) {}
void tearDown(void) {}

void test_arithmetic_sanity(void) {
  TEST_ASSERT_EQUAL_INT(4, 2 + 2);
}

void test_std_string_available(void) {
  std::string s = "philly-transit-display";
  TEST_ASSERT_EQUAL_STRING("philly-transit-display", s.c_str());
  TEST_ASSERT_EQUAL_size_t(22, s.size());
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_arithmetic_sanity);
  RUN_TEST(test_std_string_available);
  return UNITY_END();
}
