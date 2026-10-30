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

/*
 * Which Bluetooth audio device to connect again when an adapter comes up,
 * kept apart from the bus and the SIL so it can be tested.
 *
 * Nothing brought a headset back after bluetoothd, the service or the chip
 * restarted: BlueZ reconnects only after a link timeout or a suspend
 * (plugins/policy.c), and keeps that in memory. Android's stack does it on
 * every adapter power-on: PhonePolicy::autoConnect() connects the A2DP
 * device connected last. This does the same, per adapter: the device whose
 * A2DP connected last is remembered, also across restarts (the text form
 * below goes to a file), and connected again when the adapter is powered.
 *
 * An address is kept in lower case; anything that is not an adapter and a
 * device address is ignored when reading the text back.
 */
class AudioReconnect
{
public:
	//! A2DP to address on adapter connected. True if that changed what is
	//! remembered (and so has to be stored).
	bool connected(const std::string &adapter, const std::string &address);

	//! Forget the device, e.g. because it is no longer paired. True if it
	//! was remembered.
	bool forget(const std::string &adapter, const std::string &address);

	//! The device to connect on adapter, or "".
	std::string target(const std::string &adapter) const;

	//! One "<adapter> <device>" line per adapter.
	std::string toText() const;
	//! Replaces what is remembered with what text says.
	void fromText(const std::string &text);

	static bool isAddress(const std::string &text);

private:
	std::map<std::string, std::string> mLast;
};

#endif // AUDIORECONNECT_H
