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

// The headset to connect again when an adapter comes up: the one whose A2DP
// connected last on that adapter, remembered across restarts.

#include <gtest/gtest.h>

#include "audioreconnect.h"

namespace {
const char kAdapter[] = "00:1a:7d:da:71:13";
const char kOther[] = "00:1a:7d:da:71:14";
const char kNura[] = "aa:bb:cc:dd:ee:01";
const char kSpeaker[] = "aa:bb:cc:dd:ee:02";
}

TEST(AudioReconnect, NothingConnectedYetIsNothingToReconnect)
{
	AudioReconnect r;
	EXPECT_EQ(r.target(kAdapter), "");
}

TEST(AudioReconnect, TheDeviceConnectedLastIsTheOne)
{
	AudioReconnect r;
	EXPECT_TRUE(r.connected(kAdapter, kNura));
	EXPECT_EQ(r.target(kAdapter), kNura);
	EXPECT_TRUE(r.connected(kAdapter, kSpeaker));
	EXPECT_EQ(r.target(kAdapter), kSpeaker);
}

TEST(AudioReconnect, ConnectingTheSameAgainNeedsNoStoring)
{
	AudioReconnect r;
	EXPECT_TRUE(r.connected(kAdapter, kNura));
	EXPECT_FALSE(r.connected(kAdapter, "AA:BB:CC:DD:EE:01"));
}

TEST(AudioReconnect, AddressesAreTakenInEitherCase)
{
	AudioReconnect r;
	r.connected("00:1A:7D:DA:71:13", "AA:BB:CC:DD:EE:01");
	EXPECT_EQ(r.target(kAdapter), kNura);
}

TEST(AudioReconnect, EachAdapterHasItsOwn)
{
	AudioReconnect r;
	r.connected(kAdapter, kNura);
	r.connected(kOther, kSpeaker);
	EXPECT_EQ(r.target(kAdapter), kNura);
	EXPECT_EQ(r.target(kOther), kSpeaker);
}

TEST(AudioReconnect, ADeviceNoLongerPairedIsForgotten)
{
	AudioReconnect r;
	r.connected(kAdapter, kNura);
	EXPECT_FALSE(r.forget(kAdapter, kSpeaker));
	EXPECT_EQ(r.target(kAdapter), kNura);
	EXPECT_TRUE(r.forget(kAdapter, kNura));
	EXPECT_EQ(r.target(kAdapter), "");
}

TEST(AudioReconnect, WhatIsRememberedSurvivesARestart)
{
	AudioReconnect before;
	before.connected(kAdapter, kNura);
	before.connected(kOther, kSpeaker);
	AudioReconnect after;
	after.fromText(before.toText());
	EXPECT_EQ(after.target(kAdapter), kNura);
	EXPECT_EQ(after.target(kOther), kSpeaker);
}

TEST(AudioReconnect, AFileThatIsNotOursIsIgnored)
{
	AudioReconnect r;
	r.connected(kAdapter, kSpeaker);
	r.fromText(std::string("garbage\n") + kAdapter + " not-an-address\n" +
	           "../../etc " + kNura + "\n" + kOther + " " + kNura + " extra\n" +
	           std::string(kAdapter) + " " + kNura + "\n");
	EXPECT_EQ(r.target(kAdapter), kNura);
	EXPECT_EQ(r.target(kOther), "");
	EXPECT_EQ(r.toText(), std::string(kAdapter) + " " + kNura + "\n");
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
	EXPECT_FALSE(r.connected(kAdapter, "nura"));
	EXPECT_FALSE(r.connected("hci0", kNura));
	EXPECT_EQ(r.toText(), "");
}

TEST(AudioReconnect, ReadingBackReplacesWhatWasThere)
{
	AudioReconnect r;
	r.connected(kAdapter, kNura);
	r.fromText(std::string(kOther) + " " + kSpeaker + "\n");
	EXPECT_EQ(r.target(kAdapter), "");
	EXPECT_EQ(r.target(kOther), kSpeaker);
}
