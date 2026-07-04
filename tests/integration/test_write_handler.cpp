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

#include "handlers/write_handler.h"
#include "knxd/knxd_protocol.h"
#include "mock_knxd_socket.h"
#include "state/session_store.h"

using namespace cvknxd;

class WriteHandlerTest : public ::testing::Test {
protected:
  void SetUp() override {
    (void)knxd_.connect("/run/knx");
    (void)knxd_.open_group_socket(false);
  }

  MockKnxdClient knxd_;
  SessionStore sessions_;
};

TEST_F(WriteHandlerTest, WriteSingleAddress) {
  WriteHandler handler(knxd_, sessions_);

  auto result = handler.handle("a=KNX:1/2/3&v=42");

  EXPECT_EQ(result.http_status, 200);

  auto sent = knxd_.sent_packets();
  ASSERT_EQ(sent.size(), 1);
  EXPECT_EQ(sent[0].group_addr, 0x0A03);  // 1/2/3

  // Check APDU: 0x00 0x80|(0x42&0x3F) (Write + packed value)
  ASSERT_EQ(sent[0].apdu.size(), 2);
  EXPECT_EQ(sent[0].apdu[0], 0x00);
  EXPECT_EQ(sent[0].apdu[1], 0x80 | (0x42 & 0x3F));
}

TEST_F(WriteHandlerTest, WriteMultiByteValue) {
  WriteHandler handler(knxd_, sessions_);

  auto result = handler.handle("a=KNX:1/2/3&v=0c6f");

  EXPECT_EQ(result.http_status, 200);

  auto sent = knxd_.sent_packets();
  ASSERT_EQ(sent.size(), 1);
  ASSERT_EQ(sent[0].apdu.size(), 4);
  EXPECT_EQ(sent[0].apdu[0], 0x00);
  EXPECT_EQ(sent[0].apdu[1], 0x80);  // Write, multi-byte
  EXPECT_EQ(sent[0].apdu[2], 0x0C);
  EXPECT_EQ(sent[0].apdu[3], 0x6F);
}

TEST_F(WriteHandlerTest, WriteMultipleAddresses) {
  WriteHandler handler(knxd_, sessions_);

  auto result = handler.handle("a=KNX:1/2/3&a=KNX:4/5/6&v=42");

  EXPECT_EQ(result.http_status, 200);

  auto sent = knxd_.sent_packets();
  ASSERT_EQ(sent.size(), 2);
  EXPECT_EQ(sent[0].group_addr, 0x0A03);  // 1/2/3
  EXPECT_EQ(sent[1].group_addr, 0x2506);  // 4/5/6
}

TEST_F(WriteHandlerTest, MissingAddress) {
  WriteHandler handler(knxd_, sessions_);
  auto result = handler.handle("v=42");
  EXPECT_EQ(result.http_status, 400);
  EXPECT_TRUE(knxd_.sent_packets().empty());
}

TEST_F(WriteHandlerTest, MissingValue) {
  WriteHandler handler(knxd_, sessions_);
  auto result = handler.handle("a=KNX:1/2/3");
  EXPECT_EQ(result.http_status, 400);
  EXPECT_TRUE(knxd_.sent_packets().empty());
}

TEST_F(WriteHandlerTest, InvalidHexValue) {
  WriteHandler handler(knxd_, sessions_);
  auto result = handler.handle("a=KNX:1/2/3&v=ZZ");
  EXPECT_EQ(result.http_status, 400);
  EXPECT_TRUE(knxd_.sent_packets().empty());
}

TEST_F(WriteHandlerTest, InvalidAddress) {
  WriteHandler handler(knxd_, sessions_);
  auto result = handler.handle("a=invalid&v=42");
  EXPECT_EQ(result.http_status, 404);
  EXPECT_TRUE(knxd_.sent_packets().empty());
}

TEST_F(WriteHandlerTest, WriteDoesNotNeedLocalCache) {
  // After removing AddressCache, writes just send the packet.
  // knxd's built-in cache handles storage.
  WriteHandler handler(knxd_, sessions_);

  auto result = handler.handle("a=KNX:1/2/3&v=0c6f");
  EXPECT_EQ(result.http_status, 200);

  auto sent = knxd_.sent_packets();
  ASSERT_EQ(sent.size(), 1);
  EXPECT_EQ(sent[0].group_addr, 0x0A03);
}

TEST_F(WriteHandlerTest, DefaultNamespace) {
  WriteHandler handler(knxd_, sessions_);

  auto result = handler.handle("a=1/2/3&v=42");

  EXPECT_EQ(result.http_status, 200);
  auto sent = knxd_.sent_packets();
  ASSERT_EQ(sent.size(), 1);
  EXPECT_EQ(sent[0].group_addr, 0x0A03);
}

TEST_F(WriteHandlerTest, SessionInvalidReturns401) {
  (void)sessions_.create_session(false);

  WriteHandler handler(knxd_, sessions_);
  auto result = handler.handle("a=KNX:1/2/3&v=42&s=nonexistent");
  EXPECT_EQ(result.http_status, 401);
  EXPECT_TRUE(knxd_.sent_packets().empty());
}

TEST_F(WriteHandlerTest, AnonymousSessionOk) {
  WriteHandler handler(knxd_, sessions_);
  auto result = handler.handle("a=KNX:1/2/3&v=42&s=0");
  EXPECT_EQ(result.http_status, 200);
  EXPECT_FALSE(knxd_.sent_packets().empty());
}

TEST_F(WriteHandlerTest, ValidSessionWrites) {
  auto sid = sessions_.create_session(false);

  WriteHandler handler(knxd_, sessions_);
  auto result = handler.handle("a=KNX:1/2/3&v=42&s=" + sid);
  EXPECT_EQ(result.http_status, 200);
  EXPECT_FALSE(knxd_.sent_packets().empty());
}
