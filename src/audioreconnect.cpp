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

#include "audioreconnect.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include <glib.h>

namespace {

// What the remote device offers: the A2DP sink of headphones or a speaker,
// the hands-free unit of a headset or a car kit (the Assigned Numbers).
const char kA2dpSinkUuid[] = "0000110b-0000-1000-8000-00805f9b34fb";
const char kHfpHandsFreeUuid[] = "0000111e-0000-1000-8000-00805f9b34fb";

const char kSwitch[] = "autoReconnect";

std::string lower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}

}

bool AudioReconnect::profileFromName(const std::string &name, Profile &profile)
{
	if (name == "A2DP")
		profile = A2DP;
	else if (name == "HFP")
		profile = HFP;
	else
		return false;
	return true;
}

bool AudioReconnect::isAudioDevice(Profile profile, const std::vector<std::string> &uuids)
{
	const std::string wanted = profile == A2DP ? kA2dpSinkUuid : kHfpHandsFreeUuid;
	return std::any_of(uuids.begin(), uuids.end(),
	                   [&wanted](const std::string &uuid) { return lower(uuid) == wanted; });
}

bool AudioReconnect::isAddress(const std::string &text)
{
	// 00:11:22:33:44:55
	if (text.size() != 17)
		return false;
	for (std::string::size_type i = 0; i < text.size(); i++)
	{
		const unsigned char c = static_cast<unsigned char>(text[i]);
		if (i % 3 == 2 ? c != ':' : !std::isxdigit(c))
			return false;
	}
	return true;
}

bool AudioReconnect::connected(const std::string &adapter, const std::string &address, Profile profile)
{
	if (!isAddress(adapter) || !isAddress(address))
		return false;

	Target &last = mLast[lower(adapter)];
	const std::string device = lower(address);
	if (last.address != device)
	{
		// Another device: only what it connected counts.
		last.address = device;
		last.profiles = profile;
		return true;
	}
	if ((last.profiles & profile) != 0U)
		return false;
	last.profiles |= profile;
	return true;
}

bool AudioReconnect::disconnected(const std::string &adapter, const std::string &address, Disconnect how)
{
	// A link loss, or the device going away, is no reason not to bring it
	// back (PhonePolicy keeps it on CONNECTED -> DISCONNECTED).
	if (how != Disconnect::ByUser)
		return false;
	return forget(adapter, address);
}

bool AudioReconnect::forget(const std::string &adapter, const std::string &address)
{
	auto it = mLast.find(lower(adapter));
	if (it == mLast.end() || it->second.address != lower(address))
		return false;
	mLast.erase(it);
	return true;
}

bool AudioReconnect::forget(const std::string &address)
{
	const std::string device = lower(address);
	bool forgotten = false;
	for (auto it = mLast.begin(); it != mLast.end();)
	{
		if (it->second.address == device)
		{
			it = mLast.erase(it);
			forgotten = true;
		}
		else
			++it;
	}
	return forgotten;
}

AudioReconnect::Target AudioReconnect::target(const std::string &adapter) const
{
	if (!mEnabled)
		return Target();
	auto it = mLast.find(lower(adapter));
	return it == mLast.end() ? Target() : it->second;
}

bool AudioReconnect::setEnabled(bool enabled)
{
	if (mEnabled == enabled)
		return false;
	mEnabled = enabled;
	return true;
}

std::string AudioReconnect::toText() const
{
	std::string text = std::string(kSwitch) + (mEnabled ? " on\n" : " off\n");
	for (const auto &entry : mLast)
	{
		text += entry.first + " " + entry.second.address;
		if ((entry.second.profiles & A2DP) != 0U)
			text += " a2dp";
		if ((entry.second.profiles & HFP) != 0U)
			text += " hfp";
		text += "\n";
	}
	return text;
}

void AudioReconnect::fromText(const std::string &text)
{
	mLast.clear();
	mEnabled = true;

	std::istringstream lines(text);
	std::string line;
	while (std::getline(lines, line))
	{
		std::istringstream fields(line);
		std::vector<std::string> words;
		std::string word;
		while (fields >> word)
			words.push_back(word);

		if (words.size() == 2 && words[0] == kSwitch)
		{
			if (words[1] == "on" || words[1] == "off")
				mEnabled = words[1] == "on";
			continue;
		}
		if (words.size() < 2 || !isAddress(words[0]) || !isAddress(words[1]))
			continue;

		// A line with no profile is from before HFP was remembered: A2DP.
		unsigned profiles = words.size() == 2 ? static_cast<unsigned>(A2DP) : 0U;
		bool understood = true;
		for (std::vector<std::string>::size_type i = 2; i < words.size(); i++)
		{
			if (words[i] == "a2dp")
				profiles |= A2DP;
			else if (words[i] == "hfp")
				profiles |= HFP;
			else
				understood = false;
		}
		if (!understood)
			continue;

		Target &last = mLast[lower(words[0])];
		last.address = lower(words[1]);
		last.profiles = profiles;
	}
}

void AudioReconnect::load(const std::string &path)
{
	gchar *contents = nullptr;
	if (!g_file_get_contents(path.c_str(), &contents, nullptr, nullptr))
	{
		fromText("");
		return;
	}
	fromText(contents);
	g_free(contents);
}

bool AudioReconnect::store(const std::string &path, std::string &error) const
{
	GError *gerror = nullptr;
	const std::string text = toText();
	if (g_file_set_contents(path.c_str(), text.c_str(), static_cast<gssize>(text.size()), &gerror))
		return true;
	error = gerror ? gerror->message : "unknown error";
	g_clear_error(&gerror);
	return false;
}
