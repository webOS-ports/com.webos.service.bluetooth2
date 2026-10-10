// Copyright (c) 2015-2018 LG Electronics, Inc.
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

// Parsing of what an ANCS notification provider (an iPhone) sends: notification source events and data source
// blocks. Kept apart from the profile, which needs a bus, so that it can be tested and fuzzed on its own: every
// byte here comes from the remote device.

#ifndef ANCSPARSER_H_
#define ANCSPARSER_H_

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

typedef struct LSMessage LSMessage;

#define COMMAND_ID_GET_NOTIFICATION_ATTRIBUTES 0
#define MAX_UINT16 0xffff

class NotificationAttr {
public:
	uint8_t attrId;
	std::string value;
	bool found =false;
	NotificationAttr(uint8_t id) : attrId(id), found(false) {}
};

class NotificationIdQueryInfo
{
public:
	std::string deviceAddress;
	int32_t notificationId = 0;
	std::vector <NotificationAttr> attrList;
	uint16_t readingAttr = MAX_UINT16;	// attribute whose length or value continues in the next block
	uint16_t attrLenByte1 = MAX_UINT16;	// first length byte, when the block ended between the two
	int remainingLen = -1;			// value bytes still to come for readingAttr, -1 when none
	bool headerSeen = false;		// the response's first block, with the command id, has arrived
	time_t startTime = 0;
	LSMessage *requestMessage = nullptr;
};

struct AncsNotificationEvent
{
	uint8_t eventId;
	uint8_t eventFlags;
	uint8_t categoryId;
	uint8_t categoryCount;
	uint32_t notificationUid;
};

// A notification source value: false when it is too short to be one.
bool parseAncsNotificationSource(const std::vector<uint8_t> &values, AncsNotificationEvent &event);

// One data source block of the response to a Get Notification Attributes command, added to info. Returns true
// once every attribute in info.attrList has its whole value.
bool parseAncsDataSourceBlock(NotificationIdQueryInfo &info, const std::vector<uint8_t> &values);

#endif
