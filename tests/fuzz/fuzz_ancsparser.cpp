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

// libFuzzer target for the ANCS parser: everything it parses comes from the remote device.
//
// Input: [n] [n attribute ids] then blocks, each [length] [length bytes]. The first block is fed to the
// notification source parser as well. Checks, besides the sanitizers, that the parser's state stays consistent.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "ancsparser.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 1)
		return 0;
	size_t pos = 0;
	size_t count = data[pos++] % 8;
	if (size - pos < count)
		return 0;

	NotificationIdQueryInfo info;
	info.notificationId = 0x01020304;
	for (size_t i = 0; i < count; i++)
		info.attrList.push_back(NotificationAttr(data[pos++]));

	bool first = true;
	while (pos < size)
	{
		size_t length = data[pos++];
		if (length > size - pos)
			length = size - pos;
		std::vector<uint8_t> block(data + pos, data + pos + length);
		pos += length;

		if (first)
		{
			AncsNotificationEvent event;
			parseAncsNotificationSource(block, event);
			first = false;
		}

		bool done = parseAncsDataSourceBlock(info, block);

		if (info.remainingLen < -1 || info.remainingLen > 0xffff)
			abort();
		if (info.remainingLen > 0 && info.readingAttr == MAX_UINT16)
			abort();
		if (done && info.attrList.empty())
			abort();
		for (const auto &attr : info.attrList)
			if (attr.value.size() > 0xffff)
				abort();
	}
	return 0;
}
