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

#ifndef BLUETOOTHAUDIORECONNECT_H
#define BLUETOOTHAUDIORECONNECT_H

#include <map>
#include <memory>
#include <string>

#include <glib.h>
#include <luna-service2/lunaservice.hpp>

#include "audioreconnect.h"

class BluetoothManagerService;
class BluetoothProfileService;

/*
 * Connects the audio device used last again when an adapter is powered on
 * (audioreconnect.h says what is remembered and why), and the Luna methods
 * for it:
 *
 *   /autoReconnect/getStatus  {"subscribe": bool}
 *       -> {"enabled": bool, "subscribed": bool}
 *   /autoReconnect/setState   {"enabled": bool}
 *       the Bluetooth settings' switch; kept across restarts.
 *   /autoReconnect/forget     {"address": string, "adapterAddress": string}
 *       the user disconnected that device somewhere this service does not
 *       see (Settings and the system menu disconnect through BlueZ itself):
 *       do not bring it back at the next power-on. Without adapterAddress,
 *       on every adapter.
 *
 * A disconnect through this service's own /a2dp/disconnect or
 * /hfp/disconnect is one by the user already, and forgets the device the
 * same way.
 */
class BluetoothAudioReconnect
{
public:
	explicit BluetoothAudioReconnect(BluetoothManagerService *manager);
	~BluetoothAudioReconnect();

	BluetoothAudioReconnect(const BluetoothAudioReconnect &) = delete;
	BluetoothAudioReconnect &operator=(const BluetoothAudioReconnect &) = delete;

	//! The profile service called profile connected or disconnected address.
	//! byUser: the disconnect was asked for through this service.
	void profileConnectionChanged(const std::string &profile, const std::string &adapterAddress,
	                              const std::string &address, bool connected, bool byUser);

	//! The adapter has been powered on: connect the device it had last, once
	//! the adapter and the device are ready for it.
	void adapterPowered(const std::string &adapterAddress);

	bool getStatus(LSMessage &message);
	bool setState(LSMessage &message);
	bool forget(LSMessage &message);

private:
	struct Timer
	{
		std::weak_ptr<BluetoothAudioReconnect *> self;
		std::string adapterAddress;
		unsigned int attempt;
	};
	static gboolean handleTimeout(gpointer data);
	static void freeTimer(gpointer data);

	void schedule(const std::string &adapterAddress, unsigned int attempt);
	void reconnect(const std::string &adapterAddress, unsigned int attempt);
	//! Those of one adapter, or of all.
	void cancelTimers(const std::string *adapterAddress = nullptr);
	BluetoothProfileService *findProfile(AudioReconnect::Profile profile) const;
	void store();
	void notifyStatus();

	BluetoothManagerService *mManager;
	AudioReconnect mReconnect;
	std::map<guint, std::string> mTimers;	// source id -> adapter
	LS::SubscriptionPoint mStatusSubscriptions;
	// Handed to SIL callbacks and timers, which may outlive this.
	std::shared_ptr<BluetoothAudioReconnect *> mAlive;
};

#endif // BLUETOOTHAUDIORECONNECT_H
