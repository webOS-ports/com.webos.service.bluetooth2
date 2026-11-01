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

// The luna-service2 calls the service and luna-service2++ make (nm -D --undefined-only on both), with no hub
// behind them. A call out of the service with one reply is answered as by a hub that has no such service, other
// calls out are accepted and never answered; nothing here is thread safe, as the
// service is not.

#include "fakels.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glib.h>
#include <luna-service2/lunaservice.h>

struct LSHandle
{
	std::string name;
	std::map<std::string, std::pair<LSMethod *, void *>> categories;	// category -> methods, data
};

struct LSMessage
{
	unsigned refs = 1;
	LSHandle *handle = nullptr;
	std::string payload;
	std::string token;
	bool subscription = false;
	std::shared_ptr<std::vector<std::string>> replies = std::make_shared<std::vector<std::string>>();
};

namespace {

LSHandle *lastHandle = nullptr;
unsigned long nextToken = 1;

struct Subscription
{
	std::string category;
	std::string method;
	LSMessage *message;
};
std::vector<Subscription> subscriptions;

// A call to another service is answered as the hub answers one to a service that is not there, from the main
// loop: LS::Call::get() waits for it there (the mesh profile asks db8 so at start).
struct PendingReply
{
	LSHandle *handle;
	LSFilterFunc callback;
	void *context;
};
std::map<LSMessageToken, std::pair<guint, PendingReply>> pendingReplies;

gboolean deliverReply(gpointer data)
{
	const auto token = static_cast<LSMessageToken>(reinterpret_cast<std::uintptr_t>(data));
	auto found = pendingReplies.find(token);
	if (found == pendingReplies.end())
		return G_SOURCE_REMOVE;
	const PendingReply pending = found->second.second;
	pendingReplies.erase(found);

	auto *reply = new LSMessage;
	reply->handle = pending.handle;
	reply->payload = "{\"returnValue\":false,\"errorCode\":-1,\"errorText\":\"Service does not exist (glue test)\"}";
	reply->token = std::to_string(token);
	if (pending.callback)
		pending.callback(pending.handle, reply, pending.context);
	LSMessageUnref(reply);
	return G_SOURCE_REMOVE;
}

bool fail(LSError *error, const char *text)
{
	if (error)
	{
		error->error_code = -1;
		error->message = strdup(text);
		error->file = __FILE__;
		error->line = __LINE__;
		error->func = "fakels";
	}
	return false;
}

}

namespace fakels {

Reply call(const std::string &category, const std::string &method, const std::string &payload)
{
	Reply reply;
	if (!lastHandle)
		return reply;
	auto found = lastHandle->categories.find(category);
	if (found == lastHandle->categories.end())
		return reply;

	for (LSMethod *entry = found->second.first; entry && entry->name; ++entry)
	{
		if (method != entry->name)
			continue;

		auto *message = new LSMessage;
		message->handle = lastHandle;
		message->payload = payload;
		message->token = std::to_string(nextToken++);
		// As the hub says: the payload asked for one.
		message->subscription = payload.find("\"subscribe\":true") != std::string::npos ||
		                        payload.find("\"subscribe\": true") != std::string::npos;
		auto replies = message->replies;
		reply.handled = entry->function(lastHandle, message, found->second.second);
		reply.payloads = *replies;
		if (message->subscription)
			subscriptions.push_back({category, method, message});
		else
			LSMessageUnref(message);
		return reply;
	}
	return reply;
}

std::vector<std::string> posts(const std::string &category, const std::string &method)
{
	std::vector<std::string> all;
	for (const auto &subscription : subscriptions)
	{
		if (subscription.category == category && subscription.method == method)
			all.insert(all.end(), subscription.message->replies->begin(), subscription.message->replies->end());
	}
	return all;
}

void clear()
{
	for (auto &subscription : subscriptions)
		LSMessageUnref(subscription.message);
	subscriptions.clear();
}

}

extern "C" {

bool LSErrorInit(LSError *error)
{
	if (error)
		memset(error, 0, sizeof(*error));
	return true;
}

void LSErrorFree(LSError *error)
{
	if (!error)
		return;
	free(error->message);
	error->message = nullptr;
}

void LSErrorLog(PmLogContext, const char *, LSError *) { }
void LSErrorLogDefault(const char *, LSError *) { }

bool LSRegister(const char *name, LSHandle **sh, LSError *error)
{
	if (!name || !sh)
		return fail(error, "no name");
	*sh = new LSHandle;
	(*sh)->name = name;
	lastHandle = *sh;
	return true;
}

bool LSUnregister(LSHandle *sh, LSError *)
{
	if (sh == lastHandle)
		lastHandle = nullptr;
	delete sh;
	return true;
}

bool LSRegisterCategory(LSHandle *sh, const char *category, LSMethod *methods, LSSignal *, LSProperty *, LSError *error)
{
	if (!sh || !category)
		return fail(error, "no handle");
	sh->categories[category].first = methods;
	return true;
}

bool LSCategorySetData(LSHandle *sh, const char *category, void *data, LSError *error)
{
	if (!sh || !category || !sh->categories.count(category))
		return fail(error, "no such category");
	sh->categories[category].second = data;
	return true;
}

bool LSGmainAttach(LSHandle *, GMainLoop *, LSError *) { return true; }
GMainContext *LSGmainGetContext(LSHandle *, LSError *) { return g_main_context_default(); }

void LSMessageRef(LSMessage *message)
{
	if (message)
		++message->refs;
}

void LSMessageUnref(LSMessage *message)
{
	if (message && --message->refs == 0)
		delete message;
}

const char *LSMessageGetPayload(LSMessage *message) { return message ? message->payload.c_str() : nullptr; }
const char *LSMessageGetSender(LSMessage *message) { return message ? ":1.42" : nullptr; }
const char *LSMessageGetSenderServiceName(LSMessage *message) { return message ? "com.webos.glue-test" : nullptr; }
const char *LSMessageGetApplicationID(LSMessage *) { return nullptr; }
const char *LSMessageGetUniqueToken(LSMessage *message) { return message ? message->token.c_str() : nullptr; }
bool LSMessageIsSubscription(LSMessage *message) { return message && message->subscription; }
bool LSMessageIsConnected(LSMessage *message) { return message != nullptr; }

bool LSMessageRespond(LSMessage *message, const char *payload, LSError *error)
{
	if (!message || !payload)
		return fail(error, "no message");
	message->replies->push_back(payload);
	return true;
}

bool LSMessageReply(LSHandle *, LSMessage *message, const char *payload, LSError *error)
{
	return LSMessageRespond(message, payload, error);
}

bool LSCall(LSHandle *, const char *, const char *, LSFilterFunc, void *, LSMessageToken *token, LSError *)
{
	if (token)
		*token = nextToken++;
	return true;
}

bool LSCallFromApplicationOneReply(LSHandle *sh, const char *, const char *, const char *, LSFilterFunc callback,
                                   void *context, LSMessageToken *token, LSError *)
{
	const LSMessageToken mine = nextToken++;
	const guint source = g_idle_add(&deliverReply, reinterpret_cast<gpointer>(static_cast<std::uintptr_t>(mine)));
	pendingReplies[mine] = { source, { sh, callback, context } };
	if (token)
		*token = mine;
	return true;
}

bool LSCallCancel(LSHandle *, LSMessageToken token, LSError *)
{
	auto found = pendingReplies.find(token);
	if (found != pendingReplies.end())
	{
		g_source_remove(found->second.first);
		pendingReplies.erase(found);
	}
	return true;
}
bool LSCallCancelNotificationAdd(LSHandle *, LSCancelNotificationFunc, void *, LSError *) { return true; }
bool LSCallCancelNotificationRemove(LSHandle *, LSCancelNotificationFunc, void *, LSError *) { return true; }

bool LSRegisterServerStatusEx(LSHandle *, const char *, LSServerStatusFunc, void *, void **cookie, LSError *)
{
	static int cookies;
	if (cookie)
		*cookie = &cookies;
	return true;
}

bool LSCancelServerStatus(LSHandle *, void *, LSError *) { return true; }

}
