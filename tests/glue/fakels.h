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

// A Luna bus in the test process. fakels.cpp defines the luna-service2 C calls the service (and
// luna-service2++) make; being in the executable, they are found before the library's. A test calls the
// service's methods through it and reads what they answered.

#ifndef FAKELS_H
#define FAKELS_H

#include <string>
#include <vector>

namespace fakels {

struct Reply
{
	//! What the method answered, in order: the first reply, then subscription posts.
	std::vector<std::string> payloads;
	//! The method's own return value.
	bool handled = false;
};

//! Calls category/method of the service registered last, as a client would, with payload. A subscription
//! ("subscribe": true) stays open until the service lets go of it or until clear().
Reply call(const std::string &category, const std::string &method, const std::string &payload);

//! The replies a subscription made through call() has had since.
std::vector<std::string> posts(const std::string &category, const std::string &method);

//! Drops the messages kept for subscriptions (the service's own references stay).
void clear();

}

#endif
