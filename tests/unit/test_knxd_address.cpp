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

#include <cstdint>
#include <vector>

#include "knxd/knxd_protocol.h"

using namespace cvknxd;

// Additional edge case tests for the protocol layer

TEST(KnxAddressEdgeCases, TrailingGarbage) {
  EXPECT_FALSE(KnxAddress::from_cometvisu("KNX:1/2/3extra").has_value());
}

TEST(KnxAddressEdgeCases, EmptyNamespace) {
  auto addr = KnxAddress::from_cometvisu(":1/2/3");
  ASSERT_TRUE(addr.has_value());
  EXPECT_EQ(addr->ns, "");
  EXPECT_EQ(addr->group.main, 1);
}

TEST(KnxAddressEdgeCases, LargeSubAddress) {
  auto addr = KnxGroupAddress::from_string("0/0/255");
  ASSERT_TRUE(addr.has_value());
  EXPECT_EQ(addr->sub, 255);
}

TEST(ApduEdgeCases, EmptyData) {
  auto apdu = build_apdu(ApduType::Read, {});
  ASSERT_EQ(apdu.size(), 2);
}

TEST(ApduEdgeCases, MaxSingleByteValue) {
  auto apdu = build_apdu(ApduType::Write, {0x3F});
  ASSERT_EQ(apdu.size(), 2);
  EXPECT_EQ(apdu[1], 0x80 | (0x3F & 0x3F));
}

TEST(EibdMessageEdgeCases, ZeroLengthPayload) {
  // Generic builder edge case: message with type but no extra data (payload = type only)
  auto msg = build_eibd_message(EibMessageType::GROUP_PACKET, {});
  // payload = 2 bytes (type only)
  EXPECT_EQ(msg[0], 0x00);
  EXPECT_EQ(msg[1], 0x02);
  EXPECT_EQ(msg[2], static_cast<uint8_t>((EibMessageType::GROUP_PACKET >> 8) & 0xFF));
  EXPECT_EQ(msg[3], static_cast<uint8_t>(EibMessageType::GROUP_PACKET & 0xFF));
  EXPECT_EQ(msg.size(), 4);
}

TEST(EibdMessageEdgeCases, MaxData) {
  std::vector<uint8_t> data(255, 0x42);
  auto msg = build_eibd_message(EibMessageType::GROUP_PACKET, data);
  EXPECT_EQ(msg[0], static_cast<uint8_t>(((2 + 255) >> 8) & 0xFF));
  EXPECT_EQ(msg[1], static_cast<uint8_t>((2 + 255) & 0xFF));
}

TEST(KnxGroupAddressEquality, Same) {
  KnxGroupAddress a{1, 2, 3};
  KnxGroupAddress b{1, 2, 3};
  EXPECT_EQ(a, b);
}

TEST(KnxGroupAddressEquality, Different) {
  KnxGroupAddress a{1, 2, 3};
  KnxGroupAddress b{1, 2, 4};
  EXPECT_NE(a, b);
}
