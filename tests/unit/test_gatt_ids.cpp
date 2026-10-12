// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

// GATT server, client and instance ids come from Luna clients as strings.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

// bluetoothgattprofileservice.cpp
uint16_t idToInt(const std::string input);
std::string idToString(const uint16_t input);

TEST(GattIds, ParsesValidIds)
{
	EXPECT_EQ(idToInt("1"), 1);
	EXPECT_EQ(idToInt("007"), 7);
	EXPECT_EQ(idToInt("65535"), 65535);
}

TEST(GattIds, InvalidIdsAreZero)
{
	EXPECT_EQ(idToInt(""), 0);
	EXPECT_EQ(idToInt("12a"), 0);
	EXPECT_EQ(idToInt("-1"), 0);
	EXPECT_EQ(idToInt(" 1"), 0);
}

TEST(GattIds, OutOfRangeIsRejectedNotThrownOrTruncated)
{
	// std::stoi threw std::out_of_range here, which nothing caught on the way from a Luna call.
	EXPECT_NO_THROW(idToInt("99999999999"));
	EXPECT_EQ(idToInt("99999999999"), 0);
	EXPECT_EQ(idToInt("999999999999999999999999999999"), 0);
	// And truncated anything above 65535 to 16 bits.
	EXPECT_EQ(idToInt("65536"), 0);
	EXPECT_EQ(idToInt("65537"), 0);
}

TEST(GattIds, RoundTrip)
{
	EXPECT_EQ(idToString(7), "007");
	EXPECT_EQ(idToInt(idToString(42)), 42);
}
