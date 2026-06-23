/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <gtest/gtest.h>

#include "common/type_cast.h"

TEST(TypeCasterTest, ParseIntegerLiteralBoundaries) {
    Value int_val = TypeCaster::parse_integer_literal("2147483647");
    EXPECT_EQ(int_val.type, TYPE_INT);
    EXPECT_EQ(int_val.as_int(), 2147483647);

    Value bigint_val = TypeCaster::parse_integer_literal("2147483648");
    EXPECT_EQ(bigint_val.type, TYPE_BIGINT);
    EXPECT_EQ(bigint_val.as_bigint(), 2147483648LL);

    Value min_bigint = TypeCaster::parse_integer_literal("-9223372036854775808");
    EXPECT_EQ(min_bigint.type, TYPE_BIGINT);
    EXPECT_EQ(min_bigint.as_bigint(), std::numeric_limits<int64_t>::min());
}

TEST(TypeCasterTest, RejectOutOfRangeIntegerLiteral) {
    EXPECT_THROW(TypeCaster::parse_integer_literal("9223372036854775808"), InvalidIntegerLiteralError);
    EXPECT_THROW(TypeCaster::parse_integer_literal("-9223372036854775809"), InvalidIntegerLiteralError);
}

TEST(TypeCasterTest, NumericCastPriority) {
    Value int_val;
    int_val.set_int(42);

    Value widened = TypeCaster::cast_value(int_val, TYPE_BIGINT);
    EXPECT_EQ(widened.type, TYPE_BIGINT);
    EXPECT_EQ(widened.as_bigint(), 42);

    Value bigint_val;
    bigint_val.set_bigint(2147483647LL);
    Value narrowed = TypeCaster::cast_value(bigint_val, TYPE_INT);
    EXPECT_EQ(narrowed.type, TYPE_INT);
    EXPECT_EQ(narrowed.as_int(), 2147483647);

    bigint_val.set_bigint(2147483648LL);
    EXPECT_THROW(TypeCaster::cast_value(bigint_val, TYPE_INT), NumericOverflowError);

    Value float_val;
    float_val.set_float(123.75f);
    Value trunc = TypeCaster::cast_value(float_val, TYPE_INT);
    EXPECT_EQ(trunc.type, TYPE_INT);
    EXPECT_EQ(trunc.as_int(), 123);
}

TEST(TypeCasterTest, DateTimeCastAndCompare) {
    Value str_val;
    str_val.set_str("2024-02-29 12:34:56");
    Value dt_val = TypeCaster::cast_value(str_val, TYPE_DATETIME);
    EXPECT_EQ(dt_val.type, TYPE_DATETIME);
    EXPECT_EQ(int64_to_datetime_str(dt_val.as_datetime()), "2024-02-29 12:34:56");

    Value invalid_str;
    invalid_str.set_str("2024-02-30 12:34:56");
    EXPECT_THROW(TypeCaster::cast_value(invalid_str, TYPE_DATETIME), InvalidDatetimeError);

    Value dt_val2;
    dt_val2.set_datetime(datetime_str_to_int64("2024-03-01 00:00:00"));
    EXPECT_LT(TypeCaster::compare_values(dt_val, dt_val2), 0);
    EXPECT_EQ(TypeCaster::compare_values(dt_val, str_val), 0);
}

TEST(TypeCasterTest, IntegerArithmeticPromotionAndOverflow) {
    Value lhs = TypeCaster::parse_integer_literal("2147483647");
    Value rhs = TypeCaster::parse_integer_literal("1");
    Value sum = TypeCaster::apply_arithmetic(lhs, rhs, '+');
    EXPECT_EQ(sum.type, TYPE_BIGINT);
    EXPECT_EQ(sum.as_bigint(), 2147483648LL);

    Value max_bigint = TypeCaster::parse_integer_literal("9223372036854775807");
    EXPECT_THROW(TypeCaster::apply_arithmetic(max_bigint, rhs, '+'), NumericOverflowError);
}
