// Copyright (c) 2014-2018 LG Electronics, Inc.
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

#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <vector>

std::vector<std::string> split(const std::string &s, char delim);
std::string convertToLower(const std::string &input);
std::string convertToUpper(const std::string &input);
std::string replaceString(std::string subject, const std::string& search, const std::string& replace);
void eraseAllSubStr(std::string & mainStr, const std::string & toErase);

bool checkPathExists(const std::string &path);
bool checkFileIsValid(const std::string &path);
// Paths from Luna clients are relative to a storage directory. True when \p path is relative and has no
// "." or ".." component, so appending it to a directory cannot name anything outside it.
bool isContainedRelativePath(const std::string &path);
// True for one path component that is not "." or "..": a folder name, not a path.
bool isSinglePathComponent(const std::string &name);
// Resolves \p path, symbolic links included, and checks that it is \p baseDir or lies under it; the
// resolved path is stored in \p resolved. \p path must exist.
bool resolveUnderDirectory(const std::string &path, const std::string &baseDir, std::string &resolved);
// The same for a file that may not exist yet: its directory must exist and lie under \p baseDir.
bool resolveParentUnderDirectory(const std::string &path, const std::string &baseDir, std::string &resolved);
// \p relativePath under \p baseDir, resolved, for a file that exists or one to be created there; empty if
// the path would leave \p baseDir (".." components, an absolute path, a symbolic link pointing outside).
std::string resolvePathUnderDirectory(const std::string &baseDir, const std::string &relativePath);
bool changeGroup(const std::string &groupName, const std::string &fileName);
bool changeFilePermission(const std::string &permission, const std::string &fileName);
bool changeFolderPermission(const std::string &permission, const std::string &folderName);
bool changeFolderGroup(const std::string &groupName, const std::string &folderName);
bool setGroupID(const std::string &folderName);

void write_kernel_log(const char *message);
void bt_ready_msg2kernel(void);
#endif // UTILS_H
