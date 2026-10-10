// Copyright (c) 2014-2024 LG Electronics, Inc.
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

#include <sstream>
#include <locale>

#include <glib.h>
#include <time.h>

#include "utils.h"
#include "logging.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <initializer_list>
#include <vector>


using namespace std;

std::vector<std::string> split(const std::string &s, char delim)
{
	std::vector<std::string> elems;
	std::stringstream ss(s);
	std::string item;
	while (std::getline(ss, item, delim))
	{
		elems.push_back(item);
	}
	return elems;
}

std::string convertToLower(const std::string &input)
{
	std::string output;
	std::locale loc;
	for (std::string::size_type i=0; i<input.length(); ++i)
		output += std::tolower(input[i],loc);
	return output;
}

std::string convertToUpper(const std::string &input)
{
	std::string output;
	std::locale loc;
	for (std::string::size_type i=0; i<input.length(); ++i)
		output += std::toupper(input[i],loc);
	return output;
}

bool checkPathExists(const std::string &path)
{
	if (path.length() == 0)
		return false;

	gchar *fileBasePath = g_path_get_dirname(path.c_str());
	if (!fileBasePath)
		return false;

	bool result = g_file_test(fileBasePath, (GFileTest)(G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR));

	g_free(fileBasePath);

	return result;
}

bool checkFileIsValid(const std::string &path)
{
	std::string testPath = path;

	if (testPath.length() == 0)
		return false;

	// A regular file, not a link to one. g_file_test() with EXISTS | IS_REGULAR is true when either test
	// passes, so it accepted directories, FIFOs and device nodes (an endless transfer of /dev/zero).
	struct stat st;
	if (lstat(testPath.c_str(), &st) != 0)
		return false;

	return S_ISREG(st.st_mode);
}

bool isContainedRelativePath(const std::string &path)
{
	if (path.empty() || path[0] == '/')
		return false;

	std::size_t start = 0;
	while (start <= path.size())
	{
		std::size_t end = path.find('/', start);
		if (end == std::string::npos)
			end = path.size();
		const std::string component = path.substr(start, end - start);
		if (component == "." || component == "..")
			return false;
		start = end + 1;
	}
	return true;
}

bool isSinglePathComponent(const std::string &name)
{
	return !name.empty() && name.find('/') == std::string::npos && name != "." && name != "..";
}

static bool isUnderDirectory(const std::string &resolvedPath, const std::string &baseDir)
{
	char *base = realpath(baseDir.c_str(), nullptr);
	if (!base)
		return false;
	std::string resolvedBase = base;
	free(base);

	if (resolvedPath == resolvedBase)
		return true;
	if (resolvedBase.back() != '/')
		resolvedBase += '/';
	return resolvedPath.compare(0, resolvedBase.size(), resolvedBase) == 0;
}

bool resolveUnderDirectory(const std::string &path, const std::string &baseDir, std::string &resolved)
{
	if (path.empty())
		return false;

	char *real = realpath(path.c_str(), nullptr);
	if (!real)
		return false;
	std::string resolvedPath = real;
	free(real);

	if (!isUnderDirectory(resolvedPath, baseDir))
		return false;

	resolved = resolvedPath;
	return true;
}

bool resolveParentUnderDirectory(const std::string &path, const std::string &baseDir, std::string &resolved)
{
	if (path.empty() || path.back() == '/')
		return false;

	gchar *dir = g_path_get_dirname(path.c_str());
	gchar *name = g_path_get_basename(path.c_str());
	std::string fileName = name ? name : "";
	std::string resolvedDir;
	bool ok = dir && resolveUnderDirectory(dir, baseDir, resolvedDir) && isSinglePathComponent(fileName);
	g_free(dir);
	g_free(name);

	if (!ok)
		return false;

	resolved = resolvedDir + "/" + fileName;
	return true;
}

std::string resolvePathUnderDirectory(const std::string &baseDir, const std::string &relativePath)
{
	if (!isContainedRelativePath(relativePath))
		return "";

	std::string full = baseDir + "/" + relativePath;
	std::string resolved;
	if (resolveUnderDirectory(full, baseDir, resolved))
		return resolved;

	// Not there yet, a destination: its directory must be, and the name must not be an entry that did not
	// resolve inside, like a dangling link.
	struct stat st;
	if (lstat(full.c_str(), &st) == 0)
		return "";
	if (resolveParentUnderDirectory(full, baseDir, resolved))
		return resolved;

	return "";
}

/**
*      write_klog
*
*      @name   write_kernel log
*      @param  message [in] string for kernel
*      @return NULL
*/
void write_kernel_log(const char *message)
{
	// One write() is one kernel log record. This used to strncat() the message onto an uninitialised stack
	// buffer, which appended it after whatever the stack held and could run past the buffer.
	int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		BT_DEBUG("open fail : /dev/kmsg\n");
		return;
	}

	auto ret = write(fd, message, strlen(message));
	static_cast<void>(ret);
	close(fd);
}

/**
*      print bluetooth ready log to kernel
*
*      @name   bt_ready_msg2kernel
*      @param  message [in] string for kernel
*      @return NULL
*/
void bt_ready_msg2kernel(void)
{
#define MAX_STRING_SIZE 128
	char logBuf[MAX_STRING_SIZE] = "";
	time_t sec;
	long msec;
	struct timespec ct;

	clock_gettime(CLOCK_MONOTONIC, &ct);
	sec = ct.tv_sec;
	msec = ct.tv_nsec/1000000.0;
	snprintf(logBuf, sizeof(logBuf), "Get BTUSB_READY %ld.%ld PerfType:WBS PerfGroup:bt_ready \n", (long)sec, msec );
	BT_DEBUG("Get BTUSB_READY %ld.%ld PerfType:BtMngr PerfGroup:BT_INITIALIZED \n", (long)sec, msec );
	write_kernel_log(logBuf);
}

std::string replaceString(std::string subject, const std::string& search, const std::string& replace) {
    size_t pos = 0;
    while ((pos = subject.find(search, pos)) != std::string::npos) {
         subject.replace(pos, search.length(), replace);
         pos += replace.length();
    }
    return subject;
}

// Runs a program from PATH with these arguments, without a shell: file and folder names are never parsed as
// shell syntax. These used to be joined into one "sh -c" command line, unquoted.
static bool runWithoutShell(std::initializer_list<std::string> args)
{
	std::vector<gchar *> argv;
	for (const auto &arg : args)
		argv.push_back(const_cast<gchar *>(arg.c_str()));
	argv.push_back(nullptr);

	GError *error = nullptr;
	gint waitStatus = 0;
	if (!g_spawn_sync(nullptr, argv.data(), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, nullptr,
	                  &waitStatus, &error))
	{
		g_printerr("Error executing %s: %s\n", argv[0], error ? error->message : "unknown error");
		if (error)
			g_error_free(error);
		return false;
	}

	if (!g_spawn_check_wait_status(waitStatus, &error))
	{
		g_printerr("%s failed: %s\n", argv[0], error ? error->message : "unknown error");
		if (error)
			g_error_free(error);
		return false;
	}
	return true;
}

bool changeGroup(const std::string &groupName, const std::string &fileName)
{
	std::string testfileName = fileName;

	if (testfileName.length() == 0)
		return false;

	if (g_file_test(fileName.c_str(), (GFileTest) G_FILE_TEST_IS_SYMLINK))
		return false;

	if (!g_file_test(testfileName.c_str(), (GFileTest)(G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR)))
		return false;

	return runWithoutShell({ "chgrp", groupName, "--", testfileName });
}

bool changeFilePermission(const std::string &permission, const std::string &fileName)
{
	std::string testfileName = fileName;

	if (testfileName.length() == 0)
		return false;

	if (g_file_test(fileName.c_str(), (GFileTest) G_FILE_TEST_IS_SYMLINK))
		return false;

	if (!g_file_test(testfileName.c_str(), (GFileTest)(G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR)))
		return false;

	return runWithoutShell({ "chmod", permission, "--", testfileName });
}

bool changeFolderPermission(const std::string &permission, const std::string &folderName)
{
	std::string testfolderName = folderName;

	if (testfolderName.length() == 0)
		return false;

	if (g_file_test(folderName.c_str(), (GFileTest) G_FILE_TEST_IS_SYMLINK))
		return false;

	if (!g_file_test(testfolderName.c_str(), (GFileTest)(G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR)))
		return false;

	return runWithoutShell({ "chmod", "-R", permission, "--", testfolderName });
}

bool changeFolderGroup(const std::string &groupName, const std::string &folderName)
{
	std::string testfolderName = folderName;

	if (testfolderName.length() == 0)
		return false;

	if (g_file_test(folderName.c_str(), (GFileTest) G_FILE_TEST_IS_SYMLINK))
		return false;

	if (!g_file_test(testfolderName.c_str(), (GFileTest)(G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR)))
		return false;

	return runWithoutShell({ "chgrp", "-R", groupName, "--", testfolderName });
}

/*all new files and subdirectories created within
  the current directory inherit the group ID of the directory.
*/
bool setGroupID(const std::string &folderName)
{
	std::string testfolderName = folderName;

	if (testfolderName.length() == 0)
		return false;

	if (g_file_test(folderName.c_str(), (GFileTest) G_FILE_TEST_IS_SYMLINK))
		return false;

	if (!g_file_test(testfolderName.c_str(), (GFileTest)(G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR)))
		return false;

	return runWithoutShell({ "chmod", "g+s", "--", testfolderName });
}

void eraseAllSubStr(std::string & mainStr, const std::string & toErase)
{
	size_t pos = std::string::npos;
	// Search for the substring in string in a loop untill nothing is found
	while ((pos  = mainStr.find(toErase) )!= std::string::npos)
	{
		// If found then erase it from string
		mainStr.erase(pos, toErase.length());
	}
}