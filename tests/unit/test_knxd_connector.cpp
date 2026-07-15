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

#include "../../mocks/mock_knxd_socket.h"
#include "../../src/knxd/knxd_connector.h"

namespace cvknxd {
namespace {

// Use 1ms delays in tests to avoid waiting for real retry timers.
const int kTestDelaysMs[] = {1, 1, 1, 1, 1};

TEST(KnxdConnectorTest, FirstAttemptSucceeds) {
  MockKnxdClient mock;
  mock.set_connection_success(true);

  bool result = connect_knxd_with_retry(mock, "/run/knx", false, kTestDelaysMs);

  EXPECT_TRUE(result);
}

TEST(KnxdConnectorTest, SecondAttemptSucceedsAfterOneFail) {
  MockKnxdClient mock;
  mock.set_connection_success(true);
  mock.set_connect_fail_count(1);  // first call fails, second succeeds

  bool result = connect_knxd_with_retry(mock, "/run/knx", false, kTestDelaysMs);

  EXPECT_TRUE(result);
}

TEST(KnxdConnectorTest, FailsAfterAllStandardRetries) {
  MockKnxdClient mock;
  mock.set_connection_success(true);
  // Standard: 4 attempts total (1 immediate + 3 retries)
  mock.set_connect_fail_count(10);

  bool result = connect_knxd_with_retry(mock, "/run/knx", false, kTestDelaysMs);

  EXPECT_FALSE(result);
}

TEST(KnxdConnectorTest, FailsAfterAllExtendedRetries) {
  MockKnxdClient mock;
  mock.set_connection_success(true);
  // Extended: 6 attempts total (1 immediate + 5 retries)
  mock.set_connect_fail_count(10);

  bool result = connect_knxd_with_retry(mock, "/run/knx", true, kTestDelaysMs);

  EXPECT_FALSE(result);
}

TEST(KnxdConnectorTest, ExtendedRetriesThreeFailsThenSuccess) {
  MockKnxdClient mock;
  mock.set_connection_success(true);
  mock.set_connect_fail_count(3);  // 3 fails, 4th attempt succeeds

  bool result = connect_knxd_with_retry(mock, "/run/knx", true, kTestDelaysMs);

  EXPECT_TRUE(result);
}

TEST(KnxdConnectorTest, StandardModeFailsOnFourthAttempt) {
  // With fail count = 4, standard mode (4 attempts) will fail all 4 times
  MockKnxdClient mock;
  mock.set_connection_success(true);
  mock.set_connect_fail_count(4);

  bool result = connect_knxd_with_retry(mock, "/run/knx", false, kTestDelaysMs);

  EXPECT_FALSE(result);
}

TEST(KnxdConnectorTest, ExtendedModeFailsOnSixthAttempt) {
  MockKnxdClient mock;
  mock.set_connection_success(true);
  mock.set_connect_fail_count(6);

  bool result = connect_knxd_with_retry(mock, "/run/knx", true, kTestDelaysMs);

  EXPECT_FALSE(result);
}

TEST(KnxdConnectorTest, ConnectReturnsFalseWhenConnectionFails) {
  // When the mock's connection_success_ is false, connect() always returns false
  MockKnxdClient mock;
  mock.set_connection_success(false);

  bool result = connect_knxd_with_retry(mock, "/run/knx", false, kTestDelaysMs);

  EXPECT_FALSE(result);
}

}  // namespace
}  // namespace cvknxd
