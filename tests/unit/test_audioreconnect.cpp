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

// The audio device to connect again when an adapter comes up: the one whose
// A2DP or HFP connected last on that adapter, remembered across restarts,
// forgotten when the user disconnects it, behind one switch.

#include <gtest/gtest.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "audioreconnect.h"

namespace {
const char kAdapter[] = "00:1a:7d:da:71:13";
const char kOther[] = "00:1a:7d:da:71:14";
const char kNura[] = "aa:bb:cc:dd:ee:01";
const char kSpeaker[] = "aa:bb:cc:dd:ee:02";
const char kMono[] = "aa:bb:cc:dd:ee:03";

const char kA2dpSink[] = "0000110b-0000-1000-8000-00805f9b34fb";
const char kA2dpSource[] = "0000110a-0000-1000-8000-00805f9b34fb";
const char kHandsFree[] = "0000111e-0000-1000-8000-00805f9b34fb";
const char kAudioGateway[] = "0000111f-0000-1000-8000-00805f9b34fb";

const unsigned kBoth = AudioReconnect::A2DP | AudioReconnect::HFP;

std::string switchLine(bool on)
{
	return on ? "autoReconnect on\n" : "autoReconnect off\n";
}
}

TEST(AudioReconnect, NothingConnectedYetIsNothingToReconnect)
{
	AudioReconnect r;
	EXPECT_TRUE(r.target(kAdapter).empty());
}

TEST(AudioReconnect, TheDeviceConnectedLastIsTheOne)
{
	AudioReconnect r;
	EXPECT_TRUE(r.connected(kAdapter, kNura, AudioReconnect::A2DP));
	EXPECT_EQ(r.target(kAdapter).address, kNura);
	EXPECT_TRUE(r.connected(kAdapter, kSpeaker, AudioReconnect::A2DP));
	EXPECT_EQ(r.target(kAdapter).address, kSpeaker);
}

TEST(AudioReconnect, ConnectingTheSameAgainNeedsNoStoring)
{
	AudioReconnect r;
	EXPECT_TRUE(r.connected(kAdapter, kNura, AudioReconnect::A2DP));
	EXPECT_FALSE(r.connected(kAdapter, "AA:BB:CC:DD:EE:01", AudioReconnect::A2DP));
}

TEST(AudioReconnect, AddressesAreTakenInEitherCase)
{
	AudioReconnect r;
	r.connected("00:1A:7D:DA:71:13", "AA:BB:CC:DD:EE:01", AudioReconnect::A2DP);
	EXPECT_EQ(r.target(kAdapter).address, kNura);
}

TEST(AudioReconnect, EachAdapterHasItsOwn)
{
	AudioReconnect r;
	r.connected(kAdapter, kNura, AudioReconnect::A2DP);
	r.connected(kOther, kSpeaker, AudioReconnect::A2DP);
	EXPECT_EQ(r.target(kAdapter).address, kNura);
	EXPECT_EQ(r.target(kOther).address, kSpeaker);
}

TEST(AudioReconnect, ADeviceNoLongerPairedIsForgotten)
{
	AudioReconnect r;
	r.connected(kAdapter, kNura, AudioReconnect::A2DP);
	EXPECT_FALSE(r.forget(kAdapter, kSpeaker));
	EXPECT_EQ(r.target(kAdapter).address, kNura);
	EXPECT_TRUE(r.forget(kAdapter, kNura));
	EXPECT_TRUE(r.target(kAdapter).empty());
}

// --- which profiles ---

TEST(AudioReconnect, AHeadsetWithBothIsConnectedByItsA2dp)
{
	AudioReconnect r;
	EXPECT_TRUE(r.connected(kAdapter, kNura, AudioReconnect::HFP));
	EXPECT_TRUE(r.connected(kAdapter, kNura, AudioReconnect::A2DP));
	EXPECT_FALSE(r.connected(kAdapter, kNura, AudioReconnect::HFP));
	EXPECT_EQ(r.target(kAdapter).profiles, kBoth);
	// BlueZ connects its HFP after the A2DP (policy.c sink_cb).
	EXPECT_EQ(r.target(kAdapter).first(), AudioReconnect::A2DP);
}

TEST(AudioReconnect, AnHfpOnlyHeadsetIsConnectedByItsHfp)
{
	AudioReconnect r;
	r.connected(kAdapter, kNura, AudioReconnect::A2DP);
	EXPECT_TRUE(r.connected(kAdapter, kMono, AudioReconnect::HFP));
	EXPECT_EQ(r.target(kAdapter).address, kMono);
	EXPECT_EQ(r.target(kAdapter).profiles, static_cast<unsigned>(AudioReconnect::HFP));
	EXPECT_EQ(r.target(kAdapter).first(), AudioReconnect::HFP);
}

TEST(AudioReconnect, EveryProfileConnectedTogetherIsRemembered)
{
	AudioReconnect r;
	EXPECT_TRUE(r.connected(kAdapter, kNura, kBoth));
	EXPECT_EQ(r.target(kAdapter).profiles, kBoth);
	EXPECT_FALSE(r.connected(kAdapter, kNura, kBoth));
	EXPECT_FALSE(r.connected(kAdapter, kNura, AudioReconnect::HFP));
}

TEST(AudioReconnect, AProfileNewAmongThoseConnectedIsAdded)
{
	AudioReconnect r;
	r.connected(kAdapter, kNura, AudioReconnect::A2DP);
	EXPECT_TRUE(r.connected(kAdapter, kNura, kBoth));
	EXPECT_EQ(r.target(kAdapter).profiles, kBoth);
}

// hfp/disconnect forgot it while its A2DP stayed up; hfp/connect then
// connects HFP with A2DP still up.
TEST(AudioReconnect, ConnectedAgainAfterAUserDisconnectItComesBackWithAllItHasUp)
{
	AudioReconnect r;
	r.connected(kAdapter, kNura, AudioReconnect::A2DP);
	r.connected(kAdapter, kNura, AudioReconnect::HFP);
	EXPECT_TRUE(r.disconnected(kAdapter, kNura, AudioReconnect::Disconnect::ByUser));
	EXPECT_TRUE(r.target(kAdapter).empty());
	EXPECT_TRUE(r.connected(kAdapter, kNura, kBoth));
	EXPECT_EQ(r.target(kAdapter).profiles, kBoth);
	EXPECT_EQ(r.target(kAdapter).first(), AudioReconnect::A2DP);
	EXPECT_EQ(r.toText(), std::string("autoReconnect on\n") + kAdapter + " " + kNura + " a2dp hfp\n");
}

TEST(AudioReconnect, NoKnownProfileIsNothingConnected)
{
	AudioReconnect r;
	EXPECT_FALSE(r.connected(kAdapter, kNura, 0U));
	EXPECT_TRUE(r.target(kAdapter).empty());
	EXPECT_TRUE(r.connected(kAdapter, kNura, 1U << 5 | AudioReconnect::HFP));
	EXPECT_EQ(r.target(kAdapter).profiles, static_cast<unsigned>(AudioReconnect::HFP));
}

TEST(AudioReconnect, AnotherDeviceBringsOnlyWhatItConnected)
{
	AudioReconnect r;
	r.connected(kAdapter, kNura, AudioReconnect::A2DP);
	r.connected(kAdapter, kNura, AudioReconnect::HFP);
	r.connected(kAdapter, kSpeaker, AudioReconnect::A2DP);
	EXPECT_EQ(r.target(kAdapter).profiles, static_cast<unsigned>(AudioReconnect::A2DP));
}

TEST(AudioReconnect, OnlyHeadsetsAndSpeakersAreAudioDevices)
{
	const std::vector<std::string> headset = { "0000110B-0000-1000-8000-00805F9B34FB", kHandsFree };
	const std::vector<std::string> phone = { kA2dpSource, kAudioGateway };
	EXPECT_TRUE(AudioReconnect::isAudioDevice(AudioReconnect::A2DP, headset));
	EXPECT_TRUE(AudioReconnect::isAudioDevice(AudioReconnect::HFP, headset));
	EXPECT_FALSE(AudioReconnect::isAudioDevice(AudioReconnect::A2DP, phone));
	EXPECT_FALSE(AudioReconnect::isAudioDevice(AudioReconnect::HFP, phone));
	EXPECT_FALSE(AudioReconnect::isAudioDevice(AudioReconnect::HFP, { kA2dpSink }));
	EXPECT_FALSE(AudioReconnect::isAudioDevice(AudioReconnect::A2DP, {}));
}

TEST(AudioReconnect, TheProfileServicesAreKnownByName)
{
	AudioReconnect::Profile profile = AudioReconnect::HFP;
	EXPECT_TRUE(AudioReconnect::profileFromName("A2DP", profile));
	EXPECT_EQ(profile, AudioReconnect::A2DP);
	EXPECT_TRUE(AudioReconnect::profileFromName("HFP", profile));
	EXPECT_EQ(profile, AudioReconnect::HFP);
	EXPECT_FALSE(AudioReconnect::profileFromName("AVRCP", profile));
	EXPECT_FALSE(AudioReconnect::profileFromName("a2dp", profile));
}

// --- disconnects ---

TEST(AudioReconnect, TheUserDisconnectingForgetsTheDevice)
{
	AudioReconnect r;
	r.connected(kAdapter, kNura, AudioReconnect::A2DP);
	r.connected(kAdapter, kNura, AudioReconnect::HFP);
	EXPECT_TRUE(r.disconnected(kAdapter, "AA:BB:CC:DD:EE:01", AudioReconnect::Disconnect::ByUser));
	EXPECT_TRUE(r.target(kAdapter).empty());
	EXPECT_EQ(r.toText(), switchLine(true));
}

TEST(AudioReconnect, ALinkLossKeepsTheDevice)
{
	AudioReconnect r;
	r.connected(kAdapter, kNura, AudioReconnect::A2DP);
	EXPECT_FALSE(r.disconnected(kAdapter, kNura, AudioReconnect::Disconnect::ByRemote));
	EXPECT_EQ(r.target(kAdapter).address, kNura);
	EXPECT_EQ(r.target(kAdapter).profiles, static_cast<unsigned>(AudioReconnect::A2DP));
}

TEST(AudioReconnect, TheUserDisconnectingAnotherDeviceKeepsTheOne)
{
	AudioReconnect r;
	r.connected(kAdapter, kNura, AudioReconnect::A2DP);
	EXPECT_FALSE(r.disconnected(kAdapter, kSpeaker, AudioReconnect::Disconnect::ByUser));
	EXPECT_FALSE(r.disconnected(kOther, kNura, AudioReconnect::Disconnect::ByUser));
	EXPECT_EQ(r.target(kAdapter).address, kNura);
}

TEST(AudioReconnect, ConnectingAgainAfterTheUserDisconnectedRemembersItAgain)
{
	AudioReconnect r;
	r.connected(kAdapter, kNura, AudioReconnect::A2DP);
	r.disconnected(kAdapter, kNura, AudioReconnect::Disconnect::ByUser);
	EXPECT_TRUE(r.connected(kAdapter, kNura, AudioReconnect::A2DP));
	EXPECT_EQ(r.target(kAdapter).address, kNura);
}

TEST(AudioReconnect, ForgettingWithoutAnAdapterForgetsOnEach)
{
	AudioReconnect r;
	r.connected(kAdapter, kNura, AudioReconnect::A2DP);
	r.connected(kOther, kNura, AudioReconnect::HFP);
	r.connected("00:1a:7d:da:71:15", kSpeaker, AudioReconnect::A2DP);
	EXPECT_TRUE(r.forget("AA:BB:CC:DD:EE:01"));
	EXPECT_TRUE(r.target(kAdapter).empty());
	EXPECT_TRUE(r.target(kOther).empty());
	EXPECT_EQ(r.target("00:1a:7d:da:71:15").address, kSpeaker);
	EXPECT_FALSE(r.forget(kNura));
}

// --- the switch ---

TEST(AudioReconnect, TheSwitchIsOnUntilTurnedOff)
{
	AudioReconnect r;
	EXPECT_TRUE(r.enabled());
	EXPECT_FALSE(r.setEnabled(true));
	EXPECT_TRUE(r.setEnabled(false));
	EXPECT_FALSE(r.enabled());
	EXPECT_FALSE(r.setEnabled(false));
}

TEST(AudioReconnect, SwitchedOffNothingIsConnectedAgain)
{
	AudioReconnect r;
	r.connected(kAdapter, kNura, AudioReconnect::A2DP);
	r.setEnabled(false);
	EXPECT_TRUE(r.target(kAdapter).empty());
	EXPECT_TRUE(r.target(kOther).empty());
}

TEST(AudioReconnect, SwitchedOffItStillKeepsTrack)
{
	AudioReconnect r;
	r.setEnabled(false);
	EXPECT_TRUE(r.connected(kAdapter, kSpeaker, AudioReconnect::A2DP));
	r.setEnabled(true);
	EXPECT_EQ(r.target(kAdapter).address, kSpeaker);
}

// --- across restarts ---

TEST(AudioReconnect, WhatIsRememberedSurvivesARestart)
{
	AudioReconnect before;
	before.connected(kAdapter, kNura, AudioReconnect::A2DP);
	before.connected(kAdapter, kNura, AudioReconnect::HFP);
	before.connected(kOther, kMono, AudioReconnect::HFP);
	AudioReconnect after;
	after.fromText(before.toText());
	EXPECT_EQ(after.target(kAdapter).address, kNura);
	EXPECT_EQ(after.target(kAdapter).profiles, kBoth);
	EXPECT_EQ(after.target(kOther).address, kMono);
	EXPECT_EQ(after.target(kOther).profiles, static_cast<unsigned>(AudioReconnect::HFP));
	EXPECT_EQ(after.toText(), before.toText());
}

TEST(AudioReconnect, TheSwitchSurvivesARestart)
{
	AudioReconnect before;
	before.connected(kAdapter, kNura, AudioReconnect::A2DP);
	before.setEnabled(false);
	AudioReconnect after;
	after.fromText(before.toText());
	EXPECT_FALSE(after.enabled());
	EXPECT_TRUE(after.target(kAdapter).empty());
	after.setEnabled(true);
	EXPECT_EQ(after.target(kAdapter).address, kNura);
}

TEST(AudioReconnect, TheFileKeepsItAcrossARestart)
{
	gchar *dir = g_dir_make_tmp("bt-test-reconnect-XXXXXX", nullptr);
	ASSERT_NE(dir, nullptr);
	gchar *path = g_build_filename(dir, "audioReconnect.txt", nullptr);

	AudioReconnect fresh;
	fresh.load(path);	// no file yet
	EXPECT_TRUE(fresh.enabled());
	EXPECT_TRUE(fresh.target(kAdapter).empty());

	AudioReconnect before;
	before.connected(kAdapter, kMono, AudioReconnect::HFP);
	before.setEnabled(false);
	std::string error;
	EXPECT_TRUE(before.store(path, error)) << error;

	AudioReconnect after;
	after.setEnabled(true);
	after.load(path);
	EXPECT_FALSE(after.enabled());
	after.setEnabled(true);
	EXPECT_EQ(after.target(kAdapter).address, kMono);
	EXPECT_EQ(after.target(kAdapter).first(), AudioReconnect::HFP);

	g_unlink(path);
	g_rmdir(dir);
	g_free(path);
	g_free(dir);
}

TEST(AudioReconnect, AMissingFileIsNothingRememberedSwitchedOn)
{
	AudioReconnect r;
	r.connected(kAdapter, kNura, AudioReconnect::A2DP);
	r.setEnabled(false);
	r.load("no-such-directory-here/audioReconnect.txt");
	EXPECT_TRUE(r.enabled());
	EXPECT_EQ(r.toText(), switchLine(true));
}

TEST(AudioReconnect, AFileThatCannotBeWrittenSaysWhy)
{
	AudioReconnect r;
	std::string error;
	EXPECT_FALSE(r.store("no-such-directory-here/audioReconnect.txt", error));
	EXPECT_FALSE(error.empty());
}

TEST(AudioReconnect, AFileFromBeforeHfpMeansA2dp)
{
	// What 57a6ce6 wrote: no switch, no profiles.
	AudioReconnect r;
	r.fromText(std::string(kAdapter) + " " + kNura + "\n");
	EXPECT_TRUE(r.enabled());
	EXPECT_EQ(r.target(kAdapter).address, kNura);
	EXPECT_EQ(r.target(kAdapter).profiles, static_cast<unsigned>(AudioReconnect::A2DP));
}

TEST(AudioReconnect, AFileThatIsNotOursIsIgnored)
{
	AudioReconnect r;
	r.connected(kAdapter, kSpeaker, AudioReconnect::A2DP);
	r.setEnabled(false);
	r.fromText(std::string("garbage\n") + kAdapter + " not-an-address\n" +
	           "../../etc " + kNura + "\n" + kOther + " " + kNura + " extra\n" +
	           kOther + " " + kNura + " a2dp hid\n" +
	           "autoReconnect maybe\nautoReconnect off now\n" +
	           std::string(kAdapter) + " " + kNura + " hfp\n");
	EXPECT_TRUE(r.enabled());
	EXPECT_EQ(r.target(kAdapter).address, kNura);
	EXPECT_TRUE(r.target(kOther).empty());
	EXPECT_EQ(r.toText(), switchLine(true) + kAdapter + " " + kNura + " hfp\n");
}

TEST(AudioReconnect, OnlyAddressesAreAddresses)
{
	EXPECT_TRUE(AudioReconnect::isAddress("00:1a:7D:da:71:13"));
	EXPECT_FALSE(AudioReconnect::isAddress(""));
	EXPECT_FALSE(AudioReconnect::isAddress("00:1a:7d:da:71:1"));
	EXPECT_FALSE(AudioReconnect::isAddress("00:1a:7d:da:71:133"));
	EXPECT_FALSE(AudioReconnect::isAddress("00-1a-7d-da-71-13"));
	EXPECT_FALSE(AudioReconnect::isAddress("00:1a:7d:da:71:1g"));
	EXPECT_FALSE(AudioReconnect::isAddress("001a:7d:da:71:13:"));
}

TEST(AudioReconnect, NoAddressIsNotRemembered)
{
	AudioReconnect r;
	EXPECT_FALSE(r.connected(kAdapter, "nura", AudioReconnect::A2DP));
	EXPECT_FALSE(r.connected("hci0", kNura, AudioReconnect::HFP));
	EXPECT_EQ(r.toText(), switchLine(true));
}

TEST(AudioReconnect, ReadingBackReplacesWhatWasThere)
{
	AudioReconnect r;
	r.connected(kAdapter, kNura, AudioReconnect::A2DP);
	r.fromText(std::string(kOther) + " " + kSpeaker + " a2dp\n");
	EXPECT_TRUE(r.target(kAdapter).empty());
	EXPECT_EQ(r.target(kOther).address, kSpeaker);
}
