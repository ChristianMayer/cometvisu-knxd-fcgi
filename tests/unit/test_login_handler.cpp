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

#include <cstdlib>

#include "handlers/login_handler.h"
#include "state/session_store.h"

using namespace cvknxd;

namespace {

/// Helper: set an environment variable for the duration of a test case.
class ScopedEnvVar {
public:
  ScopedEnvVar(const char* name, const char* value) : name_(name) {
    if (value != nullptr) {
      setenv(name, value, 1);
    } else {
      unsetenv(name);
    }
  }
  ~ScopedEnvVar() { unsetenv(name_); }

private:
  const char* name_;
};

}  // namespace

TEST(LoginHandlerTest, AnonymousLogin) {
  SessionStore sessions;
  LoginHandler handler(sessions);

  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"v\":\"0.0.2\""), std::string::npos);
  EXPECT_NE(response.find("\"s\":\"0\""), std::string::npos);
}

TEST(LoginHandlerTest, AuthenticatedLogin) {
  SessionStore sessions;
  LoginHandler handler(sessions);

  std::string response = handler.handle("u=admin&p=secret");
  EXPECT_NE(response.find("\"v\":\"0.0.2\""), std::string::npos);
  // Session ID must NOT be "0"
  EXPECT_EQ(response.find("\"s\":\"0\""), std::string::npos);
  // Session ID should be a non-empty hex string
  auto spos = response.find("\"s\":\"");
  ASSERT_NE(spos, std::string::npos);
  auto epos = response.find('"', spos + 5);
  ASSERT_NE(epos, std::string::npos);
  std::string sid = response.substr(spos + 5, epos - spos - 5);
  EXPECT_FALSE(sid.empty());
  EXPECT_NE(sid, "0");
}

TEST(LoginHandlerTest, AnonymousWhenNoUserOrPassword) {
  SessionStore sessions;
  LoginHandler handler(sessions);

  // No u or p params → anonymous
  std::string response = handler.handle("d=mydevice");
  EXPECT_NE(response.find("\"s\":\"0\""), std::string::npos);
}

TEST(LoginHandlerTest, ValidJsonStructure) {
  SessionStore sessions;
  LoginHandler handler(sessions);

  std::string response = handler.handle("u=test&p=test");
  EXPECT_EQ(response.front(), '{');
  EXPECT_EQ(response.back(), '}');
  // Must have exactly v and s keys
  EXPECT_NE(response.find("\"v\":\"0.0.2\""), std::string::npos);
  EXPECT_NE(response.find("\"s\":"), std::string::npos);
}

TEST(LoginHandlerTest, DifferentSessionsAreUnique) {
  SessionStore sessions;
  LoginHandler handler(sessions);

  std::string r1 = handler.handle("u=a&p=b");
  std::string r2 = handler.handle("u=c&p=d");

  auto extract_sid = [](const std::string& json) -> std::string {
    auto s = json.find("\"s\":\"");
    if (s == std::string::npos)
      return "";
    auto e = json.find('"', s + 5);
    if (e == std::string::npos)
      return "";
    return json.substr(s + 5, e - s - 5);
  };

  std::string sid1 = extract_sid(r1);
  std::string sid2 = extract_sid(r2);
  EXPECT_NE(sid1, "0");
  EXPECT_NE(sid2, "0");
  EXPECT_NE(sid1, sid2);
}

TEST(LoginHandlerTest, ConfigBlockAbsentWhenNoUrlPath) {
  // Ensure CGI_URL_PATH is not set
  ScopedEnvVar clear("CGI_URL_PATH", nullptr);

  SessionStore sessions;
  LoginHandler handler(sessions);

  std::string response = handler.handle("");
  EXPECT_EQ(response.find("\"c\""), std::string::npos);
}

TEST(LoginHandlerTest, ConfigBlockPresentWhenUrlPathSet) {
  ScopedEnvVar set("CGI_URL_PATH", "/proxy/visu");

  SessionStore sessions;
  LoginHandler handler(sessions);

  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"c\""), std::string::npos);
  EXPECT_NE(response.find("\"baseURL\""), std::string::npos);
  EXPECT_NE(response.find("\"/proxy/visu\""), std::string::npos);
}

TEST(LoginHandlerTest, ConfigBlockWithCustomUrlPath) {
  ScopedEnvVar set("CGI_URL_PATH", "/custom/prefix");

  SessionStore sessions;
  LoginHandler handler(sessions);

  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"baseURL\":\"/custom/prefix\""), std::string::npos);
}

TEST(LoginHandlerTest, ConfigBlockIgnoredWhenUrlPathEmpty) {
  ScopedEnvVar set("CGI_URL_PATH", "");

  SessionStore sessions;
  LoginHandler handler(sessions);

  std::string response = handler.handle("");
  EXPECT_EQ(response.find("\"c\""), std::string::npos);
}
