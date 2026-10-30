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

namespace {

std::string lower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}

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

bool AudioReconnect::connected(const std::string &adapter, const std::string &address)
{
	if (!isAddress(adapter) || !isAddress(address))
		return false;

	std::string &last = mLast[lower(adapter)];
	const std::string device = lower(address);
	if (last == device)
		return false;
	last = device;
	return true;
}

bool AudioReconnect::forget(const std::string &adapter, const std::string &address)
{
	auto it = mLast.find(lower(adapter));
	if (it == mLast.end() || it->second != lower(address))
		return false;
	mLast.erase(it);
	return true;
}

std::string AudioReconnect::target(const std::string &adapter) const
{
	auto it = mLast.find(lower(adapter));
	return it == mLast.end() ? std::string() : it->second;
}

std::string AudioReconnect::toText() const
{
	std::string text;
	for (const auto &entry : mLast)
		text += entry.first + " " + entry.second + "\n";
	return text;
}

void AudioReconnect::fromText(const std::string &text)
{
	mLast.clear();
	std::istringstream lines(text);
	std::string line;
	while (std::getline(lines, line))
	{
		std::istringstream fields(line);
		std::string adapter, device, extra;
		if (!(fields >> adapter >> device) || (fields >> extra))
			continue;
		connected(adapter, device);
	}
}
