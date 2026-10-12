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

// changePath and playItem: the Luna caller is answered with what the remote device answered, once, and a SIL
// without the callback variants still answers it at once.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "avrcpbrowse.h"

namespace {

class StubAvrcp : public BluetoothAvrcpProfile
{
public:
	void supplyMediaMetaData(BluetoothAvrcpRequestId, const BluetoothMediaMetaData &, BluetoothResultCallback) override {}
	void supplyMediaPlayStatus(BluetoothAvrcpRequestId, const BluetoothMediaPlayStatus &, BluetoothResultCallback) override {}
	void notifyMediaPlayStatus(const BluetoothMediaPlayStatus &, BluetoothResultCallback) override {}
};

// A SIL that waits for the device: it keeps the callbacks until the test answers for the device.
class WaitingAvrcp : public StubAvrcp
{
public:
	BluetoothError changePath(const std::string &) override
	{
		syncCalls.push_back("changePath");
		return BLUETOOTH_ERROR_NONE;
	}

	BluetoothError playItem(const std::string &) override
	{
		syncCalls.push_back("playItem");
		return BLUETOOTH_ERROR_NONE;
	}

	void changePath(const std::string &itemPath, BluetoothResultCallback callback) override
	{
		paths.push_back("changePath " + itemPath);
		pending.push_back(callback);
	}

	void playItem(const std::string &itemPath, BluetoothResultCallback callback) override
	{
		paths.push_back("playItem " + itemPath);
		pending.push_back(callback);
	}

	std::vector<std::string> syncCalls;
	std::vector<std::string> paths;
	std::vector<BluetoothResultCallback> pending;
};

// A SIL built before the callback variants.
class OldAvrcp : public StubAvrcp
{
public:
	using StubAvrcp::changePath;
	using StubAvrcp::playItem;

	BluetoothError changePath(const std::string &) override { return result; }
	BluetoothError playItem(const std::string &) override { return result; }

	BluetoothError result = BLUETOOTH_ERROR_NONE;
};

struct Answers
{
	std::vector<BluetoothError> errors;

	BluetoothResultCallback callback()
	{
		return [this](BluetoothError error) { errors.push_back(error); };
	}
};

} // namespace

TEST(AvrcpBrowse, AnsweredWithTheDevicesResult)
{
	for (AvrcpBrowseRequest request : { AvrcpBrowseRequest::CHANGE_PATH, AvrcpBrowseRequest::PLAY_ITEM })
	{
		for (BluetoothError deviceAnswer : { BLUETOOTH_ERROR_NONE, BLUETOOTH_ERROR_FAIL })
		{
			WaitingAvrcp sil;
			Answers answers;
			sendAvrcpBrowseRequest(&sil, request, "/Filesystem/item1", answers.callback());
			ASSERT_EQ(sil.pending.size(), 1u);
			EXPECT_TRUE(sil.syncCalls.empty());	// not the variant that cannot report the answer
			EXPECT_EQ(sil.paths[0], std::string(request == AvrcpBrowseRequest::CHANGE_PATH ? "changePath" :
				"playItem") + " /Filesystem/item1");
			EXPECT_TRUE(answers.errors.empty());	// not before the device answered

			sil.pending[0](deviceAnswer);
			ASSERT_EQ(answers.errors.size(), 1u);
			EXPECT_EQ(answers.errors[0], deviceAnswer);
		}
	}
}

// Each answer drops a reference to the Luna message: a second one would drop it once too often.
TEST(AvrcpBrowse, AnsweredOnlyOnce)
{
	WaitingAvrcp sil;
	Answers answers;
	sendAvrcpBrowseRequest(&sil, AvrcpBrowseRequest::PLAY_ITEM, "/item2", answers.callback());
	ASSERT_EQ(sil.pending.size(), 1u);
	sil.pending[0](BLUETOOTH_ERROR_FAIL);
	sil.pending[0](BLUETOOTH_ERROR_NONE);
	auto copy = sil.pending[0];
	copy(BLUETOOTH_ERROR_NONE);
	ASSERT_EQ(answers.errors.size(), 1u);
	EXPECT_EQ(answers.errors[0], BLUETOOTH_ERROR_FAIL);
}

TEST(AvrcpBrowse, OldSilIsAnsweredAtOnce)
{
	for (BluetoothError result : { BLUETOOTH_ERROR_NONE, BLUETOOTH_ERROR_AVRCP_NOT_A_FOLDER, BLUETOOTH_ERROR_BUSY })
	{
		OldAvrcp sil;
		sil.result = result;
		Answers answers;
		sendAvrcpBrowseRequest(&sil, AvrcpBrowseRequest::CHANGE_PATH, "/item1", answers.callback());
		ASSERT_EQ(answers.errors.size(), 1u);
		EXPECT_EQ(answers.errors[0], result);

		answers.errors.clear();
		sendAvrcpBrowseRequest(&sil, AvrcpBrowseRequest::PLAY_ITEM, "/item2", answers.callback());
		ASSERT_EQ(answers.errors.size(), 1u);
		EXPECT_EQ(answers.errors[0], result);
	}
}
