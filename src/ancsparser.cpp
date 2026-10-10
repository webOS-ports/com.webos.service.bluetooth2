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

#include "ancsparser.h"

#include <algorithm>

namespace {

NotificationAttr *findAttr(NotificationIdQueryInfo &info, uint16_t attrId)
{
	for (auto &attr : info.attrList)
	{
		if (attr.attrId == attrId)
			return &attr;
	}
	return nullptr;
}

} // namespace

bool parseAncsNotificationSource(const std::vector<uint8_t> &values, AncsNotificationEvent &event)
{
	// Eight bytes from the remote device, which were read whatever it had sent.
	if (values.size() < 8)
		return false;

	event.eventId = values[0];
	event.eventFlags = values[1];
	event.categoryId = values[2];
	event.categoryCount = values[3];
	// No byte shifted into the sign bit of an int.
	event.notificationUid = values[4] | (values[5] << 8) | (values[6] << 16) | ((uint32_t)values[7] << 24);
	return true;
}

bool parseAncsDataSourceBlock(NotificationIdQueryInfo &info, const std::vector<uint8_t> &values)
{
	// Every read below is bounded by what the remote device sent; several were not.
	const size_t size = values.size();
	size_t index = 0;

	//Ancs block sizes are roughly 155 bytes long. When queried for multiple notification attributes,  the combined response length can
	//exceed this block size or message size may be longer than max block size.
	//Ancs chunks the response with only the first block containing the COMMAND_ID, and subsequent blocks containing only contiguous data
	//To handle responses spanning multiple blocks, we maintain required information in the query info,
	//which is updated after reading a complete attribute or when a new block is received.
	if (!info.headerSeen)
	{
		//Case 1:Incoming block format [0:CommandId], [1-4: NotificationId],[5-vSize : attr1]
		// Only the first block has the header. Every block starting with a 0 was taken for one, and 0 is also
		// the AppIdentifier attribute id, or any value byte that continued into the block.
		if (size < 5 || values[0] != COMMAND_ID_GET_NOTIFICATION_ATTRIBUTES)
			return false;
		uint32_t notificationId = values[1] | (values[2] << 8) | (values[3] << 16) | ((uint32_t)values[4] << 24);
		if ((int32_t)notificationId != info.notificationId)
			return false;	// the answer to another query
		info.headerSeen = true;
		index = 5;
	}
	else if (info.remainingLen == -1 && info.readingAttr != MAX_UINT16)
	{
		if (info.attrLenByte1 == MAX_UINT16)
		{
			//Case 3:Previous block: [0 to n :attr1-id-data][n+1 to (vSize -2): attr2 id and data][vsize-1: attr3 Id]
			//		 Current block:  [0 and 1: attr3 len_bytes_1_2][[1-n: attr3 data]....
			if (size < 2)
				return false;
			info.remainingLen = values[0] | (values[1] << 8);
			index = 2;
		}
		else
		{
			//Case 2:Previous block: [0 to n :attr1-id-data][n+1 to (vSize -2): attr2 id and data][vsize-2: attr3Id] [vsize-1: attr3 len_1st_byte]
			//		 Current block:  [0 :attr3 len_2nd_byte][1-n: attr3 data]....
			if (size < 1)
				return false;
			info.remainingLen = info.attrLenByte1 | (values[0] << 8);
			info.attrLenByte1 = MAX_UINT16;
			index = 1;
		}

		if (info.remainingLen == 0)
		{
			// An empty value whose length was split: complete as it is.
			if (NotificationAttr *attr = findAttr(info, info.readingAttr))
			{
				attr->value.clear();
				attr->found = true;
			}
			info.readingAttr = MAX_UINT16;
			info.remainingLen = -1;
		}
	}

	if (info.remainingLen > 0 && info.readingAttr != MAX_UINT16)
	{
		//Case 4: [0 - x: data overflowed from previous block] [x+1 - vSize :attrN]
		//Continuation of case 2 and case 3. Consumed whether or not the attribute was asked for: the bytes of
		// one that was not used to be read as further attributes.
		NotificationAttr *attr = findAttr(info, info.readingAttr);
		// What is left of this block after the length bytes, not the whole block.
		size_t len = std::min((size_t)info.remainingLen, size - index);
		if (attr)
			attr->value.append(values.begin() + index, values.begin() + index + len);
		index += len;
		info.remainingLen -= (int)len;
		if (info.remainingLen == 0)
		{
			if (attr)
				attr->found = true;
			info.readingAttr = MAX_UINT16;
			info.remainingLen = -1;
		}
	}

	while (index < size)
	{
		uint8_t attrId = values[index++];
		info.readingAttr = attrId;
		// In this order: with the attribute id as the last byte, the length byte was read past the end.
		if (index >= size)
			break;	//last byte was attrId
		if (index + 1 >= size)
		{
			//last byte was 1st byte of attrLen
			info.attrLenByte1 = values[index++];
			break;
		}
		size_t attributeLen = values[index] | (values[index + 1] << 8);
		index += 2;

		size_t valLen = std::min(attributeLen, size - index);
		NotificationAttr *attr = findAttr(info, attrId);
		if (attr)
			attr->value.assign(values.begin() + index, values.begin() + index + valLen);
		index += valLen;

		if (valLen == attributeLen)
		{
			if (attr)
				attr->found = true;
			// Nothing pending. A complete attribute left this at 0, and the next block's split length was
			// then taken for attribute data.
			info.readingAttr = MAX_UINT16;
			info.remainingLen = -1;
		}
		else
			info.remainingLen = (int)(attributeLen - valLen);
	}

	if (info.attrList.empty())
		return false;
	return std::all_of(info.attrList.begin(), info.attrList.end(), [](const NotificationAttr &attr) { return attr.found; });
}
