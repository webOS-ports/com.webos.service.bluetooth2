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

// A Bluetooth SIL with no Bluetooth behind it, for driving the real service through the paths that need a
// remote device: connects that fail, sessions the remote ends, transfers the service does not know.
//
// The service loads it instead of bluez5 with WEBOS_BLUETOOTH_SIL=fake and WEBOS_BLUETOOTH_SIL_BASE_PATH set to
// its directory. A test drives it through $FAKE_SIL_DIR (default /run/bt-fake-sil):
//
//   ctl    FIFO; one command per line (see runCommand())
//   calls  every call the service made into the SIL, one per line
//   acks   one line per command carried out, so a test can wait for it
//
// Every call answers at once with success unless "mode <op> fail" or "mode <op> defer" says otherwise; a deferred
// call is answered by "complete <op> ok|fail".

#include <bluetooth-sil-api.h>

#include <deque>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <glib.h>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

const std::string ADAPTER_ADDRESS = "00:11:22:33:44:00";

std::string controlDir()
{
	const char *dir = getenv("FAKE_SIL_DIR");
	return dir ? dir : "/run/bt-fake-sil";
}

void appendLine(const std::string &file, const std::string &line)
{
	std::ofstream out(controlDir() + "/" + file, std::ios::app);
	out << line << std::endl;
}

enum class Mode { OK, FAIL, DEFER };
std::map<std::string, Mode> modes;
std::map<std::string, std::deque<std::function<void(bool)>>> deferred;

// Logs the call, then answers it as its mode says.
void handle(const std::string &op, const std::string &args, std::function<void(bool)> answer)
{
	appendLine("calls", op + (args.empty() ? "" : " " + args));
	Mode mode = modes.count(op) ? modes[op] : Mode::OK;
	if (mode == Mode::DEFER)
		deferred[op].push_back(answer);
	else
		answer(mode == Mode::OK);
}

BluetoothError immediate(const std::string &op, const std::string &args)
{
	bool ok = true;
	handle(op, args, [&ok](bool result) { ok = result; });
	return ok ? BLUETOOTH_ERROR_NONE : BLUETOOTH_ERROR_FAIL;
}

BluetoothError result(bool ok)
{
	return ok ? BLUETOOTH_ERROR_NONE : BLUETOOTH_ERROR_FAIL;
}

BluetoothPropertiesList connectedProperty(bool connected)
{
	BluetoothPropertiesList properties;
	properties.push_back(BluetoothProperty(BluetoothProperty::Type::CONNECTED, connected));
	return properties;
}

// What every fake profile answers for the generic profile calls.
class FakeProfileBase : public BluetoothProfile
{
public:
	explicit FakeProfileBase(const std::string &name) : mName(name) { }

	void getProperties(const std::string &address, BluetoothPropertiesResultCallback callback) override
	{
		appendLine("calls", mName + ".getProperties " + address);
		callback(BLUETOOTH_ERROR_NONE, connectedProperty(mConnected.count(address) != 0));
	}

	// As bluez5 answers: CONNECTED always has its value, anything else is not a property of a profile.
	void getProperty(const std::string &address, BluetoothProperty::Type type, BluetoothPropertyResultCallback callback) override
	{
		appendLine("calls", mName + ".getProperty " + address);
		BluetoothProperty property(type);
		if (type != BluetoothProperty::Type::CONNECTED)
		{
			callback(BLUETOOTH_ERROR_PARAM_INVALID, property);
			return;
		}
		property.setValue<bool>(mConnected.count(address) != 0);
		callback(BLUETOOTH_ERROR_NONE, property);
	}

	void connect(const std::string &address, BluetoothResultCallback callback) override
	{
		handle(mName + ".connect", address, [this, address, callback](bool ok) {
			if (ok)
				notifyConnected(address, true);
			callback(result(ok));
		});
	}

	void disconnect(const std::string &address, BluetoothResultCallback callback) override
	{
		handle(mName + ".disconnect", address, [this, address, callback](bool ok) {
			if (ok)
				notifyConnected(address, false);
			callback(result(ok));
		});
	}

	// As bluez5 does for a session that starts or ends.
	void notifyConnected(const std::string &key, bool connected)
	{
		if (connected)
			mConnected.insert(key);
		else
			mConnected.erase(key);
		BluetoothProfile::getObserver()->propertiesChanged(ADAPTER_ADDRESS, key, connectedProperty(connected));
	}

protected:
	std::string mName;
	std::set<std::string> mConnected;
};

class FakeMap : public FakeProfileBase, public BluetoothMapProfile
{
public:
	FakeMap() : FakeProfileBase("map") { }

	using FakeProfileBase::connect;
	using FakeProfileBase::disconnect;

	void connect(const std::string &address, const std::string &instanceName, BluetoothMapCallback callback) override
	{
		handle("map.connect", address + " " + instanceName, [this, callback](bool ok) {
			callback(result(ok), ok ? "session" + std::to_string(++mSessions) : "");
		});
	}

	void disconnect(const std::string &sessionKey, const std::string &sessionId, BluetoothMapCallback callback) override
	{
		handle("map.disconnect", sessionKey + " " + sessionId, [sessionKey, callback](bool ok) {
			std::size_t pos = sessionKey.find('_');
			callback(result(ok), pos == std::string::npos ? "" : sessionKey.substr(pos + 1));
		});
	}

	void getFolderList(const std::string &sessionKey, const std::string &, const uint16_t &, const uint16_t &, BluetoothMapGetFoldersCallback callback) override
	{
		handle("map.getFolderList", sessionKey, [callback](bool ok) { std::vector<std::string> folders; callback(result(ok), folders); });
	}

	void getMessageFilters(const std::string &sessionKey, const std::string &, BluetoothMapListFiltersResultCallback callback) override
	{
		handle("map.getMessageFilters", sessionKey, [callback](bool ok) { callback(result(ok), std::list<std::string>()); });
	}

	void setFolder(const std::string &sessionKey, const std::string &, const std::string &folder, BluetoothResultCallback callback) override
	{
		handle("map.setFolder", sessionKey + " " + folder, [callback](bool ok) { callback(result(ok)); });
	}

	void getMessageList(const std::string &sessionKey, const std::string &, const std::string &folder, const BluetoothMapPropertiesList &, BluetoothMapGetMessageListCallback callback) override
	{
		handle("map.getMessageList", sessionKey + " " + folder, [callback](bool ok) { BluetoothMessageList list; callback(result(ok), list); });
	}

	void getMessage(const std::string &sessionKey, const std::string &messageHandle, bool, const std::string &destinationFile, BluetoothResultCallback callback) override
	{
		handle("map.getMessage", sessionKey + " " + messageHandle + " " + destinationFile, [callback](bool ok) { callback(result(ok)); });
	}

	void setMessageStatus(const std::string &sessionKey, const std::string &messageHandle, const std::string &statusIndicator, bool, BluetoothResultCallback callback) override
	{
		handle("map.setMessageStatus", sessionKey + " " + messageHandle + " " + statusIndicator, [callback](bool ok) { callback(result(ok)); });
	}

	void pushMessage(const std::string &sessionKey, const std::string &sourceFile, const std::string &folder, const std::string &, bool, bool, BluetoothMapCallback callback) override
	{
		handle("map.pushMessage", sessionKey + " " + sourceFile + " " + folder, [callback](bool ok) { callback(result(ok), ok ? "handle1" : ""); });
	}

	void notifyMessage(const std::string &sessionId)
	{
		BluetoothMessageList list;
		getMapObserver()->messageNotificationEvent(ADAPTER_ADDRESS, sessionId, list);
	}

private:
	int mSessions = 0;
};

class FakePbap : public FakeProfileBase, public BluetoothPbapProfile
{
public:
	FakePbap() : FakeProfileBase("pbap") { }

	std::string objectPath = "/org/bluez/obex/client/session0/transfer0";

	void supplyAccessConfirmation(BluetoothPbapAccessRequestId accessRequestId, bool accept, BluetoothResultCallback callback) override
	{
		handle("pbap.supplyAccessConfirmation", std::to_string(accessRequestId) + " " + std::to_string(accept), [callback](bool ok) { callback(result(ok)); });
	}

	void setPhoneBook(const std::string &address, const std::string &repository, const std::string &object, BluetoothResultCallback callback) override
	{
		handle("pbap.setPhoneBook", address + " " + repository + " " + object, [callback](bool ok) { callback(result(ok)); });
	}

	void getPhonebookSize(const std::string &address, BluetoothPbapGetSizeResultCallback callback) override
	{
		handle("pbap.getPhonebookSize", address, [callback](bool ok) { callback(result(ok), ok ? 3 : 0); });
	}

	void vCardListing(const std::string &address, BluetoothPbapVCardListResultCallback callback) override
	{
		handle("pbap.vCardListing", address, [callback](bool ok) { BluetoothPbapVCardList list; callback(result(ok), list); });
	}

	void getPhoneBookProperties(const std::string &address, BluetoothPbapApplicationParameterCallback callback) override
	{
		handle("pbap.getPhoneBookProperties", address, [callback](bool ok) { BluetoothPbapApplicationParameters params; callback(result(ok), params); });
	}

	void getvCardFilters(const std::string &address, BluetoothPbapListFiltersResultCallback callback) override
	{
		handle("pbap.getvCardFilters", address, [callback](bool ok) { callback(result(ok), std::list<std::string>()); });
	}

	void pullvCard(const std::string &address, const std::string &destinationFile, const std::string &vCardHandle, const std::string &, BluetoothPbapVCardFilterList &, BluetoothResultCallback callback) override
	{
		handle("pbap.pullvCard", address + " " + destinationFile + " " + vCardHandle, [callback](bool ok) { callback(result(ok)); });
	}

	void pullPhoneBook(const std::string &address, const std::string &destinationFile, const std::string &, BluetoothPbapVCardFilterList &, const uint16_t &, const uint16_t &, BluetoothGetPhoneBookResultCallback callback) override
	{
		handle("pbap.pullPhoneBook", address + " " + destinationFile, [this, callback](bool ok) { callback(result(ok), ok ? objectPath : ""); });
	}

	void searchPhoneBook(const std::string &address, const std::string &, const std::string &, const std::string &searchValue, BluetoothPbapVCardListResultCallback callback) override
	{
		handle("pbap.searchPhoneBook", address + " " + searchValue, [callback](bool ok) { BluetoothPbapVCardList list; callback(result(ok), list); });
	}

	void notifyTransfer(const std::string &address, const std::string &destination, const std::string &path, const std::string &state)
	{
		getPbapObserver()->transferStatusChanged(ADAPTER_ADDRESS, address, destination, path, state);
	}
};

class FakeOpp : public FakeProfileBase, public BluetoothOppProfile
{
public:
	FakeOpp() : FakeProfileBase("opp") { }

	BluetoothOppTransferId pushFile(const std::string &address, const std::string &sourcePath, BluetoothOppTransferResultCallback callback) override
	{
		BluetoothOppTransferId id = ++mNextId;
		handle("opp.pushFile", address + " " + sourcePath + " id=" + std::to_string(id), [callback](bool ok) { callback(result(ok), 0, 10, ok); });
		return id;
	}

	void cancelTransfer(BluetoothOppTransferId id, BluetoothResultCallback callback) override
	{
		handle("opp.cancelTransfer", std::to_string(id), [callback](bool ok) { callback(result(ok)); });
	}

	void supplyTransferConfirmation(BluetoothOppTransferId transferId, bool accept, BluetoothResultCallback callback) override
	{
		handle("opp.supplyTransferConfirmation", std::to_string(transferId) + " " + std::to_string(accept), [callback](bool ok) { callback(result(ok)); });
	}

	void requestTransfer(BluetoothOppTransferId id, const std::string &address, const std::string &file, uint64_t size)
	{
		getOppObserver()->transferConfirmationRequested(id, ADAPTER_ADDRESS, address, "Fake phone", file, size);
	}

	void transferState(BluetoothOppTransferId id, uint64_t transferred, bool finished)
	{
		getOppObserver()->transferStateChanged(id, transferred, finished);
	}

private:
	BluetoothOppTransferId mNextId = 100;
};

class FakeMesh : public FakeProfileBase, public BluetoothMeshProfile
{
public:
	FakeMesh() : FakeProfileBase("mesh") { }

	BluetoothError createNetwork(const std::string &bearer) override { return immediate("mesh.createNetwork", bearer); }
	BluetoothError attach(const std::string &bearer, const std::string &token) override { return immediate("mesh.attach", bearer + " " + token); }

	BluetoothError setOnOff(const std::string &bearer, uint16_t destAddress, uint16_t appKeyIndex, bool onoff, bool ack) override
	{
		return immediate("mesh.setOnOff", bearer + " " + std::to_string(destAddress) + " " + std::to_string(appKeyIndex) + " " + std::to_string(onoff) + " " + std::to_string(ack));
	}

	void onOffResult(bool state) { getMeshObserver()->modelSetOnOffResult(ADAPTER_ADDRESS, state, BLUETOOTH_ERROR_NONE); }
	void networkId(uint64_t id) { getMeshObserver()->updateNetworkId(ADAPTER_ADDRESS, id); }
};

class FakeAdapter : public BluetoothAdapter
{
public:
	FakeAdapter()
	{
		mProfiles["MAP"] = &mMap;
		mProfiles["PBAP"] = &mPbap;
		mProfiles["OPP"] = &mOpp;
		mProfiles["MESH"] = &mMesh;
	}

	FakeMap mMap;
	FakePbap mPbap;
	FakeOpp mOpp;
	FakeMesh mMesh;

	BluetoothPropertiesList adapterProperties()
	{
		BluetoothPropertiesList properties;
		properties.push_back(BluetoothProperty(BluetoothProperty::Type::BDADDR, ADAPTER_ADDRESS));
		properties.push_back(BluetoothProperty(BluetoothProperty::Type::NAME, std::string("fake")));
		properties.push_back(BluetoothProperty(BluetoothProperty::Type::ALIAS, std::string("fake")));
		properties.push_back(BluetoothProperty(BluetoothProperty::Type::INTERFACE_NAME, std::string("hci0")));
		properties.push_back(BluetoothProperty(BluetoothProperty::Type::STACK_NAME, std::string("fake")));
		properties.push_back(BluetoothProperty(BluetoothProperty::Type::STACK_VERSION, std::string("1")));
		properties.push_back(BluetoothProperty(BluetoothProperty::Type::FIRMWARE_VERSION, std::string("1")));
		properties.push_back(BluetoothProperty(BluetoothProperty::Type::PAIRABLE, true));
		properties.push_back(BluetoothProperty(BluetoothProperty::Type::DISCOVERABLE, false));
		return properties;
	}

	void getAdapterProperties(BluetoothPropertiesResultCallback callback) override
	{
		callback(BLUETOOTH_ERROR_NONE, adapterProperties());
	}

	void getAdapterProperty(BluetoothProperty::Type type, BluetoothPropertyResultCallback callback) override
	{
		for (auto &property : adapterProperties())
		{
			if (property.getType() == type)
			{
				callback(BLUETOOTH_ERROR_NONE, property);
				return;
			}
		}
		callback(BLUETOOTH_ERROR_NONE, BluetoothProperty(type));
	}

	void setAdapterProperty(const BluetoothProperty &, BluetoothResultCallback callback) override { callback(BLUETOOTH_ERROR_NONE); }
	void setAdapterProperties(const BluetoothPropertiesList &, BluetoothResultCallback callback) override { callback(BLUETOOTH_ERROR_NONE); }

	BluetoothError enable() override
	{
		getObserver()->adapterStateChanged(true);
		return BLUETOOTH_ERROR_NONE;
	}

	BluetoothError disable() override { return BLUETOOTH_ERROR_NONE; }
	BluetoothError startDiscovery() override { return immediate("adapter.startDiscovery", ""); }
	void cancelDiscovery(BluetoothResultCallback callback) override { callback(BLUETOOTH_ERROR_NONE); }

	void getDeviceProperties(const std::string &address, BluetoothPropertiesResultCallback callback) override
	{
		auto device = mDevices.find(address);
		if (device == mDevices.end())
			callback(BLUETOOTH_ERROR_PARAM_INVALID, BluetoothPropertiesList());
		else
			callback(BLUETOOTH_ERROR_NONE, device->second);
	}

	void setDeviceProperty(const std::string &, const BluetoothProperty &, BluetoothResultCallback callback) override { callback(BLUETOOTH_ERROR_NONE); }
	void setDeviceProperties(const std::string &, const BluetoothPropertiesList &, BluetoothResultCallback callback) override { callback(BLUETOOTH_ERROR_NONE); }
	void pair(const std::string &address, BluetoothResultCallback callback) override { handle("adapter.pair", address, [callback](bool ok) { callback(result(ok)); }); }
	BluetoothError supplyPairingConfirmation(const std::string &, bool) override { return BLUETOOTH_ERROR_NONE; }
	BluetoothError supplyPairingSecret(const std::string &, const std::string &) override { return BLUETOOTH_ERROR_NONE; }
	BluetoothError supplyPairingSecret(const std::string &, BluetoothPasskey) override { return BLUETOOTH_ERROR_NONE; }
	void unpair(const std::string &address, BluetoothResultCallback callback) override { handle("adapter.unpair", address, [callback](bool ok) { callback(result(ok)); }); }
	void cancelPairing(const std::string &, BluetoothResultCallback callback) override { callback(BLUETOOTH_ERROR_NONE); }
	void startAdvertising(BluetoothResultCallback callback) override { callback(BLUETOOTH_ERROR_NONE); }
	void stopAdvertising(BluetoothResultCallback callback) override { callback(BLUETOOTH_ERROR_NONE); }
	BluetoothError updateFirmware(const std::string &, const std::string &, const std::string &, bool) override { return BLUETOOTH_ERROR_UNSUPPORTED; }
	BluetoothError resetModule(const std::string &, bool) override { return BLUETOOTH_ERROR_UNSUPPORTED; }

	BluetoothProfile *getProfile(const std::string &profileId) override
	{
		auto profile = mProfiles.find(profileId);
		return profile == mProfiles.end() ? nullptr : profile->second;
	}

	// A paired device that offers MAP, PBAP and OPP, with one MAP instance.
	void addDevice(const std::string &address, bool paired, const std::string &mapInstance)
	{
		BluetoothPropertiesList properties;
		properties.push_back(BluetoothProperty(BluetoothProperty::Type::BDADDR, address));
		properties.push_back(BluetoothProperty(BluetoothProperty::Type::NAME, std::string("Fake phone ") + address.substr(address.size() - 2)));
		properties.push_back(BluetoothProperty(BluetoothProperty::Type::PAIRED, paired));
		properties.push_back(BluetoothProperty(BluetoothProperty::Type::CLASS_OF_DEVICE, (uint32_t)0x5a020c));
		properties.push_back(BluetoothProperty(BluetoothProperty::Type::TYPE_OF_DEVICE, (uint32_t)BLUETOOTH_DEVICE_TYPE_BREDR));
		properties.push_back(BluetoothProperty(BluetoothProperty::Type::UUIDS, std::vector<std::string>{
			"00001132-0000-1000-8000-00805f9b34fb", "0000112f-0000-1000-8000-00805f9b34fb", "00001105-0000-1000-8000-00805f9b34fb" }));
		if (!mapInstance.empty())
		{
			properties.push_back(BluetoothProperty(BluetoothProperty::Type::MAP_INSTANCES_NAME, std::vector<std::string>{ mapInstance }));
			properties.push_back(BluetoothProperty(BluetoothProperty::Type::MAP_SUPPORTED_MESSAGE_TYPE,
				std::map<std::string, std::vector<std::string>>{ { mapInstance, { "SMS_GSM" } } }));
		}
		mDevices[address] = properties;
		getObserver()->deviceFound(address, properties);
	}

private:
	std::map<std::string, BluetoothProfile *> mProfiles;
	std::map<std::string, BluetoothPropertiesList> mDevices;
};

class FakeSil : public BluetoothSIL
{
public:
	FakeSil()
	{
		std::string dir = controlDir();
		g_mkdir_with_parents(dir.c_str(), 0700);
		std::string fifo = dir + "/ctl";
		unlink(fifo.c_str());
		mkfifo(fifo.c_str(), 0600);
		// Read-write, so that the FIFO never reports end of file between writers.
		mFifo = open(fifo.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
		mChannel = g_io_channel_unix_new(mFifo);
		mWatch = g_io_add_watch(mChannel, G_IO_IN, &FakeSil::onControl, this);

		// The adapter appears once the service has registered its observer.
		mStartup = g_idle_add([](gpointer data) -> gboolean {
			auto *sil = static_cast<FakeSil *>(data);
			sil->mStartup = 0;
			if (sil->observer)
				sil->observer->adaptersChanged();
			return G_SOURCE_REMOVE;
		}, this);
	}

	~FakeSil() override
	{
		if (mStartup)
			g_source_remove(mStartup);
		g_source_remove(mWatch);
		g_io_channel_unref(mChannel);
		close(mFifo);
	}

	BluetoothAdapter *getDefaultAdapter() override { return &mAdapter; }
	std::vector<BluetoothAdapter *> getAdapters() override { return { &mAdapter }; }

private:
	static gboolean onControl(GIOChannel *channel, GIOCondition, gpointer data)
	{
		auto *sil = static_cast<FakeSil *>(data);
		char buffer[4096];
		ssize_t n = read(g_io_channel_unix_get_fd(channel), buffer, sizeof(buffer));
		if (n > 0)
		{
			sil->mPending.append(buffer, n);
			std::size_t end;
			while ((end = sil->mPending.find('\n')) != std::string::npos)
			{
				std::string line = sil->mPending.substr(0, end);
				sil->mPending.erase(0, end + 1);
				std::string answer = sil->runCommand(line);
				appendLine("acks", line + " -> " + answer);
			}
		}
		return G_SOURCE_CONTINUE;
	}

	std::string runCommand(const std::string &line)
	{
		std::istringstream in(line);
		std::string command;
		in >> command;

		if (command == "mode")
		{
			std::string op, mode;
			in >> op >> mode;
			modes[op] = mode == "fail" ? Mode::FAIL : mode == "defer" ? Mode::DEFER : Mode::OK;
		}
		else if (command == "complete")
		{
			std::string op, outcome;
			in >> op >> outcome;
			auto &queue = deferred[op];
			if (queue.empty())
				return "nothing deferred";
			auto answer = queue.front();
			queue.pop_front();
			answer(outcome == "ok");
		}
		else if (command == "device")
		{
			std::string address, mapInstance;
			int paired = 1;
			in >> address >> paired >> mapInstance;
			mAdapter.addDevice(address, paired != 0, mapInstance);
		}
		else if (command == "map.session")
		{
			std::string sessionKey;
			int connected = 0;
			in >> sessionKey >> connected;
			mAdapter.mMap.notifyConnected(sessionKey, connected != 0);
		}
		else if (command == "map.message")
		{
			std::string sessionId;
			in >> sessionId;
			mAdapter.mMap.notifyMessage(sessionId);
		}
		else if (command == "pbap.connected")
		{
			std::string address;
			int connected = 0;
			in >> address >> connected;
			mAdapter.mPbap.notifyConnected(address, connected != 0);
		}
		else if (command == "pbap.objectpath")
		{
			in >> mAdapter.mPbap.objectPath;
		}
		else if (command == "pbap.transfer")
		{
			std::string address, destination, path, state;
			in >> address >> destination >> path >> state;
			mAdapter.mPbap.notifyTransfer(address, destination, path, state);
		}
		else if (command == "opp.request")
		{
			uint64_t id = 0, size = 0;
			std::string address, file;
			in >> id >> address >> file >> size;
			mAdapter.mOpp.requestTransfer(id, address, file, size);
		}
		else if (command == "opp.state")
		{
			uint64_t id = 0, transferred = 0;
			int finished = 0;
			in >> id >> transferred >> finished;
			mAdapter.mOpp.transferState(id, transferred, finished != 0);
		}
		else if (command == "mesh.onoff")
		{
			int state = 0;
			in >> state;
			mAdapter.mMesh.onOffResult(state != 0);
		}
		else if (command == "mesh.networkid")
		{
			uint64_t id = 0;
			in >> id;
			mAdapter.mMesh.networkId(id);
		}
		else
			return "unknown command";
		return "ok";
	}

	FakeAdapter mAdapter;
	int mFifo = -1;
	GIOChannel *mChannel = nullptr;
	guint mWatch = 0;
	guint mStartup = 0;
	std::string mPending;
};

} // namespace

extern "C" BluetoothSIL *createBluetoothSIL(const int version, BluetoothPairingIOCapability)
{
	if (version != BLUETOOTH_SIL_API_VERSION)
		return nullptr;
	return new FakeSil();
}
