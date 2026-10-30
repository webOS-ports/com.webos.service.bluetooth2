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

// A SIL in the test process, for the audio reconnect tests: one adapter, an A2DP and an HFP profile, and the
// devices a test adds. The service loads it through its SIL factory, as it loads bluez5: glue-sil.so, built
// next to the test, hands createBluetoothSIL() on to glueTestCreateSIL() here.

#ifndef GLUE_FAKESIL_H
#define GLUE_FAKESIL_H

#include <bluetooth-sil-api.h>

#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace gluesil {

extern const char ADAPTER[];

// What a profile answers a connect or a disconnect with.
enum class Answer { OK, FAIL, ALREADY_CONNECTED, DEFER };

class Profile : public BluetoothProfile
{
public:
	explicit Profile(std::string name) : mName(std::move(name)) { }

	void getProperties(const std::string &address, BluetoothPropertiesResultCallback callback) override;
	void getProperty(const std::string &address, BluetoothProperty::Type type, BluetoothPropertyResultCallback callback) override;
	void connect(const std::string &address, BluetoothResultCallback callback) override;
	void disconnect(const std::string &address, BluetoothResultCallback callback) override;

	//! The remote side connected or went away; the profile says so, as bluez5 does.
	void remote(const std::string &address, bool connected);

	//! A deferred connect answered now.
	void completeConnect(bool ok);

	Answer connectAnswer = Answer::OK;
	//! A disconnect that drops the link, and still answers with an error (a D-Bus timeout, say).
	bool disconnectFails = false;
	//! bluez5 reports CONNECTED false before or after the disconnect's answer, depending on D-Bus timing.
	bool connectedBeforeDisconnectAnswer = true;
	std::vector<std::string> connects;
	std::vector<std::string> disconnects;

private:
	std::string mName;
	std::set<std::string> mConnected;
	std::deque<std::pair<std::string, BluetoothResultCallback>> mDeferred;
};

class A2dp : public Profile, public BluetoothA2dpProfile
{
public:
	A2dp() : Profile("A2DP") { }
	using Profile::connect;
	using Profile::disconnect;
	BluetoothError startStreaming(const std::string &) override { return BLUETOOTH_ERROR_NONE; }
	BluetoothError stopStreaming(const std::string &) override { return BLUETOOTH_ERROR_NONE; }
};

class Hfp : public Profile, public BluetoothHfpProfile
{
public:
	Hfp() : Profile("HFP") { }
	using Profile::connect;
	using Profile::disconnect;
	void openSCO(const std::string &, BluetoothResultCallback callback) override { callback(BLUETOOTH_ERROR_NONE); }
	void closeSCO(const std::string &, BluetoothResultCallback callback) override { callback(BLUETOOTH_ERROR_NONE); }
	BluetoothError sendResultCode(const std::string &, const std::string &) override { return BLUETOOTH_ERROR_NONE; }
	BluetoothError sendAtCommand(const std::string &, const BluetoothHfpAtCommand &) override { return BLUETOOTH_ERROR_NONE; }
};

class Adapter : public BluetoothAdapter
{
public:
	Adapter();

	A2dp a2dp;
	Hfp hfp;

	void getAdapterProperties(BluetoothPropertiesResultCallback callback) override;
	void getAdapterProperty(BluetoothProperty::Type type, BluetoothPropertyResultCallback callback) override;
	void setAdapterProperty(const BluetoothProperty &, BluetoothResultCallback callback) override { callback(BLUETOOTH_ERROR_NONE); }
	void setAdapterProperties(const BluetoothPropertiesList &, BluetoothResultCallback callback) override { callback(BLUETOOTH_ERROR_NONE); }
	BluetoothError enable() override;
	BluetoothError disable() override;
	BluetoothError startDiscovery() override { return BLUETOOTH_ERROR_NONE; }
	void cancelDiscovery(BluetoothResultCallback callback) override { callback(BLUETOOTH_ERROR_NONE); }
	void getDeviceProperties(const std::string &address, BluetoothPropertiesResultCallback callback) override;
	void setDeviceProperty(const std::string &, const BluetoothProperty &, BluetoothResultCallback callback) override { callback(BLUETOOTH_ERROR_NONE); }
	void setDeviceProperties(const std::string &, const BluetoothPropertiesList &, BluetoothResultCallback callback) override { callback(BLUETOOTH_ERROR_NONE); }
	void pair(const std::string &, BluetoothResultCallback callback) override { callback(BLUETOOTH_ERROR_FAIL); }
	BluetoothError supplyPairingConfirmation(const std::string &, bool) override { return BLUETOOTH_ERROR_NONE; }
	BluetoothError supplyPairingSecret(const std::string &, const std::string &) override { return BLUETOOTH_ERROR_NONE; }
	BluetoothError supplyPairingSecret(const std::string &, BluetoothPasskey) override { return BLUETOOTH_ERROR_NONE; }
	void unpair(const std::string &, BluetoothResultCallback callback) override { callback(BLUETOOTH_ERROR_FAIL); }
	void cancelPairing(const std::string &, BluetoothResultCallback callback) override { callback(BLUETOOTH_ERROR_NONE); }
	void startAdvertising(BluetoothResultCallback callback) override { callback(BLUETOOTH_ERROR_NONE); }
	void stopAdvertising(BluetoothResultCallback callback) override { callback(BLUETOOTH_ERROR_NONE); }
	BluetoothError updateFirmware(const std::string &, const std::string &, const std::string &, bool) override { return BLUETOOTH_ERROR_UNSUPPORTED; }
	BluetoothError resetModule(const std::string &, bool) override { return BLUETOOTH_ERROR_UNSUPPORTED; }
	BluetoothProfile *getProfile(const std::string &profileId) override;

	//! The adapter switched on or off, as bluez5 reports Powered. Switching off ends every connection,
	//! as the remote side going away.
	void power(bool on);

	//! A device the adapter knows, paired or not, offering uuids.
	void addDevice(const std::string &address, bool paired, const std::vector<std::string> &uuids);
	void setPaired(const std::string &address, bool paired);

private:
	std::map<std::string, BluetoothPropertiesList> mDevices;
	bool mPowered = false;
};

class Sil : public BluetoothSIL
{
public:
	BluetoothAdapter *getDefaultAdapter() override { return &adapter; }
	std::vector<BluetoothAdapter *> getAdapters() override { return { &adapter }; }

	//! The adapter appears, as bluez5 tells the service once it found it.
	void start() { observer->adaptersChanged(); }

	Adapter adapter;
};

//! The SIL the service created last; it frees it itself.
extern Sil *current;

}

#endif
