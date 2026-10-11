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

#include "avrcpbrowse.h"

#include <memory>

void sendAvrcpBrowseRequest(BluetoothAvrcpProfile *impl, AvrcpBrowseRequest request, const std::string &itemPath,
							BluetoothResultCallback answer)
{
	auto answered = std::make_shared<bool>(false);
	auto once = [answered, answer](BluetoothError error) {
		if (*answered)
			return;
		*answered = true;
		if (answer)
			answer(error);
	};

	if (request == AvrcpBrowseRequest::CHANGE_PATH)
		impl->changePath(itemPath, once);
	else
		impl->playItem(itemPath, once);
}
