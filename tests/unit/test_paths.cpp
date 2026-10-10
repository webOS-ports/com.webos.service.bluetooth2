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

// File names from Luna clients are joined to the storage directory: none of them may name anything outside
// it, by "..", an absolute path or a symbolic link.

#include <gtest/gtest.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "utils.h"

namespace {

class StorageDir : public ::testing::Test
{
protected:
	void SetUp() override
	{
		gchar *dir = g_dir_make_tmp("bt2-paths-XXXXXX", nullptr);
		ASSERT_NE(dir, nullptr);
		root = dir;
		g_free(dir);

		base = root + "/storage";
		outside = root + "/outside";
		ASSERT_EQ(g_mkdir_with_parents((base + "/sub").c_str(), 0700), 0);
		ASSERT_EQ(g_mkdir_with_parents(outside.c_str(), 0700), 0);
		writeFile(base + "/file.txt");
		writeFile(outside + "/secret");
	}

	void TearDown() override
	{
		std::string cmd = "rm -rf '" + root + "'";
		ASSERT_EQ(system(cmd.c_str()), 0);
	}

	static void writeFile(const std::string &path)
	{
		ASSERT_TRUE(g_file_set_contents(path.c_str(), "x", 1, nullptr));
	}

	std::string root, base, outside;
};

} // namespace

TEST(ContainedRelativePath, AcceptsPlainRelativePaths)
{
	EXPECT_TRUE(isContainedRelativePath("file.vcf"));
	EXPECT_TRUE(isContainedRelativePath("dir/file.vcf"));
	EXPECT_TRUE(isContainedRelativePath("dir/..file"));
	EXPECT_TRUE(isContainedRelativePath("a//b"));
}

TEST(ContainedRelativePath, RejectsEscapes)
{
	EXPECT_FALSE(isContainedRelativePath(""));
	EXPECT_FALSE(isContainedRelativePath("/etc/shadow"));
	EXPECT_FALSE(isContainedRelativePath(".."));
	EXPECT_FALSE(isContainedRelativePath("../../etc/shadow"));
	EXPECT_FALSE(isContainedRelativePath("dir/../../x"));
	EXPECT_FALSE(isContainedRelativePath("dir/.."));
	EXPECT_FALSE(isContainedRelativePath("."));
	EXPECT_FALSE(isContainedRelativePath("./file"));
}

TEST(SinglePathComponent, OnlyAFolderName)
{
	EXPECT_TRUE(isSinglePathComponent("telecom"));
	EXPECT_TRUE(isSinglePathComponent("pb"));
	EXPECT_FALSE(isSinglePathComponent(""));
	EXPECT_FALSE(isSinglePathComponent("."));
	EXPECT_FALSE(isSinglePathComponent(".."));
	EXPECT_FALSE(isSinglePathComponent("a/b"));
	EXPECT_FALSE(isSinglePathComponent("../../../var/lib/x"));
}

TEST_F(StorageDir, ResolvesExistingAndNewFilesInside)
{
	char *realBase = realpath(base.c_str(), nullptr);
	ASSERT_NE(realBase, nullptr);
	std::string expectedBase = realBase;
	free(realBase);

	EXPECT_EQ(resolvePathUnderDirectory(base, "file.txt"), expectedBase + "/file.txt");
	EXPECT_EQ(resolvePathUnderDirectory(base, "sub/new.vcf"), expectedBase + "/sub/new.vcf");
	EXPECT_EQ(resolvePathUnderDirectory(base, "new.vcf"), expectedBase + "/new.vcf");
}

TEST_F(StorageDir, RejectsTraversal)
{
	EXPECT_EQ(resolvePathUnderDirectory(base, "../outside/secret"), "");
	EXPECT_EQ(resolvePathUnderDirectory(base, outside + "/secret"), "");
	EXPECT_EQ(resolvePathUnderDirectory(base, "missing/new.vcf"), "");
}

TEST_F(StorageDir, RejectsLinksLeadingOutside)
{
	// The storage directory is writable by users: a link in it must not lead out.
	ASSERT_EQ(symlink(outside.c_str(), (base + "/escape").c_str()), 0);
	ASSERT_EQ(symlink((outside + "/secret").c_str(), (base + "/secret-link").c_str()), 0);
	ASSERT_EQ(symlink((outside + "/not-there").c_str(), (base + "/dangling").c_str()), 0);

	EXPECT_EQ(resolvePathUnderDirectory(base, "escape/secret"), "");
	EXPECT_EQ(resolvePathUnderDirectory(base, "escape/new.vcf"), "");
	EXPECT_EQ(resolvePathUnderDirectory(base, "secret-link"), "");
	// Writing through a dangling link would create the file outside.
	EXPECT_EQ(resolvePathUnderDirectory(base, "dangling"), "");
}

TEST_F(StorageDir, LinksInsideStayAllowed)
{
	ASSERT_EQ(symlink((base + "/sub").c_str(), (base + "/sub-link").c_str()), 0);
	EXPECT_NE(resolvePathUnderDirectory(base, "sub-link/new.vcf"), "");
}

TEST_F(StorageDir, CheckFileIsValidWantsARegularFile)
{
	EXPECT_TRUE(checkFileIsValid(base + "/file.txt"));
	EXPECT_FALSE(checkFileIsValid(base + "/sub"));
	EXPECT_FALSE(checkFileIsValid(base + "/missing"));
	EXPECT_FALSE(checkFileIsValid(""));

	ASSERT_EQ(mkfifo((base + "/fifo").c_str(), 0600), 0);
	EXPECT_FALSE(checkFileIsValid(base + "/fifo"));

	ASSERT_EQ(symlink((base + "/file.txt").c_str(), (base + "/link").c_str()), 0);
	EXPECT_FALSE(checkFileIsValid(base + "/link"));

	EXPECT_FALSE(checkFileIsValid("/dev/zero"));
}

TEST_F(StorageDir, PermissionChangesTakeNamesLiterally)
{
	// The name went into an unquoted "sh -c" command line: the ";" ended the chmod and ran the rest.
	std::string name = base + "/a;touch pwned $(id)";
	writeFile(name);
	ASSERT_EQ(chmod(name.c_str(), 0644), 0);

	EXPECT_TRUE(changeFilePermission("600", name));
	struct stat st = {};
	ASSERT_EQ(stat(name.c_str(), &st), 0);
	EXPECT_EQ(st.st_mode & 0777, 0600u);
	EXPECT_FALSE(g_file_test("pwned", G_FILE_TEST_EXISTS));
}

TEST_F(StorageDir, FailedPermissionChangeIsReported)
{
	// It returned true whatever chmod made of it.
	EXPECT_FALSE(changeFilePermission("not-a-mode", base + "/file.txt"));
}
