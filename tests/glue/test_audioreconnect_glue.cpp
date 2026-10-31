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

// The audio reconnect as the service runs it: the real manager, the real A2DP and HFP profile services and
// BluetoothAudioReconnect, on a SIL in the test (fakesil.h), a bus in the test (fakels.h) and a clock the test
// moves. test_audioreconnect.cpp tests what is remembered; this tests what is done with it.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <glib.h>

#include "bluetoothaudioreconnect.h"
#include "bluetoothmanagerservice.h"
#include "fakels.h"
#include "fakesil.h"

namespace {

const char HEADSET[] = "00:aa:bb:cc:dd:01";
const char SPEAKER[] = "00:aa:bb:cc:dd:02";
const char A2DP_SINK[] = "0000110b-0000-1000-8000-00805f9b34fb";
const char HANDSFREE[] = "0000111e-0000-1000-8000-00805f9b34fb";
const char AVRCP_TARGET[] = "0000110c-0000-1000-8000-00805f9b34fb";

// Timers that fire when the test says time has passed.
class FakeClock : public BluetoothAudioReconnect::Timers
{
public:
	unsigned int add(unsigned int seconds, std::function<void(unsigned int id)> fire) override
	{
		const unsigned int id = mNextId++;
		mPending.push_back({id, mNow + seconds, std::move(fire)});
		waits.push_back(seconds);
		return id;
	}

	void remove(unsigned int id) override
	{
		std::erase_if(mPending, [id](const Pending &timer) { return timer.id == id; });
	}

	//! Fires, in order, every timer due within seconds.
	void advance(unsigned int seconds)
	{
		const unsigned int until = mNow + seconds;
		for (;;)
		{
			auto next = std::min_element(mPending.begin(), mPending.end(),
			                             [](const Pending &a, const Pending &b) { return a.due < b.due; });
			if (next == mPending.end() || next->due > until)
				break;
			Pending timer = std::move(*next);
			mPending.erase(next);
			mNow = timer.due;
			timer.fire(timer.id);
		}
		mNow = until;
	}

	std::size_t pending() const { return mPending.size(); }

	//! The seconds of every wait asked for, in order.
	std::vector<unsigned int> waits;

private:
	struct Pending
	{
		unsigned int id;
		unsigned int due;
		std::function<void(unsigned int)> fire;
	};
	std::vector<Pending> mPending;
	unsigned int mNow = 0;
	unsigned int mNextId = 1;
};

// Where glue-sil.so is: GLUE_SIL_DIR, or the build directory. (/proc/self/exe is the loader when the test
// runs through a sysroot's ld.so.)
std::string silDirectory()
{
	const char *dir = getenv("GLUE_SIL_DIR");
	return dir ? dir : GLUE_SIL_BUILD_DIR;
}

std::string readFile(const char *path)
{
	std::ifstream in(path);
	std::stringstream text;
	text << in.rdbuf();
	return text.str();
}

class AudioReconnectGlue : public ::testing::Test
{
protected:
	void SetUp() override
	{
		unlink(AUDIO_RECONNECT_FILE);
		setenv("WEBOS_BLUETOOTH_SIL", "glue-sil", 1);
		setenv("WEBOS_BLUETOOTH_SIL_BASE_PATH", silDirectory().c_str(), 1);
		ASSERT_NO_FATAL_FAILURE(startService());
	}

	void TearDown() override
	{
		stopService();
		unlink(AUDIO_RECONNECT_FILE);
	}

	// The service starts, as at boot, and finds the adapter switched off.
	void startService()
	{
		gluesil::current = nullptr;
		mService = std::make_unique<BluetoothManagerService>();
		ASSERT_NE(gluesil::current, nullptr) << "the service did not load glue-sil.so";
		sil = gluesil::current;
		ASSERT_NE(mService->getAudioReconnect(), nullptr);
		auto timers = std::make_unique<FakeClock>();
		clock = timers.get();
		mService->getAudioReconnect()->useTimers(std::move(timers));
		sil->start();
		pump();
		ASSERT_NE(mService->findAdapterInfo(gluesil::ADAPTER), nullptr);
	}

	void stopService()
	{
		fakels::clear();
		mService.reset();
		pump();
		sil = nullptr;
		clock = nullptr;
	}

	// What the service put on the main loop runs.
	static void pump()
	{
		while (g_main_context_iteration(nullptr, FALSE))
			;
	}

	gluesil::Adapter &adapter() { return sil->adapter; }

	void addHeadset(const std::string &address, const std::vector<std::string> &uuids = { A2DP_SINK, HANDSFREE, AVRCP_TARGET })
	{
		adapter().addDevice(address, true, uuids);
	}

	// The adapter switched off and on again, as after a chip reset or the user's switch.
	void powerCycle()
	{
		adapter().power(false);
		adapter().power(true);
	}

	fakels::Reply luna(const std::string &category, const std::string &method, const std::string &payload)
	{
		fakels::Reply reply = fakels::call(category, method, payload);
		pump();
		return reply;
	}

	static bool succeeded(const fakels::Reply &reply)
	{
		return !reply.payloads.empty() && reply.payloads.front().find("\"returnValue\":true") != std::string::npos;
	}

	gluesil::Sil *sil = nullptr;
	FakeClock *clock = nullptr;

private:
	std::unique_ptr<BluetoothManagerService> mService;
};

// A headset that was in use, and went away by itself (or with the adapter).
TEST_F(AudioReconnectGlue, AHeadsetIsConnectedAgainWhenTheAdapterComesUp)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);
	EXPECT_TRUE(clock->waits.empty()) << "nothing to connect at the first power-on";

	powerCycle();
	ASSERT_EQ(clock->waits, std::vector<unsigned int>{ 2 });
	clock->advance(1);
	EXPECT_TRUE(adapter().a2dp.connects.empty()) << "not before the adapter's devices are back";

	clock->advance(1);
	EXPECT_EQ(adapter().a2dp.connects, std::vector<std::string>{ HEADSET });
	EXPECT_TRUE(adapter().hfp.connects.empty()) << "BlueZ connects the HFP of an A2DP headset itself";
	EXPECT_EQ(clock->pending(), 0U);
}

TEST_F(AudioReconnectGlue, AnHfpOnlyHeadsetIsConnectedByItsHfp)
{
	addHeadset(HEADSET, { HANDSFREE });
	adapter().power(true);
	adapter().hfp.remote(HEADSET, true);

	powerCycle();
	clock->advance(2);
	EXPECT_EQ(adapter().hfp.connects, std::vector<std::string>{ HEADSET });
	EXPECT_TRUE(adapter().a2dp.connects.empty());
}

TEST_F(AudioReconnectGlue, NothingIsConnectedWhenNothingWasInUse)
{
	addHeadset(HEADSET);
	adapter().power(true);
	powerCycle();
	clock->advance(100);
	EXPECT_TRUE(clock->waits.empty());
	EXPECT_TRUE(adapter().a2dp.connects.empty());
	EXPECT_TRUE(adapter().hfp.connects.empty());
}

TEST_F(AudioReconnectGlue, APhoneThisIsTheHeadsetForIsNotConnectedAgain)
{
	// An audio source and an audio gateway: a phone.
	addHeadset(HEADSET, { "0000110a-0000-1000-8000-00805f9b34fb", "0000111f-0000-1000-8000-00805f9b34fb" });
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);
	powerCycle();
	clock->advance(100);
	EXPECT_TRUE(adapter().a2dp.connects.empty());
}

TEST_F(AudioReconnectGlue, AFailingConnectIsTriedAfter2Then10Then30SecondsAndThenGivenUp)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);
	adapter().a2dp.connectAnswer = gluesil::Answer::FAIL;

	powerCycle();
	clock->advance(2);
	EXPECT_EQ(adapter().a2dp.connects.size(), 1U);
	clock->advance(9);
	EXPECT_EQ(adapter().a2dp.connects.size(), 1U);
	clock->advance(1);
	EXPECT_EQ(adapter().a2dp.connects.size(), 2U);
	clock->advance(29);
	EXPECT_EQ(adapter().a2dp.connects.size(), 2U);
	clock->advance(1);
	EXPECT_EQ(adapter().a2dp.connects.size(), 3U);

	clock->advance(3600);
	EXPECT_EQ(adapter().a2dp.connects.size(), 3U) << "given up after the third";
	EXPECT_EQ(clock->waits, (std::vector<unsigned int>{ 2, 10, 30 }));
	EXPECT_EQ(clock->pending(), 0U);
}

TEST_F(AudioReconnectGlue, ASecondAttemptThatWorksEndsTheAttempts)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);
	adapter().a2dp.connectAnswer = gluesil::Answer::FAIL;

	powerCycle();
	clock->advance(2);
	adapter().a2dp.connectAnswer = gluesil::Answer::OK;
	clock->advance(10);
	EXPECT_EQ(adapter().a2dp.connects.size(), 2U);
	clock->advance(3600);
	EXPECT_EQ(adapter().a2dp.connects.size(), 2U);
	EXPECT_EQ(clock->pending(), 0U);
}

TEST_F(AudioReconnectGlue, AHeadsetSomethingElseConnectedMeanwhileEndsTheAttempts)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);
	adapter().a2dp.connectAnswer = gluesil::Answer::ALREADY_CONNECTED;

	powerCycle();
	clock->advance(3600);
	EXPECT_EQ(adapter().a2dp.connects.size(), 1U);
	EXPECT_EQ(clock->waits, std::vector<unsigned int>{ 2 });
}

TEST_F(AudioReconnectGlue, AnAdapterSwitchedOffAgainIsNotUsed)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);

	powerCycle();
	adapter().power(false);
	clock->advance(3600);
	EXPECT_TRUE(adapter().a2dp.connects.empty());
}

TEST_F(AudioReconnectGlue, AHeadsetThatConnectedByItselfIsLeftAlone)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);

	powerCycle();
	adapter().a2dp.remote(HEADSET, true);	// it pages the phone before the 2 s are up
	clock->advance(100);
	EXPECT_TRUE(adapter().a2dp.connects.empty());
	EXPECT_TRUE(adapter().hfp.connects.empty());
}

TEST_F(AudioReconnectGlue, AHeadsetWithOnlyItsHfpBackIsLeftAlone)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);

	powerCycle();
	adapter().hfp.remote(HEADSET, true);
	clock->advance(100);
	EXPECT_TRUE(adapter().a2dp.connects.empty());
}

TEST_F(AudioReconnectGlue, AHeadsetBeingConnectedIsLeftAlone)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);

	powerCycle();
	// A client connects it before the 2 s are up, and the page takes a while.
	adapter().a2dp.connectAnswer = gluesil::Answer::DEFER;
	luna("/a2dp", "connect", std::string("{\"address\":\"") + HEADSET + "\",\"adapterAddress\":\"" + gluesil::ADAPTER + "\"}");
	ASSERT_EQ(adapter().a2dp.connects.size(), 1U);
	clock->advance(2);
	EXPECT_EQ(adapter().a2dp.connects.size(), 1U);
	adapter().a2dp.completeConnect(true);
	pump();
}

TEST_F(AudioReconnectGlue, TheUserDisconnectingThroughTheServiceForgetsTheHeadset)
{
	for (bool connectedFirst : { true, false })
	{
		SCOPED_TRACE(connectedFirst ? "CONNECTED false before the answer" : "the answer before CONNECTED false");
		addHeadset(HEADSET);
		adapter().power(true);
		adapter().a2dp.remote(HEADSET, true);
		adapter().a2dp.connectedBeforeDisconnectAnswer = connectedFirst;

		const fakels::Reply reply = luna("/a2dp", "disconnect", std::string("{\"address\":\"") + HEADSET +
		                                 "\",\"adapterAddress\":\"" + gluesil::ADAPTER + "\"}");
		ASSERT_TRUE(succeeded(reply)) << (reply.payloads.empty() ? "no answer" : reply.payloads.front());
		EXPECT_EQ(adapter().a2dp.disconnects, std::vector<std::string>{ HEADSET });

		powerCycle();
		clock->advance(100);
		EXPECT_TRUE(clock->waits.empty());
		EXPECT_TRUE(adapter().a2dp.connects.empty());
		EXPECT_EQ(readFile(AUDIO_RECONNECT_FILE).find(HEADSET), std::string::npos);

		stopService();
		unlink(AUDIO_RECONNECT_FILE);
		ASSERT_NO_FATAL_FAILURE(startService());
	}
}

TEST_F(AudioReconnectGlue, AUserDisconnectThatEndsTheLinkButAnswersAnErrorStillForgets)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);
	adapter().a2dp.disconnectFails = true;

	const fakels::Reply reply = luna("/a2dp", "disconnect", std::string("{\"address\":\"") + HEADSET + "\"}");
	EXPECT_FALSE(succeeded(reply));

	powerCycle();
	clock->advance(100);
	EXPECT_TRUE(adapter().a2dp.connects.empty());
}

TEST_F(AudioReconnectGlue, TheUserDisconnectingHfpThroughTheServiceForgetsTheHeadset)
{
	addHeadset(HEADSET, { HANDSFREE });
	adapter().power(true);
	adapter().hfp.remote(HEADSET, true);
	ASSERT_TRUE(succeeded(luna("/hfp", "disconnect", std::string("{\"address\":\"") + HEADSET + "\"}")));

	powerCycle();
	clock->advance(100);
	EXPECT_TRUE(adapter().hfp.connects.empty());
}

// The user disconnecting one profile forgets the whole device, also while
// the other stays up (G5-9, G5-14): what they did not want back is the
// headset, not one of its profiles.
TEST_F(AudioReconnectGlue, TheUserDisconnectingOnlyHfpForgetsTheHeadsetWithItsA2dpStillUp)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);
	adapter().hfp.remote(HEADSET, true);
	ASSERT_TRUE(succeeded(luna("/hfp", "disconnect", std::string("{\"address\":\"") + HEADSET + "\"}")));
	EXPECT_EQ(readFile(AUDIO_RECONNECT_FILE).find(HEADSET), std::string::npos);

	powerCycle();
	clock->advance(100);
	EXPECT_TRUE(adapter().a2dp.connects.empty());
	EXPECT_TRUE(adapter().hfp.connects.empty());
}

// The user connecting the profile again remembers the headset with every
// profile it has up then, not only the one just connected: the A2DP that
// stayed up is brought back at the next power-on as well (it is the one
// connected then, and BlueZ brings the HFP).
TEST_F(AudioReconnectGlue, HfpConnectedAgainWhileA2dpStayedUpRemembersBoth)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);
	adapter().hfp.remote(HEADSET, true);
	ASSERT_TRUE(succeeded(luna("/hfp", "disconnect", std::string("{\"address\":\"") + HEADSET + "\"}")));
	ASSERT_TRUE(succeeded(luna("/hfp", "connect", std::string("{\"address\":\"") + HEADSET + "\"}")));
	EXPECT_NE(readFile(AUDIO_RECONNECT_FILE).find(std::string(gluesil::ADAPTER) + " " + HEADSET + " a2dp hfp\n"),
	          std::string::npos) << readFile(AUDIO_RECONNECT_FILE);

	powerCycle();
	clock->advance(2);
	EXPECT_EQ(adapter().a2dp.connects, std::vector<std::string>{ HEADSET });
	EXPECT_EQ(adapter().hfp.connects, std::vector<std::string>{ HEADSET }) << "only the user's connect";
}

TEST_F(AudioReconnectGlue, A2dpConnectedAgainWhileHfpStayedUpRemembersBoth)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);
	adapter().hfp.remote(HEADSET, true);
	ASSERT_TRUE(succeeded(luna("/a2dp", "disconnect", std::string("{\"address\":\"") + HEADSET + "\"}")));
	ASSERT_TRUE(succeeded(luna("/a2dp", "connect", std::string("{\"address\":\"") + HEADSET + "\"}")));
	EXPECT_NE(readFile(AUDIO_RECONNECT_FILE).find(std::string(gluesil::ADAPTER) + " " + HEADSET + " a2dp hfp\n"),
	          std::string::npos) << readFile(AUDIO_RECONNECT_FILE);
}

// A profile the headset offers but did not connect is not remembered.
TEST_F(AudioReconnectGlue, OnlyTheProfilesUpAreRemembered)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);
	EXPECT_NE(readFile(AUDIO_RECONNECT_FILE).find(std::string(gluesil::ADAPTER) + " " + HEADSET + " a2dp\n"),
	          std::string::npos) << readFile(AUDIO_RECONNECT_FILE);
}

// An HFP link to a device that offers no hands-free unit (this phone is its
// headset) is not one of the headset's profiles to remember.
TEST_F(AudioReconnectGlue, AnHfpLinkOfADeviceWithoutHandsFreeIsNotRemembered)
{
	addHeadset(SPEAKER, { A2DP_SINK, AVRCP_TARGET });
	adapter().power(true);
	adapter().hfp.remote(SPEAKER, true);
	adapter().a2dp.remote(SPEAKER, true);
	EXPECT_NE(readFile(AUDIO_RECONNECT_FILE).find(std::string(gluesil::ADAPTER) + " " + SPEAKER + " a2dp\n"),
	          std::string::npos) << readFile(AUDIO_RECONNECT_FILE);
}

TEST_F(AudioReconnectGlue, TheHeadsetGoingAwayKeepsIt)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);
	adapter().a2dp.remote(HEADSET, false);	// switched off, or out of range

	powerCycle();
	clock->advance(2);
	EXPECT_EQ(adapter().a2dp.connects, std::vector<std::string>{ HEADSET });
}

TEST_F(AudioReconnectGlue, ForgetOverTheBusKeepsItFromComingBack)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);
	// The system menu: forget, then disconnect through BlueZ, which looks like the headset going away.
	ASSERT_TRUE(succeeded(luna("/autoReconnect", "forget", std::string("{\"address\":\"") + HEADSET +
	                           "\",\"adapterAddress\":\"" + gluesil::ADAPTER + "\"}")));
	adapter().a2dp.remote(HEADSET, false);

	powerCycle();
	clock->advance(100);
	EXPECT_TRUE(adapter().a2dp.connects.empty());
}

TEST_F(AudioReconnectGlue, ForgetWithoutAnAdapterForgetsTheHeadset)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);
	// As Settings asks: the address only, in BluezQt's upper case.
	std::string upper = HEADSET;
	std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
	ASSERT_TRUE(succeeded(luna("/autoReconnect", "forget", "{\"address\":\"" + upper + "\"}")));
	adapter().a2dp.remote(HEADSET, false);

	powerCycle();
	clock->advance(100);
	EXPECT_TRUE(adapter().a2dp.connects.empty());
}

TEST_F(AudioReconnectGlue, ForgettingAnotherDeviceKeepsTheHeadset)
{
	addHeadset(HEADSET);
	addHeadset(SPEAKER);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);
	ASSERT_TRUE(succeeded(luna("/autoReconnect", "forget", std::string("{\"address\":\"") + SPEAKER + "\"}")));
	adapter().a2dp.remote(HEADSET, false);

	powerCycle();
	clock->advance(2);
	EXPECT_EQ(adapter().a2dp.connects, std::vector<std::string>{ HEADSET });
}

TEST_F(AudioReconnectGlue, ForgetWantsAnAddress)
{
	EXPECT_FALSE(succeeded(luna("/autoReconnect", "forget", "{}")));
	EXPECT_FALSE(succeeded(luna("/autoReconnect", "forget", "{\"address\":\"headset\"}")));
	EXPECT_FALSE(succeeded(luna("/autoReconnect", "forget", std::string("{\"address\":\"") + HEADSET +
	                            "\",\"adapterAddress\":\"hci0\"}")));
}

TEST_F(AudioReconnectGlue, SwitchedOffNothingIsConnectedAgain)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);

	const fakels::Reply reply = luna("/autoReconnect", "setState", "{\"enabled\":false}");
	ASSERT_TRUE(succeeded(reply));
	EXPECT_NE(reply.payloads.front().find("\"enabled\":false"), std::string::npos);

	powerCycle();
	clock->advance(100);
	EXPECT_TRUE(clock->waits.empty());
	EXPECT_TRUE(adapter().a2dp.connects.empty());
	EXPECT_NE(luna("/autoReconnect", "getStatus", "{}").payloads.front().find("\"enabled\":false"), std::string::npos);
}

TEST_F(AudioReconnectGlue, SwitchingOffStopsTheAttemptsUnderWay)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);
	adapter().a2dp.connectAnswer = gluesil::Answer::FAIL;

	powerCycle();
	clock->advance(2);
	ASSERT_EQ(clock->pending(), 1U);
	ASSERT_TRUE(succeeded(luna("/autoReconnect", "setState", "{\"enabled\":false}")));
	EXPECT_EQ(clock->pending(), 0U);
	clock->advance(100);
	EXPECT_EQ(adapter().a2dp.connects.size(), 1U);
}

TEST_F(AudioReconnectGlue, SwitchedBackOnTheHeadsetComesBackAgain)
{
	addHeadset(HEADSET);
	adapter().power(true);
	ASSERT_TRUE(succeeded(luna("/autoReconnect", "setState", "{\"enabled\":false}")));
	adapter().a2dp.remote(HEADSET, true);	// kept track of while off
	ASSERT_TRUE(succeeded(luna("/autoReconnect", "setState", "{\"enabled\":true}")));

	powerCycle();
	clock->advance(2);
	EXPECT_EQ(adapter().a2dp.connects, std::vector<std::string>{ HEADSET });
}

TEST_F(AudioReconnectGlue, TheSwitchIsToldToItsSubscribers)
{
	const fakels::Reply first = luna("/autoReconnect", "getStatus", "{\"subscribe\":true}");
	ASSERT_TRUE(succeeded(first));
	EXPECT_NE(first.payloads.front().find("\"enabled\":true"), std::string::npos);
	EXPECT_NE(first.payloads.front().find("\"subscribed\":true"), std::string::npos);

	ASSERT_TRUE(succeeded(luna("/autoReconnect", "setState", "{\"enabled\":false}")));
	const std::vector<std::string> posts = fakels::posts("/autoReconnect", "getStatus");
	ASSERT_EQ(posts.size(), 2U);
	EXPECT_NE(posts.back().find("\"enabled\":false"), std::string::npos);

	// The same again changes nothing, and says nothing.
	ASSERT_TRUE(succeeded(luna("/autoReconnect", "setState", "{\"enabled\":false}")));
	EXPECT_EQ(fakels::posts("/autoReconnect", "getStatus").size(), 2U);
}

TEST_F(AudioReconnectGlue, TheSwitchAndTheHeadsetSurviveARestartOfTheService)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);

	stopService();
	ASSERT_NO_FATAL_FAILURE(startService());
	addHeadset(HEADSET);
	adapter().power(true);
	clock->advance(2);
	EXPECT_EQ(adapter().a2dp.connects, std::vector<std::string>{ HEADSET });

	ASSERT_TRUE(succeeded(luna("/autoReconnect", "setState", "{\"enabled\":false}")));
	stopService();
	ASSERT_NO_FATAL_FAILURE(startService());
	EXPECT_NE(luna("/autoReconnect", "getStatus", "{}").payloads.front().find("\"enabled\":false"), std::string::npos);
}

TEST_F(AudioReconnectGlue, ADeviceTheAdapterHasNotFoundYetIsTriedAgain)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);

	stopService();
	ASSERT_NO_FATAL_FAILURE(startService());
	adapter().power(true);	// after a chip reset: the adapter first, its devices later
	clock->advance(2);
	EXPECT_TRUE(adapter().a2dp.connects.empty());
	addHeadset(HEADSET);
	clock->advance(10);
	EXPECT_EQ(adapter().a2dp.connects, std::vector<std::string>{ HEADSET });
}

TEST_F(AudioReconnectGlue, AHeadsetNoLongerPairedIsForgotten)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);
	adapter().setPaired(HEADSET, false);

	powerCycle();
	clock->advance(100);
	EXPECT_TRUE(adapter().a2dp.connects.empty());
	EXPECT_EQ(clock->waits, std::vector<unsigned int>{ 2 }) << "tried no further";
	EXPECT_EQ(readFile(AUDIO_RECONNECT_FILE).find(HEADSET), std::string::npos);
}

// G5-5: unpairing through the service forgets the headset at once. BlueZ's
// RemoveDevice takes its profiles down first, which the service cannot tell
// from a link loss.
TEST_F(AudioReconnectGlue, TheUserUnpairingTheHeadsetForgetsIt)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);
	adapter().hfp.remote(HEADSET, true);
	adapter().unpairWorks = true;

	ASSERT_TRUE(succeeded(luna("/adapter", "unpair", std::string("{\"address\":\"") + HEADSET + "\"}")));
	EXPECT_EQ(readFile(AUDIO_RECONNECT_FILE).find(HEADSET), std::string::npos) << readFile(AUDIO_RECONNECT_FILE);

	powerCycle();
	clock->advance(100);
	EXPECT_TRUE(clock->waits.empty()) << "nothing left to connect";
	EXPECT_TRUE(adapter().a2dp.connects.empty());
	EXPECT_TRUE(adapter().hfp.connects.empty());
}

TEST_F(AudioReconnectGlue, AnUnpairThatFailsKeepsTheHeadset)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);

	EXPECT_FALSE(succeeded(luna("/adapter", "unpair", std::string("{\"address\":\"") + HEADSET + "\"}")));
	powerCycle();
	clock->advance(2);
	EXPECT_EQ(adapter().a2dp.connects, std::vector<std::string>{ HEADSET });
}

TEST_F(AudioReconnectGlue, UnpairingAnotherDeviceKeepsTheHeadset)
{
	addHeadset(HEADSET);
	addHeadset(SPEAKER);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);
	adapter().unpairWorks = true;

	ASSERT_TRUE(succeeded(luna("/adapter", "unpair", std::string("{\"address\":\"") + SPEAKER + "\"}")));
	powerCycle();
	clock->advance(2);
	EXPECT_EQ(adapter().a2dp.connects, std::vector<std::string>{ HEADSET });
}

// Removed from BlueZ behind the service's back (bluetoothctl remove). That
// looks the same to the service as a chip reset, whose devices come back
// after the adapter, so the attempts wait for it as long as they last; one
// still unknown at the last of them is gone for good, and forgotten instead
// of being tried at every power-on again.
TEST_F(AudioReconnectGlue, AHeadsetBlueZNoLongerKnowsIsForgottenAtTheLastAttempt)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);
	adapter().removeDevice(HEADSET);

	powerCycle();
	clock->advance(2 + 10);
	EXPECT_NE(readFile(AUDIO_RECONNECT_FILE).find(HEADSET), std::string::npos) << "not before the last attempt";
	clock->advance(30);
	EXPECT_EQ(readFile(AUDIO_RECONNECT_FILE).find(HEADSET), std::string::npos) << readFile(AUDIO_RECONNECT_FILE);
	EXPECT_EQ(clock->waits, (std::vector<unsigned int>{ 2, 10, 30 }));
	EXPECT_EQ(clock->pending(), 0U);

	powerCycle();
	clock->advance(100);
	EXPECT_EQ(clock->waits, (std::vector<unsigned int>{ 2, 10, 30 })) << "no attempts at the next power-on";
	EXPECT_TRUE(adapter().a2dp.connects.empty());
}

// The chip-reset case: the device is back by the last attempt, and connected.
TEST_F(AudioReconnectGlue, ADeviceFoundOnlyByTheLastAttemptIsStillConnected)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);

	stopService();
	ASSERT_NO_FATAL_FAILURE(startService());
	adapter().power(true);
	clock->advance(2 + 10);
	EXPECT_TRUE(adapter().a2dp.connects.empty());
	addHeadset(HEADSET);
	clock->advance(30);
	EXPECT_EQ(adapter().a2dp.connects, std::vector<std::string>{ HEADSET });
	EXPECT_NE(readFile(AUDIO_RECONNECT_FILE).find(HEADSET), std::string::npos);
}

// A headset that is there but does not answer (switched off) is kept: only
// one BlueZ does not know is forgotten (G5-6).
TEST_F(AudioReconnectGlue, AHeadsetThatDoesNotAnswerIsKeptAfterTheLastAttempt)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);
	adapter().a2dp.connectAnswer = gluesil::Answer::FAIL;

	powerCycle();
	clock->advance(100);
	EXPECT_EQ(adapter().a2dp.connects.size(), 3U);
	EXPECT_NE(readFile(AUDIO_RECONNECT_FILE).find(HEADSET), std::string::npos);
}

TEST_F(AudioReconnectGlue, PoweredAgainBeforeTheAttemptsAreDoneStartsOver)
{
	addHeadset(HEADSET);
	adapter().power(true);
	adapter().a2dp.remote(HEADSET, true);
	adapter().a2dp.connectAnswer = gluesil::Answer::FAIL;

	powerCycle();
	clock->advance(2);
	powerCycle();
	EXPECT_EQ(clock->pending(), 1U) << "the 10 s wait dropped, a 2 s one instead";
	EXPECT_EQ(clock->waits, (std::vector<unsigned int>{ 2, 10, 2 }));
}

}
