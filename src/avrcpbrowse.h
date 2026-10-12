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

// The AVRCP browsing requests that the remote device answers (ChangeFolder, Play), as the profile service sends
// them to the SIL. Kept apart from the service, which needs a bus, so that it can be tested.

#ifndef AVRCPBROWSE_H_
#define AVRCPBROWSE_H_

#include <string>

#include <bluetooth-sil-api.h>

enum class AvrcpBrowseRequest
{
	CHANGE_PATH,
	PLAY_ITEM
};

// Sends the request through the SIL's callback variant: answer is called with the remote device's result, or
// with the error that kept the request from going out. A SIL without that variant answers at once with what
// its variant without a callback returned (the API's default). answer is called at most once, also when a SIL
// calls back twice: the Luna reply it sends drops the request's message reference.
void sendAvrcpBrowseRequest(BluetoothAvrcpProfile *impl, AvrcpBrowseRequest request, const std::string &itemPath,
							BluetoothResultCallback answer);

#endif // AVRCPBROWSE_H_
