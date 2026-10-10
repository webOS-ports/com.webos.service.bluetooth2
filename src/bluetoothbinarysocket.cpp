// Copyright (c) 2015-2020 LG Electronics, Inc.
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


#include "bluetoothbinarysocket.h"
#include "logging.h"

#include <cerrno>

BluetoothBinarySocket::BluetoothBinarySocket() :
	mBufferSize(0),
	mRetryTimeout(0),
	mRetryCount(0),
	mServerSocketFd(-1),
	mClientSocketFd(-1),
	mWriting(false),
	mServerIoChannel(NULL),
	mClientIoChannel(NULL),
	mServerWatch(0),
	mClientWatch(0)
{
	mSocketFileName[0] = '\0';
}

BluetoothBinarySocket::~BluetoothBinarySocket()
{
	// Idempotent: callers remove the socket first, but a source left behind would run on a freed object.
	removeBinarySocket();
}

bool BluetoothBinarySocket::createBinarySocket(const std::string &name)
{
	if (name.empty())
		return false;

	if (mkdir(BINARY_SOCKET_DIRECTORY, S_IRUSR | S_IWUSR | S_IXUSR |
										S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) < 0)
	{
		if (errno != EEXIST)
			BT_DEBUG("Failed to create binary socket directory");
	}

	int serverSockFd;
	struct sockaddr_un serverAddr;

	snprintf(mSocketFileName, BINARY_SOCKET_FILE_NAME_SIZE, "%s/%s%s",
			BINARY_SOCKET_DIRECTORY, BINARY_SOCKET_FILE_NAME_PREFIX, name.c_str());

	// A socket left by an earlier run; one that is not there is fine. (access() first was a check-then-act.)
	unlink(mSocketFileName);

	if ((serverSockFd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return false;

	bzero(&serverAddr, sizeof(serverAddr));
	serverAddr.sun_family = AF_UNIX;
	strncpy(serverAddr.sun_path, mSocketFileName, sizeof(serverAddr.sun_path)-1);

	if (bind(serverSockFd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0)
	{
		if (!close(serverSockFd))
			BT_DEBUG("Failed to close server socket");
		return false;
	}
	mServerSocketFd = serverSockFd;

	if (chmod(mSocketFileName, ACCESSPERMS) < 0)
	{
		if (errno != EEXIST)
		{
			BT_DEBUG("Failed to chmod binary socket file");
			return false;
		}
	}

	return true;
}

void BluetoothBinarySocket::closeClient()
{
	if (mClientWatch)
	{
		g_source_remove(mClientWatch);
		mClientWatch = 0;
	}

	// Shutting the channel down closes its fd; it was closed a second time after that.
	if (NULL != mClientIoChannel)
	{
		g_io_channel_shutdown(mClientIoChannel, TRUE, NULL);
		g_io_channel_unref(mClientIoChannel);
		mClientIoChannel = NULL;
	}
	else if (mClientSocketFd >= 0)
	{
		close(mClientSocketFd);
	}
	mClientSocketFd = -1;

	if (mRetryTimeout)
	{
		g_source_remove(mRetryTimeout);
		mRetryTimeout = 0;
	}
	mRetryData.clear();
}

void BluetoothBinarySocket::removeBinarySocket(void)
{
	closeClient();

	if (mServerWatch)
	{
		g_source_remove(mServerWatch);
		mServerWatch = 0;
	}

	if (NULL != mServerIoChannel)
	{
		g_io_channel_shutdown(mServerIoChannel, TRUE, NULL);
		g_io_channel_unref(mServerIoChannel);
		mServerIoChannel = NULL;
	}
	else if (mServerSocketFd >= 0)
	{
		close(mServerSocketFd);
	}
	mServerSocketFd = -1;

	if (mSocketFileName[0] != '\0')
		unlink(mSocketFileName);
}

bool BluetoothBinarySocket::registerReceiveDataWatch(BluetoothBinarySocketReceiveCallback callback)
{
	if (listen(mServerSocketFd, DEFAULT_LISTEN_BACKLOG))
		return false;

	mCallback = callback;

	mServerIoChannel = g_io_channel_unix_new(mServerSocketFd);
	g_io_channel_set_flags(mServerIoChannel, G_IO_FLAG_NONBLOCK, NULL);
	g_io_channel_set_close_on_unref(mServerIoChannel, TRUE);

	mServerWatch = g_io_add_watch(mServerIoChannel, (GIOCondition)(G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL),
					&getAcceptRequest, this);

	return true;
}

bool BluetoothBinarySocket::sendData(const uint8_t *data, const uint32_t size)
{
	if (mClientSocketFd < 0)
	{
		storeSendDataToBuffer(data, size);
		return true;
	}

	// Earlier data still waits for the client: queue behind it to keep the order.
	if (mRetryTimeout)
	{
		mRetryData.insert(mRetryData.end(), data, data + size);
		return true;
	}

	// MSG_NOSIGNAL: a client that has gone away gives EPIPE, not a SIGPIPE that ends the service.
	ssize_t written = send(mClientSocketFd, data, size, MSG_NOSIGNAL);
	if (written == (ssize_t)size)
		return true;

	if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
	{
		BT_DEBUG("Binary socket client is gone (%d)", errno);
		closeClient();
		return true;
	}

	// A partial write used to count as done and drop the rest.
	uint32_t offset = written > 0 ? (uint32_t)written : 0;
	retrySendData(data + offset, size - offset);

	return true;
}

void BluetoothBinarySocket::retrySendData(const uint8_t *data, const uint32_t size)
{
	/*
	 * This logic is the migration from bluetooth-manager.
	 * It avoids to drop the data which is sent from the stack to the socket.
	 * MAX_WRITE_RETRY_COUNT and WRITE_RETRY_SLEEP_TIME are configured with the
	 * socket client side. (i.e. watchmanager and mashupmanager)
	 *
	 * The data is copied: the caller's buffer belongs to the SIL and is gone after the call. The timer id
	 * is kept, so that removing the socket removes the timer; it was dropped, and the timer then ran on a
	 * deleted socket.
	 */
	auto retrySendDataCallback = [] (gpointer userData) -> gboolean {
		BluetoothBinarySocket *binarySocket = static_cast<BluetoothBinarySocket *>(userData);

		if (binarySocket->mRetryCount > MAX_WRITE_RETRY_COUNT || binarySocket->mClientSocketFd < 0)
		{
			binarySocket->mRetryData.clear();
			binarySocket->mRetryTimeout = 0;
			return FALSE;
		}

		ssize_t written = send(binarySocket->mClientSocketFd, binarySocket->mRetryData.data(),
		                       binarySocket->mRetryData.size(), MSG_NOSIGNAL);
		if (written > 0)
			binarySocket->mRetryData.erase(binarySocket->mRetryData.begin(), binarySocket->mRetryData.begin() + written);

		if (binarySocket->mRetryData.empty())
		{
			binarySocket->mRetryTimeout = 0;
			return FALSE;
		}

		binarySocket->mRetryCount++;
		return TRUE;
	};

	mRetryData.assign(data, data + size);
	mRetryCount = 0;

	mRetryTimeout = g_timeout_add(WRITE_RETRY_SLEEP_TIME, retrySendDataCallback, this);
}

void BluetoothBinarySocket::storeSendDataToBuffer(const uint8_t *data, const uint32_t size)
{
	/*
	 * This logic is the migration from bluetooth-manager.
	 * The buffer will be sent first when the socket is connected.
	 */
	if (size < (DATA_BUFFER_SIZE - mBufferSize))
	{
		memcpy((uint8_t *)(mBufferData + mBufferSize), data, size);
		mBufferSize += size;
	}
}

void BluetoothBinarySocket::sendBufferData()
{
	if (mBufferSize == 0)
		return;

	ssize_t written = send(mClientSocketFd, mBufferData, mBufferSize, MSG_NOSIGNAL);
	if (written <= 0)
		return;

	// Keep what did not go out for the next connection; it used to be kept whole or dropped whole.
	mBufferSize -= (uint32_t)written;
	memmove(mBufferData, mBufferData + written, mBufferSize);
}

gboolean BluetoothBinarySocket::getAcceptRequest(GIOChannel */*io*/, GIOCondition cond, gpointer userData)
{
	BluetoothBinarySocket *binarySocket = static_cast<BluetoothBinarySocket *>(userData);

	if (cond & (G_IO_NVAL | G_IO_HUP | G_IO_ERR))
	{
		binarySocket->mServerWatch = 0;
		return FALSE;
	}

	if (NULL == binarySocket->mServerIoChannel || binarySocket->mServerSocketFd < 0)
	{
		binarySocket->mServerWatch = 0;
		return FALSE;
	}

	struct sockaddr_un clientAddr;
	socklen_t clientLen = sizeof(clientAddr);

	int clientFd = accept4(binarySocket->mServerSocketFd, (struct sockaddr *)&clientAddr, &clientLen, SOCK_CLOEXEC);
	if (clientFd < 0)
		return TRUE;	// a failed accept() does not end the listening socket

	// One client at a time: a second accept() used to overwrite the first client's fd and channel and leak them.
	binarySocket->closeClient();
	binarySocket->mClientSocketFd = clientFd;

	binarySocket->sendBufferData();

	binarySocket->mClientIoChannel = g_io_channel_unix_new(binarySocket->mClientSocketFd);
	g_io_channel_set_flags(binarySocket->mClientIoChannel, G_IO_FLAG_NONBLOCK, NULL);
	g_io_channel_set_close_on_unref(binarySocket->mClientIoChannel, TRUE);
	binarySocket->mClientWatch = g_io_add_watch(binarySocket->mClientIoChannel,
	                                            (GIOCondition)(G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL),
	                                            &getReceiveRequest, userData);

	return TRUE;
}

gboolean BluetoothBinarySocket::getReceiveRequest(GIOChannel */*io*/, GIOCondition cond, gpointer userData)
{
	BluetoothBinarySocket *binarySocket = static_cast<BluetoothBinarySocket *>(userData);

	if (NULL == binarySocket->mClientIoChannel || binarySocket->mClientSocketFd < 0)
	{
		binarySocket->mClientWatch = 0;
		return FALSE;
	}

	if (!(cond & (G_IO_NVAL | G_IO_ERR)))
	{
		if (binarySocket->isWriting())
			return TRUE;

		guchar buf[READ_BUFFER_SIZE];
		ssize_t readBytes = read(binarySocket->mClientSocketFd, buf, sizeof(buf));

		if (readBytes > 0)
		{
			if (NULL != binarySocket->mCallback)
				binarySocket->mCallback(buf, readBytes);
			return TRUE;
		}

		if (readBytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) && !(cond & G_IO_HUP))
			return TRUE;
	}

	// The client hung up or failed. Its fd and channel used to stay, and the next data from the remote
	// device was written to the dead peer. This source ends with the FALSE below.
	binarySocket->mClientWatch = 0;
	binarySocket->closeClient();
	return FALSE;
}
