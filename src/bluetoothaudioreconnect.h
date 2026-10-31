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

#include <functional>
#include <map>
#include <memory>
#include <string>

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
 * same way, also when only that profile goes down. A connect remembers the
 * device with every audio profile it has connected then.
 */
class BluetoothAudioReconnect
{
public:
	//! Where the attempts wait: GLib's main loop, or a test's clock.
	class Timers
	{
	public:
		virtual ~Timers() = default;
		//! fire is called once, after seconds, with the id add() gave.
		virtual unsigned int add(unsigned int seconds, std::function<void(unsigned int id)> fire) = 0;
		//! Before it fired: it never does.
		virtual void remove(unsigned int id) = 0;
	};

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

	//! Waits with timers from now on, instead of the main loop's; the
	//! attempts waiting now are dropped.
	void useTimers(std::unique_ptr<Timers> timers);

	bool getStatus(LSMessage &message);
	bool setState(LSMessage &message);
	bool forget(LSMessage &message);

private:
	void schedule(const std::string &adapterAddress, unsigned int attempt);
	void reconnect(const std::string &adapterAddress, unsigned int attempt);
	//! Those of one adapter, or of all.
	void cancelTimers(const std::string *adapterAddress = nullptr);
	BluetoothProfileService *findProfile(AudioReconnect::Profile profile) const;
	void store();
	void notifyStatus();

	BluetoothManagerService *mManager;
	AudioReconnect mReconnect;
	std::unique_ptr<Timers> mClock;
	std::map<unsigned int, std::string> mTimers;	// timer id -> adapter
	LS::SubscriptionPoint mStatusSubscriptions;
	// Handed to SIL callbacks and timers, which may outlive this.
	std::shared_ptr<BluetoothAudioReconnect *> mAlive;
};

#endif // BLUETOOTHAUDIORECONNECT_H
