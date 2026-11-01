// Copyright (C) 2026 Christian Mayer and the CometVisu contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <gtest/gtest.h>

#include "version.h"

using namespace cvknxd;

TEST(VersionTest, ReturnsExpectedVersion) {
  EXPECT_EQ(version(), "1.1.0");
}

TEST(VersionTest, ApplicationName) {
  EXPECT_EQ(application_name(), "cometvisu-knxd-fcgi");
}

TEST(VersionTest, VersionStringNotEmpty) {
  EXPECT_FALSE(version().empty());
}

TEST(VersionTest, GitHashNotEmpty) {
  EXPECT_FALSE(git_hash().empty());
}

TEST(VersionTest, GitHashIsShortHash) {
  // git short hash is typically 7 hex chars
  std::string_view hash = git_hash();
  if (hash != "unknown") {
    EXPECT_GE(hash.size(), 7);
    EXPECT_LE(hash.size(), 8);
  }
}

TEST(VersionTest, KnxdBuildVersionIsEmptyOrValid) {
  // knxd_build_version may be empty (not available) or a valid version string
  std::string_view v = knxd_build_version();
  if (!v.empty()) {
    EXPECT_NE(v.find('.'), std::string_view::npos);  // should contain a dot
  }
}

TEST(VersionTest, KnxdBuildGitHashIsEmptyOrValid) {
  // knxd_build_git_hash is typically empty or a short hash
  std::string_view hash = knxd_build_git_hash();
  if (!hash.empty()) {
    EXPECT_GE(hash.size(), 7);
  }
}
