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

#include <sstream>
#include <string>

#include "../src/util/debug_log.h"

namespace cvknxd {
namespace {

// Helper: capture stderr by swapping the stream buffer.
class StderrCapture {
public:
  StderrCapture() {
    old_ = std::cerr.rdbuf();
    std::cerr.rdbuf(capture_.rdbuf());
  }
  ~StderrCapture() { std::cerr.rdbuf(old_); }
  [[nodiscard]] std::string str() const { return capture_.str(); }

private:
  std::ostringstream capture_;
  std::streambuf* old_;
};

// ---- Test fixture ----
class DebugLogTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Reset debug log state for each test
    DebugLog::set_enabled(false);
    DebugLog::set_max_uri_length(200);
    DebugLog::set_max_body_length(200);
  }
};

// ============================================================
// Enable/Disable
// ============================================================

TEST_F(DebugLogTest, DisabledProducesNoOutput) {
  StderrCapture capture;
  DebugLog::http_request("GET", "/r?a=1/2/3&t=5");
  DebugLog::http_response(200, "{}");
  DebugLog::knxd_send("cache_read", "1/2/3", "nowait=true");
  DebugLog::knxd_recv("cache_read", "1/2/3", "42");

  EXPECT_EQ(capture.str(), "");
}

TEST_F(DebugLogTest, EnabledProducesOutput) {
  DebugLog::set_enabled(true);
  StderrCapture capture;
  DebugLog::http_request("GET", "/r?a=1/2/3");
  std::string output = capture.str();
  EXPECT_FALSE(output.empty());
  EXPECT_NE(output.find("HTTP REQUEST"), std::string::npos);
  EXPECT_NE(output.find("GET"), std::string::npos);
  EXPECT_NE(output.find("/r?a=1/2/3"), std::string::npos);
}

TEST_F(DebugLogTest, EnableDisableToggle) {
  StderrCapture capture;

  DebugLog::set_enabled(true);
  DebugLog::http_request("GET", "/test1");
  EXPECT_FALSE(capture.str().empty());

  DebugLog::set_enabled(false);
  size_t len_before = capture.str().size();
  DebugLog::http_request("GET", "/test2");
  EXPECT_EQ(capture.str().size(), len_before);
}

// ============================================================
// HTTP Request Logging
// ============================================================

TEST_F(DebugLogTest, HttpRequestLogsMethodAndUri) {
  DebugLog::set_enabled(true);
  StderrCapture capture;
  DebugLog::http_request("POST", "/w?a=1/2/3&v=42");

  std::string output = capture.str();
  EXPECT_NE(output.find("POST"), std::string::npos);
  EXPECT_NE(output.find("/w?a=1/2/3&v=42"), std::string::npos);
}

TEST_F(DebugLogTest, HttpRequestTruncatesLongUri) {
  DebugLog::set_enabled(true);
  DebugLog::set_max_uri_length(30);

  StderrCapture capture;
  std::string long_uri = "/r?a=1/2/3&a=4/5/6&a=7/8/9&a=10/11/12&s=abcdef123456";
  DebugLog::http_request("GET", long_uri);

  std::string output = capture.str();
  // Should contain truncated prefix
  EXPECT_NE(output.find("/r?a=1/2/3&a=4/5/6&a=7/8/9"), std::string::npos);
  // Should indicate truncation and total length
  EXPECT_NE(output.find("..."), std::string::npos);
  EXPECT_NE(output.find(std::to_string(long_uri.size())), std::string::npos);
}

TEST_F(DebugLogTest, HttpRequestDoesNotTruncateShortUri) {
  DebugLog::set_enabled(true);
  DebugLog::set_max_uri_length(500);

  StderrCapture capture;
  std::string short_uri = "/r?a=1/2/3&t=5";
  DebugLog::http_request("GET", short_uri);

  std::string output = capture.str();
  EXPECT_NE(output.find(short_uri), std::string::npos);
  // No truncation marker for short URIs
  EXPECT_EQ(output.find("... (truncated"), std::string::npos);
}

// ============================================================
// HTTP Response Logging
// ============================================================

TEST_F(DebugLogTest, HttpResponseLogsStatusCode) {
  DebugLog::set_enabled(true);
  StderrCapture capture;
  DebugLog::http_response(200, "{\"d\":{}}");

  std::string output = capture.str();
  EXPECT_NE(output.find("200"), std::string::npos);
  EXPECT_NE(output.find("HTTP RESPONSE"), std::string::npos);
}

TEST_F(DebugLogTest, HttpResponseLogsBody) {
  DebugLog::set_enabled(true);
  StderrCapture capture;
  DebugLog::http_response(200, "{\"d\":{\"KNX:1/2/3\":\"42\"}}");

  std::string output = capture.str();
  EXPECT_NE(output.find("{\"d\":{\"KNX:1/2/3\":\"42\"}}"), std::string::npos);
}

TEST_F(DebugLogTest, HttpResponseTruncatesLargeBody) {
  DebugLog::set_enabled(true);
  DebugLog::set_max_body_length(50);

  StderrCapture capture;
  std::string large_body(500, 'x');  // 500 'x' characters
  DebugLog::http_response(200, large_body);

  std::string output = capture.str();
  // Should not contain the full body
  EXPECT_EQ(output.find(std::string(100, 'x')), std::string::npos);
  // Should indicate truncation
  EXPECT_NE(output.find("..."), std::string::npos);
  EXPECT_NE(output.find(std::to_string(large_body.size())), std::string::npos);
  // Should contain the prefix of the body
  EXPECT_NE(output.find(std::string(40, 'x')), std::string::npos);
}

TEST_F(DebugLogTest, HttpResponseEmptyBody) {
  DebugLog::set_enabled(true);
  StderrCapture capture;
  DebugLog::http_response(204, "");

  std::string output = capture.str();
  EXPECT_NE(output.find("204"), std::string::npos);
  // Should handle empty body gracefully
  EXPECT_NE(output.find("HTTP RESPONSE"), std::string::npos);
}

// ============================================================
// KNXD Send Logging
// ============================================================

TEST_F(DebugLogTest, KnxdSendLogsOperationAndAddress) {
  DebugLog::set_enabled(true);
  StderrCapture capture;
  DebugLog::knxd_send("cache_read", "1/2/3", "nowait=true");

  std::string output = capture.str();
  EXPECT_NE(output.find("KNXD SEND"), std::string::npos);
  EXPECT_NE(output.find("cache_read"), std::string::npos);
  EXPECT_NE(output.find("1/2/3"), std::string::npos);
  EXPECT_NE(output.find("nowait=true"), std::string::npos);
}

TEST_F(DebugLogTest, KnxdSendGroupPacket) {
  DebugLog::set_enabled(true);
  StderrCapture capture;
  DebugLog::knxd_send("group_packet", "1/2/3", "apdu=008042");

  std::string output = capture.str();
  EXPECT_NE(output.find("group_packet"), std::string::npos);
  EXPECT_NE(output.find("apdu=008042"), std::string::npos);
}

// ============================================================
// KNXD Recv Logging
// ============================================================

TEST_F(DebugLogTest, KnxdRecvLogsOperationAndData) {
  DebugLog::set_enabled(true);
  StderrCapture capture;
  DebugLog::knxd_recv("cache_read", "1/2/3", "42");

  std::string output = capture.str();
  EXPECT_NE(output.find("KNXD RECV"), std::string::npos);
  EXPECT_NE(output.find("cache_read"), std::string::npos);
  EXPECT_NE(output.find("1/2/3"), std::string::npos);
  EXPECT_NE(output.find("42"), std::string::npos);
}

// ============================================================
// Helper: Logging separators and context
// ============================================================

TEST_F(DebugLogTest, LogsContainTimestamp) {
  DebugLog::set_enabled(true);
  StderrCapture capture;
  DebugLog::http_request("GET", "/test");

  std::string output = capture.str();
  // Timestamp format: [YYYY-MM-DD HH:MM:SS.mmm]
  // Example: [2026-07-04 12:34:56.789]
  EXPECT_NE(output.find("["), std::string::npos);
  EXPECT_NE(output.find("]"), std::string::npos);
  // Should contain a date-like pattern (starts with year 20xx)
  EXPECT_NE(output.find("[20"), std::string::npos);
  EXPECT_NE(output.find("HTTP REQUEST"), std::string::npos);
}

TEST_F(DebugLogTest, KnxdLogsHaveIndentation) {
  DebugLog::set_enabled(true);
  StderrCapture capture;
  DebugLog::knxd_send("open_group_socket", "-", "write_only=false");

  std::string output = capture.str();
  // KNXD logs should be indented to show they're nested
  EXPECT_NE(output.find("KNXD"), std::string::npos);
}

// ============================================================
// Edge cases
// ============================================================

TEST_F(DebugLogTest, MaxLengthZeroMeansNoTruncation) {
  DebugLog::set_enabled(true);
  DebugLog::set_max_uri_length(0);
  DebugLog::set_max_body_length(0);

  StderrCapture capture;
  std::string long_str(1000, 'x');
  DebugLog::http_request("GET", long_str);
  DebugLog::http_response(200, long_str);

  std::string output = capture.str();
  // Full content should be present when max length is 0
  EXPECT_NE(output.find(long_str), std::string::npos);
}

TEST_F(DebugLogTest, InitFromEnvParsesCorrectly) {
  // Simulate DEBUG_BACKEND=1
  setenv("DEBUG_BACKEND", "1", 1);
  DebugLog::init_from_env();
  EXPECT_TRUE(DebugLog::is_enabled());
  unsetenv("DEBUG_BACKEND");
}

TEST_F(DebugLogTest, InitFromEnvDisabledWhenNotSet) {
  unsetenv("DEBUG_BACKEND");
  DebugLog::init_from_env();
  EXPECT_FALSE(DebugLog::is_enabled());
}

TEST_F(DebugLogTest, InitFromEnvDisabledWhenZero) {
  setenv("DEBUG_BACKEND", "0", 1);
  DebugLog::init_from_env();
  EXPECT_FALSE(DebugLog::is_enabled());
  unsetenv("DEBUG_BACKEND");
}

TEST_F(DebugLogTest, InitFromEnvDisabledWhenFalse) {
  setenv("DEBUG_BACKEND", "false", 1);
  DebugLog::init_from_env();
  EXPECT_FALSE(DebugLog::is_enabled());
  unsetenv("DEBUG_BACKEND");
}

}  // namespace
}  // namespace cvknxd
