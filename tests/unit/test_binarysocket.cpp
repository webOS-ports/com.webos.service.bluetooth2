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

// The SPP binary socket, with a real client on its UNIX socket and the GLib main loop pumped by hand.
// SIGPIPE keeps its default action: a write to a client that is gone must not end the process.

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <functional>
#include <glib.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include "bluetoothbinarysocket.h"

namespace {

void pumpUntil(const std::function<bool()> &done, int timeoutMs = 2000)
{
	auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	while (!done() && std::chrono::steady_clock::now() < end)
		g_main_context_iteration(nullptr, FALSE);
}

void pumpFor(int ms)
{
	pumpUntil([] { return false; }, ms);
}

int connectClient(const std::string &name)
{
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_un addr = {};
	addr.sun_family = AF_UNIX;
	std::string path = std::string(BINARY_SOCKET_DIRECTORY) + "/" + BINARY_SOCKET_FILE_NAME_PREFIX + name;
	strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		close(fd);
		return -1;
	}
	return fd;
}

// Reads what is there now, without blocking.
std::string drain(int fd)
{
	std::string out;
	char buf[4096];
	for (;;)
	{
		ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
		if (n <= 0)
			break;
		out.append(buf, n);
	}
	return out;
}

class BinarySocket : public ::testing::Test
{
protected:
	void SetUp() override
	{
		signal(SIGPIPE, SIG_DFL);
		socket = new BluetoothBinarySocket();
		ASSERT_TRUE(socket->createBinarySocket(name()));
		ASSERT_TRUE(socket->registerReceiveDataWatch([this](guchar *buf, gsize len) {
			received.append(reinterpret_cast<char *>(buf), len);
		}));
	}

	void TearDown() override
	{
		if (socket)
		{
			socket->removeBinarySocket();
			delete socket;
		}
		for (int fd : clients)
			close(fd);
		pumpFor(10);
	}

	// Short: the socket path is cut at BINARY_SOCKET_FILE_NAME_SIZE (the service uses channel ids like "001").
	std::string name() const
	{
		static int counter = 0;
		static std::string current;
		static const ::testing::TestInfo *forTest = nullptr;
		const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
		if (info != forTest)
		{
			forTest = info;
			current = "t" + std::to_string(++counter);
		}
		return current;
	}

	int client()
	{
		int fd = connectClient(name());
		EXPECT_GE(fd, 0);
		clients.push_back(fd);
		return fd;
	}

	void send(const std::string &data)
	{
		socket->sendData(reinterpret_cast<const uint8_t *>(data.data()), data.size());
	}

	BluetoothBinarySocket *socket = nullptr;
	std::vector<int> clients;
	std::string received;
};

} // namespace

TEST_F(BinarySocket, DataBeforeTheClientConnectsIsDeliveredOnConnect)
{
	send("early");
	int fd = client();
	std::string got;
	pumpUntil([&] { got += drain(fd); return got == "early"; });
	EXPECT_EQ(got, "early");
}

TEST_F(BinarySocket, DeliversToTheClientAndReceivesFromIt)
{
	int fd = client();
	pumpFor(50);	// accept

	send("to-client");
	std::string got;
	pumpUntil([&] { got += drain(fd); return got == "to-client"; });
	EXPECT_EQ(got, "to-client");

	ASSERT_EQ(write(fd, "from-client", 11), 11);
	pumpUntil([&] { return received == "from-client"; });
	EXPECT_EQ(received, "from-client");
}

TEST_F(BinarySocket, ClientThatHungUpDoesNotRaiseSigpipe)
{
	int fd = client();
	pumpFor(50);	// accept
	close(fd);
	clients.clear();

	// Before the hang-up has been seen: the write fails with EPIPE, it must not raise SIGPIPE.
	send("into the void");
	send("and again");
	pumpFor(50);
	send("after the hang-up was seen");
	SUCCEED();
}

TEST_F(BinarySocket, SecondClientReplacesTheFirst)
{
	int first = client();
	pumpFor(50);
	int second = client();
	pumpFor(50);

	send("second");
	std::string got;
	pumpUntil([&] { got += drain(second); return got == "second"; });
	EXPECT_EQ(got, "second");

	// The first client's connection was closed rather than leaked: it reads end of file.
	char c;
	EXPECT_EQ(recv(first, &c, 1, MSG_DONTWAIT), 0);
}

TEST_F(BinarySocket, BackPressureKeepsEverythingInOrder)
{
	int fd = client();
	pumpFor(50);
	int small = 4096;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));

	// More than the socket buffers hold while the client does not read: sends are partial or would block,
	// and the rest has to wait for the retry timer, in order.
	std::string expected;
	for (int i = 0; i < 400; i++)
	{
		std::string chunk = "chunk " + std::to_string(i) + " " + std::string(1000, 'a' + i % 26) + "\n";
		expected += chunk;
		send(chunk);
	}

	std::string got;
	pumpUntil([&] { got += drain(fd); return got.size() >= expected.size(); }, 10000);
	EXPECT_EQ(got.size(), expected.size());
	EXPECT_TRUE(got == expected);
}

TEST_F(BinarySocket, RemovingTheSocketStopsAPendingRetry)
{
	int fd = client();
	pumpFor(50);
	int small = 4096;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));

	for (int i = 0; i < 200; i++)
		send(std::string(1000, 'x'));

	// A retry is now pending. The timer used to stay armed and run on the deleted socket.
	socket->removeBinarySocket();
	delete socket;
	socket = nullptr;

	pumpFor(200);	// ten retry periods
	SUCCEED();
}

TEST_F(BinarySocket, OneLargeWriteArrivesWhole)
{
	int fd = client();
	pumpFor(50);
	int small = 4096;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));

	// Bigger than the socket buffers in one go: the first send is partial. The rest used to be dropped as
	// soon as any part had been written.
	std::string expected;
	for (int i = 0; expected.size() < 512 * 1024; i++)
		expected += std::to_string(i) + ",";
	send(expected);

	std::string got;
	pumpUntil([&] { got += drain(fd); return got.size() >= expected.size(); }, 10000);
	EXPECT_EQ(got.size(), expected.size());
	EXPECT_TRUE(got == expected);
}
