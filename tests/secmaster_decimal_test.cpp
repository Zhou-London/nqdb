#include <nqdb/secmaster/decimal.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace {

using namespace nqdb::secmaster;

TEST(SecmasterDecimal, ReadsDecimalsExactly) {
  EXPECT_EQ(nanos_from_decimal("0"), 0);
  EXPECT_EQ(nanos_from_decimal("100"), 100 * nanos_per_unit);
  EXPECT_EQ(nanos_from_decimal("0.01"), 10'000'000);
  EXPECT_EQ(nanos_from_decimal("12.5"), 12'500'000'000);
  EXPECT_EQ(nanos_from_decimal("0.000000001"), 1);
  EXPECT_EQ(nanos_from_decimal("-0.25"), -250'000'000);
  EXPECT_EQ(nanos_from_decimal("9223372036.854775807"), std::numeric_limits<std::int64_t>::max());
}

TEST(SecmasterDecimal, RejectsTextThatIsNotADecimalOfNineFractionDigits) {
  for (const char* malformed : {"", "-", ".5", "5.", "1.2.3", "1e3", "0x10", " 1", "1 ", "+1",
                                "--1", "1,5", "0.0000000001", "9223372036.854775808",
                                "-9223372036.854775808", "-.5", "-1.",
                                "99999999999999999999"}) {
    EXPECT_THROW(nanos_from_decimal(malformed), error) << "'" << malformed << "'";
  }
}

TEST(SecmasterDecimal, WritesTheShortestDecimal) {
  EXPECT_EQ(decimal_from_nanos(0), "0");
  EXPECT_EQ(decimal_from_nanos(100 * nanos_per_unit), "100");
  EXPECT_EQ(decimal_from_nanos(10'000'000), "0.01");
  EXPECT_EQ(decimal_from_nanos(12'500'000'000), "12.5");
  EXPECT_EQ(decimal_from_nanos(1), "0.000000001");
  EXPECT_EQ(decimal_from_nanos(-250'000'000), "-0.25");
  EXPECT_EQ(decimal_from_nanos(-12'500'000'000), "-12.5");
  EXPECT_EQ(decimal_from_nanos(std::numeric_limits<std::int64_t>::max()), "9223372036.854775807");
  EXPECT_EQ(decimal_from_nanos(std::numeric_limits<std::int64_t>::min()), "-9223372036.854775808");
}

}  // namespace
