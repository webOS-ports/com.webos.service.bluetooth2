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

#include "bluetoothaudioreconnect.h"

#include <iterator>

#include <glib.h>

#include "bluetoothdevice.h"
#include "bluetootherrors.h"
#include "bluetoothmanagerservice.h"
#include "bluetoothprofileservice.h"
#include "logging.h"
#include "ls2utils.h"

namespace {

// Next to adaptersAssignment.json; bluetoothd only reads the directories.
// The unit tests keep theirs in their working directory.
#ifndef AUDIO_RECONNECT_FILE
#define AUDIO_RECONNECT_FILE "/var/lib/bluetooth/audioReconnect.txt"
#endif
const char kReconnectFile[] = AUDIO_RECONNECT_FILE;

// Seconds before each attempt, counted from the adapter coming up or the
// attempt before failing. The first waits for the adapter's devices and
// profiles to be there, as BlueZ waits 2 s before reconnecting on resume;
// a headset still switching itself back on gets two more chances.
const guint kDelays[] = { 2, 10, 30 };
const unsigned int kAttempts = std::size(kDelays);

const char *profileName(AudioReconnect::Profile profile)
{
	return profile == AudioReconnect::A2DP ? "A2DP" : "HFP";
}

class MainLoopTimers : public BluetoothAudioReconnect::Timers
{
public:
	unsigned int add(unsigned int seconds, std::function<void(unsigned int id)> fire) override
	{
		return g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, seconds, &MainLoopTimers::handleTimeout,
		                                  new std::function<void(unsigned int)>(std::move(fire)),
		                                  &MainLoopTimers::freeTimer);
	}

	// Removing a source frees its timer; its handler is not called.
	void remove(unsigned int id) override { g_source_remove(id); }

private:
	static gboolean handleTimeout(gpointer data)
	{
		(*static_cast<std::function<void(unsigned int)> *>(data))(g_source_get_id(g_main_current_source()));
		return G_SOURCE_REMOVE;
	}

	static void freeTimer(gpointer data)
	{
		delete static_cast<std::function<void(unsigned int)> *>(data);
	}
};

}

BluetoothAudioReconnect::BluetoothAudioReconnect(BluetoothManagerService *manager) :
	mManager(manager),
	mClock(std::make_unique<MainLoopTimers>()),
	mAlive(std::make_shared<BluetoothAudioReconnect *>(this))
{
	LS_CREATE_CATEGORY_BEGIN(BluetoothAudioReconnect, autoReconnect)
		LS_CATEGORY_CLASS_METHOD(BluetoothAudioReconnect, getStatus)
		LS_CATEGORY_CLASS_METHOD(BluetoothAudioReconnect, setState)
		LS_CATEGORY_CLASS_METHOD(BluetoothAudioReconnect, forget)
	LS_CREATE_CATEGORY_END

	manager->registerCategory("/autoReconnect", LS_CATEGORY_TABLE_NAME(autoReconnect), nullptr, nullptr);
	manager->setCategoryData("/autoReconnect", this);
	mStatusSubscriptions.setServiceHandle(manager);

	mReconnect.load(kReconnectFile);
}

BluetoothAudioReconnect::~BluetoothAudioReconnect()
{
	cancelTimers();
}

void BluetoothAudioReconnect::useTimers(std::unique_ptr<Timers> timers)
{
	cancelTimers();
	mClock = std::move(timers);
}

void BluetoothAudioReconnect::cancelTimers(const std::string *adapterAddress)
{
	for (auto it = mTimers.begin(); it != mTimers.end();)
	{
		if (adapterAddress && it->second != *adapterAddress)
		{
			++it;
			continue;
		}
		mClock->remove(it->first);
		it = mTimers.erase(it);
	}
}

void BluetoothAudioReconnect::store()
{
	std::string error;
	if (!mReconnect.store(kReconnectFile, error))
		BT_WARNING("AUDIO_RECONNECT", 0, "Failed to store %s: %s", kReconnectFile, error.c_str());
}

BluetoothProfileService *BluetoothAudioReconnect::findProfile(AudioReconnect::Profile profile) const
{
	const std::string name = profileName(profile);
	for (auto service : mManager->getProfiles())
	{
		if (service->getName() == name)
			return service;
	}
	return nullptr;
}

void BluetoothAudioReconnect::profileConnectionChanged(const std::string &profile, const std::string &adapterAddress,
                                                       const std::string &address, bool connected, bool byUser)
{
	AudioReconnect::Profile which = AudioReconnect::A2DP;
	if (!AudioReconnect::profileFromName(profile, which))
		return;

	if (!connected)
	{
		if (mReconnect.disconnected(adapterAddress, address,
		                            byUser ? AudioReconnect::Disconnect::ByUser : AudioReconnect::Disconnect::ByRemote))
		{
			BT_INFO("AUDIO_RECONNECT", 0, "%s disconnected by the user, not connecting it again", address.c_str());
			store();
		}
		return;
	}

	// The other end of A2DP or HFP - a phone this one is a headset for -
	// is not one to bring back.
	BluetoothDevice *device = mManager->findDevice(adapterAddress, address);
	if (!device || !AudioReconnect::isAudioDevice(which, device->getUuids()))
		return;
	if (mReconnect.connected(adapterAddress, address, which))
		store();
}

void BluetoothAudioReconnect::adapterPowered(const std::string &adapterAddress)
{
	// Powered again before the last attempts are done: start over.
	cancelTimers(&adapterAddress);
	if (mReconnect.target(adapterAddress).empty())
		return;
	schedule(adapterAddress, 0);
}

void BluetoothAudioReconnect::schedule(const std::string &adapterAddress, unsigned int attempt)
{
	if (attempt >= kAttempts)
	{
		BT_INFO("AUDIO_RECONNECT", 0, "Gave up connecting %s again", mReconnect.target(adapterAddress).address.c_str());
		return;
	}
	std::weak_ptr<BluetoothAudioReconnect *> weak = mAlive;
	const unsigned int id = mClock->add(kDelays[attempt], [weak, adapterAddress, attempt](unsigned int fired) {
		auto alive = weak.lock();
		if (!alive)
			return;
		BluetoothAudioReconnect *self = *alive;
		self->mTimers.erase(fired);
		self->reconnect(adapterAddress, attempt);
	});
	mTimers[id] = adapterAddress;
}

void BluetoothAudioReconnect::reconnect(const std::string &adapterAddress, unsigned int attempt)
{
	const AudioReconnect::Target target = mReconnect.target(adapterAddress);
	if (target.empty() || !mManager->getPowered(adapterAddress))
		return;

	// Connected or connecting already, by the device itself, by BlueZ or by
	// the user: nothing to do.
	for (AudioReconnect::Profile profile : { AudioReconnect::A2DP, AudioReconnect::HFP })
	{
		BluetoothProfileService *service = findProfile(profile);
		if (service && (service->isDeviceConnected(adapterAddress, target.address) ||
		                service->isDeviceConnecting(adapterAddress, target.address)))
			return;
	}

	BluetoothProfileService *service = findProfile(target.first());
	if (!service)
		return;		// not built in

	BluetoothDevice *device = mManager->findDevice(adapterAddress, target.address);
	if (!device)
	{
		// After a chip reset the adapter's devices come back after the
		// adapter itself.
		schedule(adapterAddress, attempt + 1);
		return;
	}
	if (!device->getPaired())
	{
		if (mReconnect.forget(adapterAddress, target.address))
			store();
		return;
	}

	BT_INFO("AUDIO_RECONNECT", 0, "Connecting %s of %s again (attempt %u)", profileName(target.first()),
	        target.address.c_str(), attempt + 1);
	std::weak_ptr<BluetoothAudioReconnect *> weak = mAlive;
	const std::string address = target.address;
	const bool started = service->connectAgain(adapterAddress, address,
	                                           [weak, adapterAddress, address, attempt](BluetoothError error) {
		auto alive = weak.lock();
		if (!alive || error == BLUETOOTH_ERROR_NONE)
			return;
		BT_INFO("AUDIO_RECONNECT", 0, "Connecting %s again failed: %d", address.c_str(), error);
		// Something else has it now, or the user connected it meanwhile.
		if (error != BLUETOOTH_ERROR_DEVICE_ALREADY_CONNECTED)
			(*alive)->schedule(adapterAddress, attempt + 1);
	});
	// The profile is not there yet for this adapter.
	if (!started)
		schedule(adapterAddress, attempt + 1);
}

void BluetoothAudioReconnect::notifyStatus()
{
	pbnjson::JValue responseObj = pbnjson::Object();
	responseObj.put("returnValue", true);
	responseObj.put("subscribed", true);
	responseObj.put("enabled", mReconnect.enabled());
	LSUtils::postToSubscriptionPoint(&mStatusSubscriptions, responseObj);
}

bool BluetoothAudioReconnect::getStatus(LSMessage &message)
{
	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, SCHEMA_1(PROP(subscribe, boolean)), &parseError))
	{
		if (parseError == JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);
		else
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);
		return true;
	}

	bool subscribed = false;
	if (request.isSubscription())
		subscribed = mStatusSubscriptions.subscribe(request);

	pbnjson::JValue responseObj = pbnjson::Object();
	responseObj.put("returnValue", true);
	responseObj.put("subscribed", subscribed);
	responseObj.put("enabled", mReconnect.enabled());
	LSUtils::postToClient(request, responseObj);
	return true;
}

bool BluetoothAudioReconnect::setState(LSMessage &message)
{
	BT_INFO("AUDIO_RECONNECT", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;
	const std::string schema = STRICT_SCHEMA(PROPS_1(PROP(enabled, boolean)) REQUIRED_1(enabled));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError == JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);
		else
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);
		return true;
	}

	if (mReconnect.setEnabled(requestObj["enabled"].asBool()))
	{
		BT_INFO("AUDIO_RECONNECT", 0, "Connecting audio devices again is now %s", mReconnect.enabled() ? "on" : "off");
		if (!mReconnect.enabled())
			cancelTimers();
		store();
		notifyStatus();
	}

	pbnjson::JValue responseObj = pbnjson::Object();
	responseObj.put("returnValue", true);
	responseObj.put("enabled", mReconnect.enabled());
	LSUtils::postToClient(request, responseObj);
	return true;
}

bool BluetoothAudioReconnect::forget(LSMessage &message)
{
	BT_INFO("AUDIO_RECONNECT", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;
	const std::string schema = STRICT_SCHEMA(PROPS_2(PROP(address, string), PROP(adapterAddress, string)) REQUIRED_1(address));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);
		else if (!requestObj.hasKey("address"))
			LSUtils::respondWithError(request, BT_ERR_ADDR_PARAM_MISSING);
		else
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);
		return true;
	}

	const std::string address = requestObj["address"].asString();
	if (!AudioReconnect::isAddress(address))
	{
		LSUtils::respondWithError(request, BT_ERR_DEVICE_NOT_AVAIL);
		return true;
	}

	bool forgotten = false;
	if (requestObj.hasKey("adapterAddress"))
	{
		const std::string adapterAddress = requestObj["adapterAddress"].asString();
		if (!AudioReconnect::isAddress(adapterAddress))
		{
			LSUtils::respondWithError(request, BT_ERR_INVALID_ADAPTER_ADDRESS);
			return true;
		}
		forgotten = mReconnect.forget(adapterAddress, address);
	}
	else
		forgotten = mReconnect.forget(address);

	if (forgotten)
	{
		BT_INFO("AUDIO_RECONNECT", 0, "%s disconnected by the user, not connecting it again", address.c_str());
		store();
	}

	pbnjson::JValue responseObj = pbnjson::Object();
	responseObj.put("returnValue", true);
	responseObj.put("address", address);
	LSUtils::postToClient(request, responseObj);
	return true;
}
