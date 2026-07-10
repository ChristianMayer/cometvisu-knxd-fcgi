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
#include "knxd/knxd_protocol.h"
#include "mock_knxd_socket.h"

using namespace cvknxd;

// Integration tests for the mock KnxdClient

TEST(MockKnxdClientTest, ConnectSuccess) {
  MockKnxdClient client;
  EXPECT_TRUE(client.connect("/run/knx"));
  EXPECT_TRUE(client.is_connected());
}

TEST(MockKnxdClientTest, ConnectFailure) {
  MockKnxdClient client;
  client.set_connection_success(false);
  EXPECT_FALSE(client.connect("/run/knx"));
  EXPECT_FALSE(client.is_connected());
}

TEST(MockKnxdClientTest, OpenGroupSocket) {
  MockKnxdClient client;
  ASSERT_TRUE(client.connect("/run/knx"));
  EXPECT_TRUE(client.open_group_socket(false));
}

TEST(MockKnxdClientTest, OpenGroupSocketNotConnected) {
  MockKnxdClient client;
  EXPECT_FALSE(client.open_group_socket(false));
}

TEST(MockKnxdClientTest, SendGroupPacket) {
  MockKnxdClient client;
  ASSERT_TRUE(client.connect("/run/knx"));
  ASSERT_TRUE(client.open_group_socket(false));

  std::vector<uint8_t> apdu = {0x00, 0x80, 0x0C, 0x6F};
  EXPECT_TRUE(client.send_group_packet(0x0A03, apdu));

  auto sent = client.sent_packets();
  ASSERT_EQ(sent.size(), 1);
  EXPECT_EQ(sent[0].group_addr, 0x0A03);
  EXPECT_EQ(sent[0].apdu, apdu);
}

TEST(MockKnxdClientTest, SendGroupPacketNotConnected) {
  MockKnxdClient client;
  std::vector<uint8_t> apdu = {0x00, 0x80};
  EXPECT_FALSE(client.send_group_packet(0x0A03, apdu));
  EXPECT_TRUE(client.sent_packets().empty());
}

TEST(MockKnxdClientTest, CacheRead) {
  MockKnxdClient client;
  ASSERT_TRUE(client.connect("/run/knx"));

  std::vector<uint8_t> expected = {0x0C, 0x6F};
  client.set_cached_value(0x0A03, expected);

  auto result = client.cache_read(0x0A03, false);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, expected);
}

TEST(MockKnxdClientTest, CacheReadMiss) {
  MockKnxdClient client;
  ASSERT_TRUE(client.connect("/run/knx"));

  auto result = client.cache_read(0xFFFF, false);
  EXPECT_FALSE(result.has_value());
}

TEST(MockKnxdClientTest, PollTelegram) {
  MockKnxdClient client;
  ASSERT_TRUE(client.connect("/run/knx"));

  client.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});

  uint16_t addr;
  std::vector<uint8_t> apdu;
  EXPECT_TRUE(client.poll_group_telegram(addr, apdu));
  EXPECT_EQ(addr, 0x0A03);
  ASSERT_EQ(apdu.size(), 3);
  EXPECT_EQ(apdu[0], 0x00);
  EXPECT_EQ(apdu[1], 0x80);
  EXPECT_EQ(apdu[2], 0x42);

  // Queue should be empty now
  EXPECT_FALSE(client.poll_group_telegram(addr, apdu));
}

TEST(MockKnxdClientTest, Disconnect) {
  MockKnxdClient client;
  ASSERT_TRUE(client.connect("/run/knx"));
  EXPECT_TRUE(client.is_connected());
  client.disconnect();
  EXPECT_FALSE(client.is_connected());
  EXPECT_FALSE(client.send_group_packet(0x0A03, {0x00, 0x80}));
}

TEST(MockKnxdClientTest, Reset) {
  MockKnxdClient client;
  ASSERT_TRUE(client.connect("/run/knx"));
  client.set_cached_value(0x0A03, {0x42});
  client.enqueue_telegram(0x0A03, {0x00, 0x80});

  client.reset();

  EXPECT_FALSE(client.is_connected());
  EXPECT_FALSE(client.cache_read(0x0A03, false).has_value());

  uint16_t addr;
  std::vector<uint8_t> apdu;
  EXPECT_FALSE(client.poll_group_telegram(addr, apdu));
  EXPECT_TRUE(client.sent_packets().empty());
}

TEST(MockKnxdClientTest, ReconnectAfterDisconnect) {
  MockKnxdClient client;
  ASSERT_TRUE(client.connect("/run/knx"));
  ASSERT_TRUE(client.open_group_socket(false));
  ASSERT_TRUE(client.is_connected());

  // Simulate knxd disconnect
  client.disconnect();
  EXPECT_FALSE(client.is_connected());

  // Reconnect
  ASSERT_TRUE(client.reconnect());
  EXPECT_TRUE(client.is_connected());

  // Verify operations work after reconnect
  EXPECT_TRUE(client.open_group_socket(false));
  std::vector<uint8_t> apdu = {0x00, 0x80, 0x42};
  EXPECT_TRUE(client.send_group_packet(0x0A03, apdu));
  EXPECT_EQ(client.sent_packets().size(), 1);
}

TEST(MockKnxdClientTest, ReconnectWithoutInitialConnect) {
  MockKnxdClient client;
  // Never connected — reconnect should fail
  EXPECT_FALSE(client.reconnect());
  EXPECT_FALSE(client.is_connected());
}

TEST(MockKnxdClientTest, OperationsWorkAfterReconnect) {
  MockKnxdClient client;
  ASSERT_TRUE(client.connect("/run/knx"));
  ASSERT_TRUE(client.open_group_socket(false));

  // Set up some state
  client.set_cached_value(0x0A03, {0x42});
  client.enqueue_telegram(0x0B04, {0x00, 0x80, 0x0C, 0x6F});

  // Disconnect and reconnect
  client.disconnect();
  ASSERT_TRUE(client.reconnect());

  // cache_read should work
  auto val = client.cache_read(0x0A03, false);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, (std::vector<uint8_t>{0x42}));

  // poll_group_telegram should work
  uint16_t addr;
  std::vector<uint8_t> apdu;
  EXPECT_TRUE(client.poll_group_telegram(addr, apdu));
  EXPECT_EQ(addr, 0x0B04);
}

TEST(MockKnxdClientTest, SendGroupPacketWorksAfterReconnect) {
  MockKnxdClient client;
  ASSERT_TRUE(client.connect("/run/knx"));
  ASSERT_TRUE(client.open_group_socket(false));

  // Disconnect and reconnect
  client.disconnect();
  ASSERT_TRUE(client.reconnect());
  ASSERT_TRUE(client.open_group_socket(false));

  // send_group_packet should work after reconnect
  std::vector<uint8_t> apdu = {0x00, 0x80, 0x42};
  EXPECT_TRUE(client.send_group_packet(0x0A03, apdu));
  EXPECT_EQ(client.sent_packets().size(), 1);
}

TEST(MockKnxdClientTest, CacheReadWorksAfterReconnect) {
  MockKnxdClient client;
  ASSERT_TRUE(client.connect("/run/knx"));
  ASSERT_TRUE(client.open_group_socket(false));

  client.set_cached_value(0x0A03, {0x42});

  // Disconnect and reconnect
  client.disconnect();
  ASSERT_TRUE(client.reconnect());

  // cache_read should still find the cached value
  auto val = client.cache_read(0x0A03, false);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, (std::vector<uint8_t>{0x42}));
}

TEST(MockKnxdClientTest, CacheLastUpdatesWorksAfterReconnect) {
  MockKnxdClient client;
  ASSERT_TRUE(client.connect("/run/knx"));
  ASSERT_TRUE(client.open_group_socket(false));

  // Disconnect and reconnect
  client.disconnect();
  ASSERT_TRUE(client.reconnect());

  // cache_last_updates_2 should work after reconnect
  client.set_last_updates_result(0, {0x0A03}, 10);
  auto result = client.cache_last_updates_2(0, 5);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->new_position, 10);
  ASSERT_EQ(result->changed_addresses.size(), 1);
  EXPECT_EQ(result->changed_addresses[0], 0x0A03);
}

TEST(MockKnxdClientTest, CacheReadReturnsNulloptWhenDisconnected) {
  MockKnxdClient client;
  ASSERT_TRUE(client.connect("/run/knx"));
  client.set_cached_value(0x0A03, {0x42});

  client.disconnect();
  // cache_read should return nullopt when disconnected (no auto-reconnect in mock)
  EXPECT_FALSE(client.cache_read(0x0A03, false).has_value());
}

TEST(MockKnxdClientTest, CacheLastUpdatesReturnsNulloptWhenDisconnected) {
  MockKnxdClient client;
  ASSERT_TRUE(client.connect("/run/knx"));
  client.set_last_updates_result(0, {0x0A03}, 10);

  client.disconnect();
  EXPECT_FALSE(client.cache_last_updates_2(0, 5).has_value());
}

TEST(MockKnxdClientTest, CacheReadFailCount) {
  MockKnxdClient client;
  ASSERT_TRUE(client.connect("/run/knx"));
  client.set_cached_value(0x0A03, {0x42});

  // Set fail count: first call returns nullopt, second succeeds
  client.set_cache_read_fail_count(1);

  // First call fails (also simulates connection drop)
  EXPECT_FALSE(client.cache_read(0x0A03, false).has_value());
  // Reconnect after simulated failure (as handlers do)
  ASSERT_TRUE(client.reconnect());
  // Second call succeeds
  auto val = client.cache_read(0x0A03, false);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, (std::vector<uint8_t>{0x42}));
  // Third call also succeeds (fail count exhausted)
  val = client.cache_read(0x0A03, false);
  ASSERT_TRUE(val.has_value());
}

TEST(MockKnxdClientTest, CacheLastUpdatesFailCount) {
  MockKnxdClient client;
  ASSERT_TRUE(client.connect("/run/knx"));
  client.set_last_updates_result(0, {0x0A03}, 10);

  // Set fail count: first call returns nullopt, second succeeds
  client.set_cache_last_updates_fail_count(1);

  // First call fails (also simulates connection drop)
  EXPECT_FALSE(client.cache_last_updates_2(0, 5).has_value());
  // Reconnect after simulated failure (as handlers do)
  ASSERT_TRUE(client.reconnect());
  // Second call succeeds
  auto result = client.cache_last_updates_2(0, 5);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->new_position, 10);
}
