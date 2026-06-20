#include <catch2/catch_test_macros.hpp>

TEST_CASE("Catch2 test harness is registered with CTest", "[smoke]") {
  STATIC_REQUIRE(1 + 1 == 2);
}
