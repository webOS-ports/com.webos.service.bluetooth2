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

// SPP channel bookkeeping and the pair state. Run under AddressSanitizer or valgrind these catch the
// mismatched frees, leaks and stale timers the audit removed.

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <glib.h>
#include <string>

#include "bluetoothdevice.h"
#include "bluetoothpairstate.h"
#include "channelmanager.h"

namespace {

void pumpFor(int ms)
{
	auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (std::chrono::steady_clock::now() < end)
		g_main_context_iteration(nullptr, FALSE);
}

const uint8_t payload[] = { 'a', 'b', 'c' };

} // namespace

TEST(ChannelManager, ReceivedDataComesBackWhole)
{
	ChannelManager manager;
	std::string channel = manager.markChannelAsConnected(5, "00:11:22:33:44:55", "uuid-a");
	ASSERT_EQ(channel, "001");

	manager.addReceiveQueue("adapter", 5, payload, sizeof(payload));
	pumpFor(20);	// the idle notification

	const ChannelManager::DataBuffer *buffer = manager.getChannelBufferData(channel, "");
	ASSERT_NE(buffer, nullptr);
	ASSERT_EQ(buffer->size, sizeof(payload));
	EXPECT_EQ(memcmp(buffer->buffer, payload, sizeof(payload)), 0);
	delete buffer;
}

TEST(ChannelManager, QueuedDataIsFreedWithTheManager)
{
	// Packets nobody read: the destructor freed the queue entries but not their buffers, which were
	// allocated with new[] and, when read, freed with plain delete.
	auto *manager = new ChannelManager();
	manager->markChannelAsConnected(7, "00:11:22:33:44:66", "uuid-b");
	manager->addReceiveQueue("adapter", 7, payload, sizeof(payload));
	manager->addReceiveQueue("adapter", 7, payload, sizeof(payload));
	pumpFor(20);
	delete manager;
}

TEST(ChannelManager, DeletedReadSubscriptionTakesItsTimerWithIt)
{
	ChannelManager manager;
	void *subscription = manager.addReadDataSubscription("001", 1, nullptr, "com.example.app");
	ASSERT_NE(subscription, nullptr);

	// Deleted another way before its timeout: the timer used to stay and delete the freed entry again,
	// or whatever had been allocated at its address since.
	manager.deleteReadDataSubscription(subscription);
	pumpFor(1300);
}

TEST(ChannelManager, ExpiredReadSubscriptionIsDeletedOnce)
{
	auto *manager = new ChannelManager();
	manager->addReadDataSubscription("001", 1, nullptr, "com.example.app");
	pumpFor(1300);	// the timeout deletes it
	delete manager;	// and the destructor must not delete it again
}

TEST(ChannelManager, SubscriptionLeftAtDestructionIsFreed)
{
	auto *manager = new ChannelManager();
	manager->addReadDataSubscription("001", 5, nullptr, "com.example.app");
	delete manager;	// its timer goes with it
	pumpFor(20);
}

TEST(PairState, ForgetsADeviceThatIsRemovedWhilePairing)
{
	BluetoothPairState state;
	auto *device = new BluetoothDevice();
	state.startPairing(device);
	ASSERT_EQ(state.getDevice(), device);

	// deviceRemoved(): the device goes while pairing, then the pairing ends.
	state.forgetDevice(device);
	delete device;
	EXPECT_EQ(state.getDevice(), nullptr);
	EXPECT_TRUE(state.isPairing());

	state.stopPairing();	// used to write to the deleted device
	EXPECT_FALSE(state.isPairing());
}

TEST(PairState, ForgettingAnotherDeviceKeepsTheOneBeingPaired)
{
	BluetoothPairState state;
	BluetoothDevice paired, other;
	state.startPairing(&paired);
	state.forgetDevice(&other);
	EXPECT_EQ(state.getDevice(), &paired);
	state.stopPairing();
}
