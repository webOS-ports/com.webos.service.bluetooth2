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

// What an ANCS notification provider sends, cut into blocks the way it may cut them. Under AddressSanitizer
// these also catch reads past the end of a block.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ancsparser.h"

namespace {

using Bytes = std::vector<uint8_t>;

const uint32_t uid = 0x01020304;

Bytes header(uint32_t notificationUid = uid)
{
	return { COMMAND_ID_GET_NOTIFICATION_ATTRIBUTES, (uint8_t)notificationUid, (uint8_t)(notificationUid >> 8),
	         (uint8_t)(notificationUid >> 16), (uint8_t)(notificationUid >> 24) };
}

Bytes attribute(uint8_t id, const std::string &value)
{
	Bytes out = { id, (uint8_t)value.size(), (uint8_t)(value.size() >> 8) };
	out.insert(out.end(), value.begin(), value.end());
	return out;
}

Bytes operator+(Bytes a, const Bytes &b)
{
	a.insert(a.end(), b.begin(), b.end());
	return a;
}

Bytes slice(const Bytes &b, size_t from, size_t to)
{
	return Bytes(b.begin() + from, b.begin() + to);
}

NotificationIdQueryInfo query(std::vector<uint8_t> ids)
{
	NotificationIdQueryInfo info;
	info.notificationId = (int32_t)uid;
	for (uint8_t id : ids)
		info.attrList.push_back(NotificationAttr(id));
	return info;
}

std::string valueOf(const NotificationIdQueryInfo &info, uint8_t id)
{
	for (const auto &attr : info.attrList)
		if (attr.attrId == id)
			return attr.value;
	return "<missing>";
}

// Feeds a whole response cut at the given offsets; returns what the last block returned.
bool feed(NotificationIdQueryInfo &info, const Bytes &response, std::vector<size_t> cuts)
{
	cuts.push_back(response.size());
	size_t from = 0;
	bool done = false;
	for (size_t to : cuts)
	{
		done = parseAncsDataSourceBlock(info, slice(response, from, to));
		from = to;
	}
	return done;
}

} // namespace

TEST(AncsNotificationSource, ShortValueIsRejected)
{
	AncsNotificationEvent event;
	EXPECT_FALSE(parseAncsNotificationSource(Bytes(7, 0), event));
	EXPECT_FALSE(parseAncsNotificationSource(Bytes(), event));
}

TEST(AncsNotificationSource, ParsesAllFields)
{
	AncsNotificationEvent event;
	ASSERT_TRUE(parseAncsNotificationSource({ 0, 2, 4, 1, 0xff, 0xff, 0xff, 0xff }, event));
	EXPECT_EQ(event.eventId, 0);
	EXPECT_EQ(event.eventFlags, 2);
	EXPECT_EQ(event.categoryId, 4);
	EXPECT_EQ(event.categoryCount, 1);
	EXPECT_EQ(event.notificationUid, 0xffffffffu);
}

TEST(AncsDataSource, OneBlock)
{
	auto info = query({ 0, 1 });
	EXPECT_TRUE(parseAncsDataSourceBlock(info, header() + attribute(0, "com.apple.MobileSMS") + attribute(1, "Hi")));
	EXPECT_EQ(valueOf(info, 0), "com.apple.MobileSMS");
	EXPECT_EQ(valueOf(info, 1), "Hi");
}

TEST(AncsDataSource, EmptyAndShortBlocksAreHarmless)
{
	auto info = query({ 1 });
	EXPECT_FALSE(parseAncsDataSourceBlock(info, Bytes()));
	EXPECT_FALSE(parseAncsDataSourceBlock(info, Bytes{ 0 }));
	EXPECT_FALSE(parseAncsDataSourceBlock(info, Bytes{ 0, 4, 3 }));
	EXPECT_FALSE(info.headerSeen);
	EXPECT_TRUE(parseAncsDataSourceBlock(info, header() + attribute(1, "x")));
}

TEST(AncsDataSource, AnswerToAnotherNotificationIsIgnored)
{
	auto info = query({ 1 });
	EXPECT_FALSE(parseAncsDataSourceBlock(info, header(0x0a0b0c0d) + attribute(1, "other")));
	EXPECT_EQ(valueOf(info, 1), "");
	EXPECT_TRUE(parseAncsDataSourceBlock(info, header() + attribute(1, "ours")));
	EXPECT_EQ(valueOf(info, 1), "ours");
}

TEST(AncsDataSource, ValueSplitAcrossBlocks)
{
	auto info = query({ 3 });
	Bytes response = header() + attribute(3, "a message that does not fit one block");
	EXPECT_TRUE(feed(info, response, { 12, 20 }));
	EXPECT_EQ(valueOf(info, 3), "a message that does not fit one block");
}

TEST(AncsDataSource, LengthSplitAfterACompleteAttribute)
{
	// The block ends after the first length byte of the second attribute. A complete first attribute left
	// remainingLen at 0, so the next block's second length byte was taken for an attribute id.
	auto info = query({ 1, 3 });
	Bytes first = header() + attribute(1, "Title");
	Bytes response = first + attribute(3, "Body");
	EXPECT_TRUE(feed(info, response, { first.size() + 2 }));
	EXPECT_EQ(valueOf(info, 1), "Title");
	EXPECT_EQ(valueOf(info, 3), "Body");
}

TEST(AncsDataSource, AttributeIdEndsABlockAfterACompleteAttribute)
{
	auto info = query({ 1, 3 });
	Bytes first = header() + attribute(1, "Title");
	Bytes response = first + attribute(3, "Body");
	EXPECT_TRUE(feed(info, response, { first.size() + 1 }));
	EXPECT_EQ(valueOf(info, 3), "Body");
}

TEST(AncsDataSource, ContinuationStartingWithAppIdentifierIsNoHeader)
{
	// Attribute id 0 (AppIdentifier) at the start of a later block was taken for a new response's header.
	auto info = query({ 1, 0 });
	Bytes first = header() + attribute(1, "Title");
	Bytes response = first + attribute(0, "com.example");
	EXPECT_TRUE(feed(info, response, { first.size() }));
	EXPECT_EQ(valueOf(info, 0), "com.example");
}

TEST(AncsDataSource, ContinuationOfAnAttributeNotAskedForIsSkipped)
{
	// Attribute 2 was not asked for; the rest of its value used to be parsed as further attributes.
	auto info = query({ 3 });
	Bytes first = header() + attribute(2, std::string(30, 'x'));
	Bytes response = first + attribute(3, "Body");
	EXPECT_TRUE(feed(info, response, { 15 }));
	EXPECT_EQ(valueOf(info, 3), "Body");
}

TEST(AncsDataSource, EmptyValueWithSplitLength)
{
	auto info = query({ 1, 3 });
	Bytes first = header() + attribute(1, "T");
	Bytes response = first + attribute(3, "");
	EXPECT_TRUE(feed(info, response, { first.size() + 2 }));
	EXPECT_EQ(valueOf(info, 3), "");
}

TEST(AncsDataSource, LengthBeyondTheDataStaysIncomplete)
{
	auto info = query({ 1 });
	Bytes block = header() + Bytes{ 1, 0xff, 0xff, 'a', 'b' };
	EXPECT_FALSE(parseAncsDataSourceBlock(info, block));
	EXPECT_EQ(valueOf(info, 1), "ab");
	EXPECT_EQ(info.remainingLen, 0xffff - 2);
	// More data continues it, without reading past any block.
	EXPECT_FALSE(parseAncsDataSourceBlock(info, Bytes(100, 'c')));
	EXPECT_EQ(valueOf(info, 1).size(), 102u);
}
