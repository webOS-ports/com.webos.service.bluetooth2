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

#include "fakesil.h"

namespace gluesil {

const char ADAPTER[] = "00:11:22:33:44:00";
Sil *current = nullptr;

namespace {

BluetoothPropertiesList connectedProperty(bool connected)
{
	BluetoothPropertiesList properties;
	properties.push_back(BluetoothProperty(BluetoothProperty::Type::CONNECTED, connected));
	return properties;
}

}

void Profile::getProperties(const std::string &address, BluetoothPropertiesResultCallback callback)
{
	callback(BLUETOOTH_ERROR_NONE, connectedProperty(mConnected.count(address) != 0));
}

void Profile::getProperty(const std::string &address, BluetoothProperty::Type type, BluetoothPropertyResultCallback callback)
{
	BluetoothProperty property(type);
	if (type != BluetoothProperty::Type::CONNECTED)
	{
		callback(BLUETOOTH_ERROR_PARAM_INVALID, property);
		return;
	}
	property.setValue<bool>(mConnected.count(address) != 0);
	callback(BLUETOOTH_ERROR_NONE, property);
}

void Profile::connect(const std::string &address, BluetoothResultCallback callback)
{
	connects.push_back(address);
	switch (connectAnswer)
	{
	case Answer::OK:
		remote(address, true);
		callback(BLUETOOTH_ERROR_NONE);
		break;
	case Answer::FAIL:
		// As bluez5 answers a page that timed out, or a service the remote does not offer.
		callback(BLUETOOTH_ERROR_FAIL);
		break;
	case Answer::ALREADY_CONNECTED:
		callback(BLUETOOTH_ERROR_DEVICE_ALREADY_CONNECTED);
		break;
	case Answer::DEFER:
		mDeferred.emplace_back(address, callback);
		break;
	}
}

void Profile::completeConnect(bool ok)
{
	if (mDeferred.empty())
		return;
	auto [address, callback] = mDeferred.front();
	mDeferred.pop_front();
	if (ok)
		remote(address, true);
	callback(ok ? BLUETOOTH_ERROR_NONE : BLUETOOTH_ERROR_FAIL);
}

void Profile::disconnect(const std::string &address, BluetoothResultCallback callback)
{
	disconnects.push_back(address);
	const BluetoothError answer = disconnectFails ? BLUETOOTH_ERROR_FAIL : BLUETOOTH_ERROR_NONE;
	if (connectedBeforeDisconnectAnswer)
	{
		remote(address, false);
		callback(answer);
	}
	else
	{
		callback(answer);
		remote(address, false);
	}
}

void Profile::remote(const std::string &address, bool connected)
{
	if (connected)
		mConnected.insert(address);
	else if (!mConnected.erase(address))
		return;
	if (BluetoothProfile::getObserver())
		BluetoothProfile::getObserver()->propertiesChanged(ADAPTER, address, connectedProperty(connected));
}

Adapter::Adapter() = default;

void Adapter::getAdapterProperties(BluetoothPropertiesResultCallback callback)
{
	BluetoothPropertiesList properties;
	properties.push_back(BluetoothProperty(BluetoothProperty::Type::BDADDR, std::string(ADAPTER)));
	properties.push_back(BluetoothProperty(BluetoothProperty::Type::NAME, std::string("glue")));
	properties.push_back(BluetoothProperty(BluetoothProperty::Type::ALIAS, std::string("glue")));
	properties.push_back(BluetoothProperty(BluetoothProperty::Type::INTERFACE_NAME, std::string("hci0")));
	properties.push_back(BluetoothProperty(BluetoothProperty::Type::STACK_NAME, std::string("fake")));
	properties.push_back(BluetoothProperty(BluetoothProperty::Type::STACK_VERSION, std::string("1")));
	properties.push_back(BluetoothProperty(BluetoothProperty::Type::FIRMWARE_VERSION, std::string("1")));
	properties.push_back(BluetoothProperty(BluetoothProperty::Type::PAIRABLE, true));
	properties.push_back(BluetoothProperty(BluetoothProperty::Type::DISCOVERABLE, false));
	callback(BLUETOOTH_ERROR_NONE, properties);
}

void Adapter::getAdapterProperty(BluetoothProperty::Type type, BluetoothPropertyResultCallback callback)
{
	if (type == BluetoothProperty::Type::BDADDR)
	{
		callback(BLUETOOTH_ERROR_NONE, BluetoothProperty(type, std::string(ADAPTER)));
		return;
	}
	callback(BLUETOOTH_ERROR_NONE, BluetoothProperty(type));
}

BluetoothError Adapter::enable()
{
	power(true);
	return BLUETOOTH_ERROR_NONE;
}

BluetoothError Adapter::disable()
{
	power(false);
	return BLUETOOTH_ERROR_NONE;
}

void Adapter::power(bool on)
{
	if (on == mPowered)
		return;
	mPowered = on;
	if (!on)
	{
		for (const auto &device : mDevices)
		{
			a2dp.remote(device.first, false);
			hfp.remote(device.first, false);
		}
	}
	getObserver()->adapterStateChanged(on);
}

void Adapter::getDeviceProperties(const std::string &address, BluetoothPropertiesResultCallback callback)
{
	auto device = mDevices.find(address);
	if (device == mDevices.end())
		callback(BLUETOOTH_ERROR_PARAM_INVALID, BluetoothPropertiesList());
	else
		callback(BLUETOOTH_ERROR_NONE, device->second);
}

BluetoothProfile *Adapter::getProfile(const std::string &profileId)
{
	if (profileId == "A2DP")
		return static_cast<Profile *>(&a2dp);
	if (profileId == "HFP")
		return static_cast<Profile *>(&hfp);
	return nullptr;
}

void Adapter::addDevice(const std::string &address, bool paired, const std::vector<std::string> &uuids)
{
	BluetoothPropertiesList properties;
	properties.push_back(BluetoothProperty(BluetoothProperty::Type::BDADDR, address));
	properties.push_back(BluetoothProperty(BluetoothProperty::Type::NAME, std::string("Headset ") + address.substr(address.size() - 2)));
	properties.push_back(BluetoothProperty(BluetoothProperty::Type::PAIRED, paired));
	properties.push_back(BluetoothProperty(BluetoothProperty::Type::CLASS_OF_DEVICE, static_cast<uint32_t>(0x240404)));
	properties.push_back(BluetoothProperty(BluetoothProperty::Type::TYPE_OF_DEVICE, static_cast<uint32_t>(BLUETOOTH_DEVICE_TYPE_BREDR)));
	properties.push_back(BluetoothProperty(BluetoothProperty::Type::UUIDS, uuids));
	mDevices[address] = properties;
	getObserver()->deviceFound(address, properties);
}

void Adapter::setPaired(const std::string &address, bool paired)
{
	BluetoothPropertiesList properties;
	properties.push_back(BluetoothProperty(BluetoothProperty::Type::PAIRED, paired));
	getObserver()->devicePropertiesChanged(address, properties);
}

}

extern "C" BluetoothSIL *glueTestCreateSIL(int version, BluetoothPairingIOCapability)
{
	if (version != BLUETOOTH_SIL_API_VERSION)
		return nullptr;
	gluesil::current = new gluesil::Sil;
	return gluesil::current;
}
