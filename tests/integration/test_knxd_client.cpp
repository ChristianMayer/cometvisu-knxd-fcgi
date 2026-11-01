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

#include "knxd/knxd_client.h"
#include "mock_knxd_socket.h"

using namespace cvknxd;

class KnxdClientTest : public ::testing::Test {
protected:
  MockKnxdClient client_;
};

TEST_F(KnxdClientTest, ConnectAndDisconnect) {
  EXPECT_TRUE(client_.connect("/run/knx"));
  EXPECT_TRUE(client_.is_connected());
  client_.disconnect();
  EXPECT_FALSE(client_.is_connected());
}

TEST_F(KnxdClientTest, OpenGroupSocket) {
  ASSERT_TRUE(client_.connect("/run/knx"));
  EXPECT_TRUE(client_.open_group_socket(false));
  EXPECT_TRUE(client_.open_group_socket(true));
}

TEST_F(KnxdClientTest, SendGroupPacket) {
  ASSERT_TRUE(client_.connect("/run/knx"));
  ASSERT_TRUE(client_.open_group_socket(false));
  std::vector<uint8_t> apdu = {0x00, 0x80, 0x42};
  EXPECT_TRUE(client_.send_group_packet(0x0A03, apdu));
  auto sent = client_.sent_packets();
  ASSERT_EQ(sent.size(), 1);
  EXPECT_EQ(sent[0].group_addr, 0x0A03);
  EXPECT_EQ(sent[0].apdu, apdu);
}

TEST_F(KnxdClientTest, PollGroupTelegram) {
  ASSERT_TRUE(client_.connect("/run/knx"));
  ASSERT_TRUE(client_.open_group_socket(false));
  client_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});
  uint16_t addr = 0;
  std::vector<uint8_t> apdu;
  EXPECT_TRUE(client_.poll_group_telegram(addr, apdu));
  EXPECT_EQ(addr, 0x0A03);
  EXPECT_EQ(apdu.size(), 3);
  EXPECT_EQ(client_.get_telegram_count(), 1);
}

TEST_F(KnxdClientTest, WaitForActivity) {
  ASSERT_TRUE(client_.connect("/run/knx"));
  ASSERT_TRUE(client_.open_group_socket(false));
  // No telegrams enqueued -> Timeout
  EXPECT_EQ(client_.wait_for_activity(10), KnxdClientInterface::WaitResult::Timeout);
  // Enqueue -> GroupData
  client_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});
  EXPECT_EQ(client_.wait_for_activity(10), KnxdClientInterface::WaitResult::GroupData);
}

TEST_F(KnxdClientTest, Reconnect) {
  ASSERT_TRUE(client_.connect("/run/knx"));
  ASSERT_TRUE(client_.open_group_socket(false));
  EXPECT_TRUE(client_.reconnect());
  EXPECT_TRUE(client_.is_connected());
}

TEST_F(KnxdClientTest, SendFailsWhenDisconnected) {
  std::vector<uint8_t> apdu = {0x00, 0x80, 0x42};
  EXPECT_FALSE(client_.send_group_packet(0x0A03, apdu));
}

TEST_F(KnxdClientTest, SetNonblocking) {
  ASSERT_TRUE(client_.connect("/run/knx"));
  client_.set_nonblocking(true);  // no-op in mock, just verify no crash
  client_.set_nonblocking(false);
}

TEST_F(KnxdClientTest, GetFd) {
  EXPECT_EQ(client_.get_fd(), -1);  // mock returns -1
  ASSERT_TRUE(client_.connect("/run/knx"));
  EXPECT_EQ(client_.get_fd(), -1);  // still -1 in mock
}

// ================================================================
// Tests adapted from original mock coverage — no intent lost
// ================================================================

TEST_F(KnxdClientTest, ConnectFailure) {
  client_.set_connection_success(false);
  EXPECT_FALSE(client_.connect("/run/knx"));
  EXPECT_FALSE(client_.is_connected());
}

TEST_F(KnxdClientTest, OpenGroupSocketNotConnected) {
  EXPECT_FALSE(client_.open_group_socket(false));
}

TEST_F(KnxdClientTest, DisconnectClearsState) {
  ASSERT_TRUE(client_.connect("/run/knx"));
  ASSERT_TRUE(client_.open_group_socket(false));
  client_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});
  client_.disconnect();
  EXPECT_FALSE(client_.is_connected());
  // After disconnect, poll should return false (not connected)
  uint16_t addr = 0;
  std::vector<uint8_t> apdu;
  EXPECT_FALSE(client_.poll_group_telegram(addr, apdu));
  // wait_for_activity must report the lost connection instead of a plain
  // timeout — otherwise the cache reader spins at 100% CPU.
  EXPECT_EQ(client_.wait_for_activity(1), KnxdClientInterface::WaitResult::Error);
}

TEST_F(KnxdClientTest, ResetClearsState) {
  ASSERT_TRUE(client_.connect("/run/knx"));
  ASSERT_TRUE(client_.open_group_socket(false));
  client_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});
  (void)client_.send_group_packet(0x0B04, {0x00, 0x80, 0x01});
  client_.reset();
  EXPECT_FALSE(client_.is_connected());
  EXPECT_EQ(client_.sent_packets().size(), 0);
  EXPECT_EQ(client_.get_telegram_count(), 0);
}

TEST_F(KnxdClientTest, ReconnectWithoutInitialConnect) {
  EXPECT_FALSE(client_.reconnect());  // never connected → fails
}

TEST_F(KnxdClientTest, SendFailsWhenNotConnected) {
  std::vector<uint8_t> apdu = {0x00, 0x80, 0x42};
  EXPECT_FALSE(client_.send_group_packet(0x0A03, apdu));
  EXPECT_TRUE(client_.sent_packets().empty());
}

TEST_F(KnxdClientTest, PollTelegramReturnsFalseWhenEmpty) {
  ASSERT_TRUE(client_.connect("/run/knx"));
  ASSERT_TRUE(client_.open_group_socket(false));
  uint16_t addr = 0;
  std::vector<uint8_t> apdu;
  EXPECT_FALSE(client_.poll_group_telegram(addr, apdu));  // queue empty
}

TEST_F(KnxdClientTest, MultiplePollsDrainAll) {
  ASSERT_TRUE(client_.connect("/run/knx"));
  ASSERT_TRUE(client_.open_group_socket(false));
  client_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});
  client_.enqueue_telegram(0x0B04, {0x00, 0x80, 0x01});
  client_.enqueue_telegram(0x0C05, {0x00, 0x80, 0xFF});
  uint16_t addr = 0;
  std::vector<uint8_t> apdu;
  int count = 0;
  while (client_.poll_group_telegram(addr, apdu))
    count++;
  EXPECT_EQ(count, 3);
  EXPECT_EQ(client_.get_telegram_count(), 3);
}

TEST_F(KnxdClientTest, SendGroupPacketFailsWhenGroupSocketNotOpen) {
  ASSERT_TRUE(client_.connect("/run/knx"));
  // group socket NOT opened
  std::vector<uint8_t> apdu = {0x00, 0x80, 0x42};
  EXPECT_FALSE(client_.send_group_packet(0x0A03, apdu));
}

TEST_F(KnxdClientTest, OperationsWorkAfterReconnect) {
  ASSERT_TRUE(client_.connect("/run/knx"));
  ASSERT_TRUE(client_.open_group_socket(false));
  EXPECT_TRUE(client_.reconnect());
  // After reconnect, group socket must be re-opened and operations work
  std::vector<uint8_t> apdu = {0x00, 0x80, 0x42};
  EXPECT_TRUE(client_.send_group_packet(0x0A03, apdu));
  EXPECT_EQ(client_.sent_packets().size(), 1);
}

// ================================================================
// Transformed cache tests → GroupCache tests
