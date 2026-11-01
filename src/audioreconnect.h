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

#ifndef AUDIORECONNECT_H
#define AUDIORECONNECT_H

#include <map>
#include <string>
#include <vector>

/*
 * Which Bluetooth audio device to connect again when an adapter comes up,
 * kept apart from the bus and the SIL so it can be tested.
 *
 * Nothing brought a headset back after bluetoothd, the service or the chip
 * restarted: BlueZ reconnects only after a link timeout or a suspend
 * (plugins/policy.c), and keeps that in memory. Android's stack does it on
 * every adapter power-on: PhonePolicy::autoConnect() connects the A2DP and
 * HFP device connected last. This does the same, per adapter: the audio
 * device whose A2DP or HFP connected last is remembered, with the profiles
 * it had, also across restarts (the text form below goes to a file), and
 * connected again when the adapter is powered.
 *
 * As on Android (PhonePolicy, DatabaseManager::setDisconnection), the user
 * disconnecting the device forgets it, so it is not brought back at the next
 * power-on; a link loss, or the device going away by itself, keeps it.
 * BlueZ reconnects after a link loss on its own (policy.c), so that is left
 * to BlueZ. A disconnect of one of its profiles forgets the whole device,
 * also while the other stays up: there is no per-profile setting here, as
 * Android's CONNECTION_POLICY, to say only that profile is not wanted. A
 * connect afterwards remembers it with every profile it has up then.
 *
 * One switch, on unless the user turned it off, turns connecting again off
 * altogether. What is remembered is still kept up to date while it is off,
 * so turning it back on needs no new connect first.
 *
 * An address is kept in lower case; anything that is not understood is
 * ignored when reading the text back.
 */
class AudioReconnect
{
public:
	enum Profile : unsigned
	{
		A2DP = 1U << 0,
		HFP = 1U << 1,
	};

	enum class Disconnect
	{
		ByUser,		//!< asked for here: by the user, or by an application for them
		ByRemote,	//!< the device went away, or the link was lost
	};

	struct Target
	{
		std::string address;
		unsigned profiles = 0;

		bool empty() const { return address.empty(); }
		//! The profile to connect: A2DP when the device had it, as BlueZ
		//! then connects the device's HFP too (policy.c sink_cb).
		Profile first() const { return (profiles & A2DP) != 0U ? A2DP : HFP; }
	};

	//! "A2DP" and "HFP", the names of their profile services.
	static bool profileFromName(const std::string &name, Profile &profile);

	//! True if a device offering uuids is one to connect profile to again:
	//! an A2DP sink (headphones, a speaker), or a hands-free unit (a headset,
	//! a car kit) - not a phone, which offers the other ends.
	static bool isAudioDevice(Profile profile, const std::vector<std::string> &uuids);

	static bool isAddress(const std::string &text);

	//! profiles (Profile bits) of address on adapter are connected: the one
	//! that connected now, and those it had up already. True if that changed
	//! what is remembered (and so has to be stored).
	bool connected(const std::string &adapter, const std::string &address, unsigned profiles);

	//! address on adapter disconnected. A disconnect by the user forgets the
	//! device, and gives true if it was remembered; one by the remote side
	//! keeps it.
	bool disconnected(const std::string &adapter, const std::string &address, Disconnect how);

	//! Forget the device, e.g. because it is no longer paired. True if it
	//! was remembered.
	bool forget(const std::string &adapter, const std::string &address);
	//! The same, on every adapter.
	bool forget(const std::string &address);

	//! The device to connect on adapter; empty when there is none, or when
	//! connecting again is switched off.
	Target target(const std::string &adapter) const;

	bool enabled() const { return mEnabled; }
	//! True if that changed it.
	bool setEnabled(bool enabled);

	//! The switch, then one "<adapter> <device> <profile>..." line per adapter.
	std::string toText() const;
	//! Replaces what is remembered, and the switch, with what text says.
	void fromText(const std::string &text);

	//! Read from and written to a file, written whole through a temporary
	//! file renamed over it. A missing file is nothing remembered, switched
	//! on. False, with error set, if storing failed.
	void load(const std::string &path);
	bool store(const std::string &path, std::string &error) const;

private:
	std::map<std::string, Target> mLast;
	bool mEnabled = true;
};

#endif // AUDIORECONNECT_H
