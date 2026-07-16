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

#include "knxd/knxd_protocol.h"

using namespace cvknxd;

// ---- Helpers for extracting bytes from knxd constants at compile time ----
// These avoid hard-coding knxd message type values (like 0x0026) in tests.
// If knxd ever renumbers its message types, the tests adapt automatically
// because the constants come from the knxd headers (<eibtypes.h>).

namespace {
constexpr uint8_t hi(uint16_t v) {
  return static_cast<uint8_t>((v >> 8) & 0xFF);
}
constexpr uint8_t lo(uint16_t v) {
  return static_cast<uint8_t>(v & 0xFF);
}
}  // namespace

// ---- KnxGroupAddress ----

TEST(KnxGroupAddressTest, FromStringValid) {
  auto addr = KnxGroupAddress::from_string("1/2/3");
  ASSERT_TRUE(addr.has_value());
  EXPECT_EQ(addr->main, 1);
  EXPECT_EQ(addr->middle, 2);
  EXPECT_EQ(addr->sub, 3);
}

TEST(KnxGroupAddressTest, FromStringMaxValues) {
  auto addr = KnxGroupAddress::from_string("31/7/255");
  ASSERT_TRUE(addr.has_value());
  EXPECT_EQ(addr->main, 31);
  EXPECT_EQ(addr->middle, 7);
  EXPECT_EQ(addr->sub, 255);
}

TEST(KnxGroupAddressTest, FromStringInvalidFormat) {
  EXPECT_FALSE(KnxGroupAddress::from_string("1/2").has_value());
  EXPECT_FALSE(KnxGroupAddress::from_string("a/b/c").has_value());
  EXPECT_FALSE(KnxGroupAddress::from_string("1/2/3/4").has_value());
  EXPECT_FALSE(KnxGroupAddress::from_string("").has_value());
}

TEST(KnxGroupAddressTest, FromStringTooLarge) {
  EXPECT_FALSE(KnxGroupAddress::from_string("32/0/0").has_value());
}

TEST(KnxGroupAddressTest, ToString) {
  KnxGroupAddress addr{1, 2, 3};
  EXPECT_EQ(addr.to_string(), "1/2/3");
}

TEST(KnxGroupAddressTest, ToEibAddr) {
  // 1/2/3 → (1 << 11) | (2 << 8) | 3 = 0x0800 | 0x0200 | 0x0003 = 0x0A03
  KnxGroupAddress addr{1, 2, 3};
  EXPECT_EQ(addr.to_eibaddr(), 0x0A03);
}

TEST(KnxGroupAddressTest, FromEibAddr) {
  auto addr = KnxGroupAddress::from_eibaddr(0x0A03);
  EXPECT_EQ(addr.main, 1);
  EXPECT_EQ(addr.middle, 2);
  EXPECT_EQ(addr.sub, 3);
}

TEST(KnxGroupAddressTest, EibAddrRoundTrip) {
  KnxGroupAddress original{15, 7, 200};
  auto addr = KnxGroupAddress::from_eibaddr(original.to_eibaddr());
  EXPECT_EQ(addr.main, original.main);
  EXPECT_EQ(addr.middle, original.middle);
  EXPECT_EQ(addr.sub, original.sub);
}

// ---- KnxAddress (CometVisu format) ----

TEST(KnxAddressTest, FromCometvisuWithNamespace) {
  auto addr = KnxAddress::from_cometvisu("KNX:1/2/3");
  ASSERT_TRUE(addr.has_value());
  EXPECT_EQ(addr->ns, "KNX");
  EXPECT_EQ(addr->group.main, 1);
  EXPECT_EQ(addr->group.middle, 2);
  EXPECT_EQ(addr->group.sub, 3);
}

TEST(KnxAddressTest, FromCometvisuDefaultNamespace) {
  // With default namespace empty, no-colon addresses get empty namespace.
  auto addr = KnxAddress::from_cometvisu("1/2/3");
  ASSERT_TRUE(addr.has_value());
  EXPECT_EQ(addr->ns, "");
  EXPECT_EQ(addr->group.main, 1);
}

TEST(KnxAddressTest, FromCometvisuInvalid) {
  EXPECT_FALSE(KnxAddress::from_cometvisu("KNX:invalid").has_value());
  EXPECT_FALSE(KnxAddress::from_cometvisu("").has_value());
}

TEST(KnxAddressTest, ToCometvisu) {
  KnxAddress addr{"KNX", KnxGroupAddress{4, 5, 6}};
  EXPECT_EQ(addr.to_cometvisu(), "KNX:4/5/6");
}

// ---- APDU ----

TEST(ApduTest, BuildWriteOneByte) {
  // Value fits in 6 bits (≤ 0x3F): packed into APCI byte.
  auto apdu = build_apdu(ApduType::Write, {0x2A});
  ASSERT_EQ(apdu.size(), 2);
  EXPECT_EQ(apdu[0], 0x00);
  EXPECT_EQ(apdu[1], 0x80 | 0x2A);  // Write + packed 6-bit value
}

TEST(ApduTest, BuildWriteOneByteAutoPromote) {
  // Value exceeds 6 bits (> 0x3F): auto-promoted to multi-byte format
  // to avoid silent data truncation.
  auto apdu = build_apdu(ApduType::Write, {0x42});
  ASSERT_EQ(apdu.size(), 3);
  EXPECT_EQ(apdu[0], 0x00);
  EXPECT_EQ(apdu[1], 0x80);  // Write type marker
  EXPECT_EQ(apdu[2], 0x42);  // Full value preserved
}

TEST(ApduTest, BuildWriteMultiByte) {
  auto apdu = build_apdu(ApduType::Write, {0x0C, 0x6F});
  ASSERT_EQ(apdu.size(), 4);
  EXPECT_EQ(apdu[0], 0x00);
  EXPECT_EQ(apdu[1], 0x80);  // Write, no packed data for multi-byte
  EXPECT_EQ(apdu[2], 0x0C);
  EXPECT_EQ(apdu[3], 0x6F);
}

TEST(ApduTest, BuildRead) {
  auto apdu = build_apdu(ApduType::Read, {});
  ASSERT_EQ(apdu.size(), 2);
  EXPECT_EQ(apdu[0], 0x00);
  EXPECT_EQ(apdu[1], 0x00);  // Read
}

TEST(ApduTest, BuildResponseOneByte) {
  auto apdu = build_apdu(ApduType::Response, {0x2A});
  ASSERT_EQ(apdu.size(), 2);
  EXPECT_EQ(apdu[0], 0x00);
  EXPECT_EQ(apdu[1], 0x40 | (0x2A & 0x3F));  // Response + packed 6-bit value
}

TEST(ApduTest, ParseWriteOneByte) {
  // Single-byte write: type bits 0x80, data in lower 6 bits
  std::vector<uint8_t> apdu = {0x00, 0x80 | (0x2A & 0x3F)};
  ApduType type;
  std::vector<uint8_t> data;
  ASSERT_TRUE(parse_apdu(apdu, type, data));
  EXPECT_EQ(type, ApduType::Write);
  ASSERT_EQ(data.size(), 1);
  EXPECT_EQ(data[0], 0x2A);
}

TEST(ApduTest, ParseWriteMultiByte) {
  std::vector<uint8_t> apdu = {0x00, 0x80, 0x0C, 0x6F};
  ApduType type;
  std::vector<uint8_t> data;
  ASSERT_TRUE(parse_apdu(apdu, type, data));
  EXPECT_EQ(type, ApduType::Write);
  ASSERT_EQ(data.size(), 2);
  EXPECT_EQ(data[0], 0x0C);
  EXPECT_EQ(data[1], 0x6F);
}

TEST(ApduTest, ParseRead) {
  std::vector<uint8_t> apdu = {0x00, 0x00};
  ApduType type;
  std::vector<uint8_t> data;
  ASSERT_TRUE(parse_apdu(apdu, type, data));
  EXPECT_EQ(type, ApduType::Read);
  EXPECT_TRUE(data.empty());
}

TEST(ApduTest, ParseResponse) {
  std::vector<uint8_t> apdu = {0x00, 0x40 | (0x1A & 0x3F)};
  ApduType type;
  std::vector<uint8_t> data;
  ASSERT_TRUE(parse_apdu(apdu, type, data));
  EXPECT_EQ(type, ApduType::Response);
  ASSERT_EQ(data.size(), 1);
  EXPECT_EQ(data[0], 0x1A);
}

// ---- EIBD Wire Format: Type-Specific Builders ----
//
// These tests verify the exact byte-level wire format of every message type
// we send to knxd.  Each test calls the dedicated builder function (the same
// one used in production code) and asserts every byte.
//
// Message type bytes are derived from knxd's <eibtypes.h> constants via the
// EibMessageType namespace — so if knxd renumbers its types, the tests adapt
// automatically.  Only the *structural* format (payload lengths, field order,
// reserved bytes) is hard-coded, because that *is* the specification we are
// testing against.

// -- EIB_OPEN_GROUPCON --

TEST(EibdWireFormatTest, OpenGroupconWriteOnly) {
  // knxd >= 0.14 expects a 5-byte payload for all EIB_OPEN_* types:
  // [type:2][reserved/addr:2][write_only:1]
  auto msg = build_open_groupcon(true);
  // Wire: [len:2][type:2][reserved:2][write_only:1] = 7 bytes
  EXPECT_EQ(msg.size(), 7);                              // 2 (len) + 5 (payload)
  EXPECT_EQ(msg[0], 0x00);                               // len hi
  EXPECT_EQ(msg[1], 0x05);                               // len lo = 5
  EXPECT_EQ(msg[2], hi(EibMessageType::OPEN_GROUPCON));  // type hi
  EXPECT_EQ(msg[3], lo(EibMessageType::OPEN_GROUPCON));  // type lo
  EXPECT_EQ(msg[4], 0x00);                               // reserved hi
  EXPECT_EQ(msg[5], 0x00);                               // reserved lo
  EXPECT_EQ(msg[6], 0x01);                               // write_only = true
}

TEST(EibdWireFormatTest, OpenGroupconReadWrite) {
  auto msg = build_open_groupcon(false);
  EXPECT_EQ(msg.size(), 7);
  EXPECT_EQ(msg[0], 0x00);
  EXPECT_EQ(msg[1], 0x05);
  EXPECT_EQ(msg[2], hi(EibMessageType::OPEN_GROUPCON));
  EXPECT_EQ(msg[3], lo(EibMessageType::OPEN_GROUPCON));
  EXPECT_EQ(msg[4], 0x00);
  EXPECT_EQ(msg[5], 0x00);
  EXPECT_EQ(msg[6], 0x00);  // write_only = false
}

// -- EIB_GROUP_PACKET --

TEST(EibdWireFormatTest, GroupPacket) {
  // Payload: [type:2][dest_addr:2][apdu:N]
  // 1/2/3 = 0x0A03, APDU = {0x00, 0x80, 0x42}
  std::vector<uint8_t> apdu = {0x00, 0x80, 0x42};
  auto msg = build_group_packet(0x0A03, apdu);
  // Wire: [len:2][type:2][addr:2][apdu:3] = 9 bytes
  EXPECT_EQ(msg.size(), 9);
  EXPECT_EQ(msg[0], 0x00);                              // len hi
  EXPECT_EQ(msg[1], 0x07);                              // len lo = 7
  EXPECT_EQ(msg[2], hi(EibMessageType::GROUP_PACKET));  // type hi
  EXPECT_EQ(msg[3], lo(EibMessageType::GROUP_PACKET));  // type lo
  EXPECT_EQ(msg[4], 0x0A);                              // addr hi (1/2/3 = 0x0A03)
  EXPECT_EQ(msg[5], 0x03);                              // addr lo
  EXPECT_EQ(msg[6], 0x00);                              // apdu[0]
  EXPECT_EQ(msg[7], 0x80);                              // apdu[1]
  EXPECT_EQ(msg[8], 0x42);                              // apdu[2]
}

TEST(EibdWireFormatTest, GroupPacketMultiByteApdu) {
  // Temperature DPT 9.001: 2-byte value 0x0C6F
  std::vector<uint8_t> apdu = {0x00, 0x80, 0x0C, 0x6F};
  auto msg = build_group_packet(0x1F07, apdu);  // 31/7/7
  EXPECT_EQ(msg.size(), 10);                    // 2 (len) + 8 (payload)
  EXPECT_EQ(msg[0], 0x00);                      // len hi
  EXPECT_EQ(msg[1], 0x08);                      // len lo = 8
  EXPECT_EQ(msg[2], hi(EibMessageType::GROUP_PACKET));
  EXPECT_EQ(msg[3], lo(EibMessageType::GROUP_PACKET));
  EXPECT_EQ(msg[4], 0x1F);  // addr hi (31/7/7 = 0x1F07)
  EXPECT_EQ(msg[5], 0x07);  // addr lo
  EXPECT_EQ(msg[6], 0x00);  // apdu[0]
  EXPECT_EQ(msg[7], 0x80);  // apdu[1]
  EXPECT_EQ(msg[8], 0x0C);  // apdu[2]
  EXPECT_EQ(msg[9], 0x6F);  // apdu[3]
}

TEST(EibdWireFormatTest, GroupPacketEmptyApdu) {
  auto msg = build_group_packet(0x0000, {});
  EXPECT_EQ(msg.size(), 6);  // 2 (len) + 4 (payload)
  EXPECT_EQ(msg[0], 0x00);   // len hi
  EXPECT_EQ(msg[1], 0x04);   // len lo = 4
  EXPECT_EQ(msg[2], hi(EibMessageType::GROUP_PACKET));
  EXPECT_EQ(msg[3], lo(EibMessageType::GROUP_PACKET));
}

// -- EIB_CACHE_READ --

TEST(EibdWireFormatTest, CacheRead) {
  // Payload: [type:2][addr:2] = 4 bytes
  auto msg = build_cache_read(0x0A03);  // 1/2/3
  EXPECT_EQ(msg.size(), 6);             // 2 (len) + 4 (payload)
  EXPECT_EQ(msg[0], 0x00);              // len hi
  EXPECT_EQ(msg[1], 0x04);              // len lo = 4
  EXPECT_EQ(msg[2], hi(EibMessageType::CACHE_READ));
  EXPECT_EQ(msg[3], lo(EibMessageType::CACHE_READ));
  EXPECT_EQ(msg[4], 0x0A);  // addr hi
  EXPECT_EQ(msg[5], 0x03);  // addr lo
}

TEST(EibdWireFormatTest, CacheReadMaxAddress) {
  // 31/7/255 = 0x1FFF
  auto msg = build_cache_read(0x1FFF);
  EXPECT_EQ(msg.size(), 6);
  EXPECT_EQ(msg[0], 0x00);
  EXPECT_EQ(msg[1], 0x04);
  EXPECT_EQ(msg[2], hi(EibMessageType::CACHE_READ));
  EXPECT_EQ(msg[3], lo(EibMessageType::CACHE_READ));
  EXPECT_EQ(msg[4], 0x1F);  // addr hi
  EXPECT_EQ(msg[5], 0xFF);  // addr lo
}

// -- EIB_CACHE_READ_NOWAIT --

TEST(EibdWireFormatTest, CacheReadNowait) {
  // Payload: [type:2][addr:2] — same structure as CACHE_READ, different type
  auto msg = build_cache_read_nowait(0x0A03);
  EXPECT_EQ(msg.size(), 6);
  EXPECT_EQ(msg[0], 0x00);  // len hi
  EXPECT_EQ(msg[1], 0x04);  // len lo = 4
  EXPECT_EQ(msg[2], hi(EibMessageType::CACHE_READ_NOWAIT));
  EXPECT_EQ(msg[3], lo(EibMessageType::CACHE_READ_NOWAIT));
  EXPECT_EQ(msg[4], 0x0A);  // addr hi
  EXPECT_EQ(msg[5], 0x03);  // addr lo
}

// -- Generic build_eibd_message (used internally by the builders above) --

TEST(EibdWireFormatTest, GenericBuilderLengthBoundary) {
  // Verify the generic builder handles edge cases correctly
  // 255-byte payload → total payload = 257 (0x0101)
  std::vector<uint8_t> data(255, 0x42);
  auto msg = build_eibd_message(EibMessageType::GROUP_PACKET, data);
  EXPECT_EQ(msg[0], 0x01);         // len hi = 1
  EXPECT_EQ(msg[1], 0x01);         // len lo = 1 (257 = 0x0101)
  EXPECT_EQ(msg.size(), 2 + 257);  // 2 (len header) + 257 (payload)
}

// ---- parse_eibd_message (inbound message parsing) ----

TEST(EibdMessageParseTest, ParseGroupPacket) {
  // Build a raw GROUP_PACKET message for parsing
  uint8_t t_hi = hi(EibMessageType::GROUP_PACKET);
  uint8_t t_lo = lo(EibMessageType::GROUP_PACKET);
  std::vector<uint8_t> raw = {0x00, 0x04, t_hi, t_lo, 0xAA, 0xBB};
  uint16_t type;
  std::vector<uint8_t> data;
  ASSERT_TRUE(parse_eibd_message(raw, type, data));
  EXPECT_EQ(type, EibMessageType::GROUP_PACKET);
  ASSERT_EQ(data.size(), 2);
  EXPECT_EQ(data[0], 0xAA);
  EXPECT_EQ(data[1], 0xBB);
}

TEST(EibdMessageParseTest, ParseOpenGroupconResponse) {
  // knxd responds to EIB_OPEN_GROUPCON with just the 2 type bytes (no data)
  uint8_t t_hi = hi(EibMessageType::OPEN_GROUPCON);
  uint8_t t_lo = lo(EibMessageType::OPEN_GROUPCON);
  std::vector<uint8_t> raw = {0x00, 0x02, t_hi, t_lo};
  uint16_t type;
  std::vector<uint8_t> data;
  ASSERT_TRUE(parse_eibd_message(raw, type, data));
  EXPECT_EQ(type, EibMessageType::OPEN_GROUPCON);
  EXPECT_TRUE(data.empty());
}

TEST(EibdMessageTest, ParseTooShort) {
  std::vector<uint8_t> raw = {0x00, 0x04, 0x00};  // truncated
  uint16_t type;
  std::vector<uint8_t> data;
  EXPECT_FALSE(parse_eibd_message(raw, type, data));
}

// ---- try_extract_message (buffer-based incremental parsing) ----

TEST(TryExtractMessageTest, CompleteMessage) {
  std::vector<uint8_t> buffer = {0x00, 0x04, 0x00, 0x27, 0xAA, 0xBB};
  auto msg = try_extract_message(buffer);
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg->size(), 6);
  // Verify the message contains what we put in
  EXPECT_EQ((*msg)[0], 0x00);
  EXPECT_EQ((*msg)[1], 0x04);
  EXPECT_EQ((*msg)[4], 0xAA);
  EXPECT_EQ((*msg)[5], 0xBB);
  // Buffer should be consumed
  EXPECT_TRUE(buffer.empty());
}

TEST(TryExtractMessageTest, IncompleteMessage) {
  std::vector<uint8_t> buffer = {0x00, 0x04, 0x00};  // length says 4, only 1 byte available
  auto msg = try_extract_message(buffer);
  EXPECT_FALSE(msg.has_value());
  // Buffer should be unchanged
  EXPECT_EQ(buffer.size(), 3);
}

TEST(TryExtractMessageTest, EmptyBuffer) {
  std::vector<uint8_t> buffer;
  auto msg = try_extract_message(buffer);
  EXPECT_FALSE(msg.has_value());
}

TEST(TryExtractMessageTest, MultipleMessagesBuffered) {
  // Two messages back-to-back
  std::vector<uint8_t> buffer = {
      0x00, 0x02, 0x00, 0x27,       // 2-byte payload, type 0x0027
      0x00, 0x03, 0x00, 0x25, 0xAA  // 3-byte payload, type 0x0025
  };
  auto msg1 = try_extract_message(buffer);
  ASSERT_TRUE(msg1.has_value());
  EXPECT_EQ(msg1->size(), 4);  // 2 len + 2 payload

  auto msg2 = try_extract_message(buffer);
  ASSERT_TRUE(msg2.has_value());
  EXPECT_EQ(msg2->size(), 5);  // 2 len + 3 payload

  EXPECT_TRUE(buffer.empty());
}

TEST(TryExtractMessageTest, PartialSecondMessage) {
  // First message complete, second incomplete
  std::vector<uint8_t> buffer = {
      0x00, 0x02, 0x00, 0x27,  // complete: 2+2=4 bytes
      0x00, 0x05, 0x00         // length says 5, only 1 byte available → incomplete
  };
  auto msg1 = try_extract_message(buffer);
  ASSERT_TRUE(msg1.has_value());
  EXPECT_EQ(msg1->size(), 4);

  auto msg2 = try_extract_message(buffer);
  EXPECT_FALSE(msg2.has_value());
  EXPECT_EQ(buffer.size(), 3);  // remaining partial message preserved
}

TEST(TryExtractMessageTest, SingleByteBuffer) {
  std::vector<uint8_t> buffer = {0x00};  // need 2 bytes for length
  auto msg = try_extract_message(buffer);
  EXPECT_FALSE(msg.has_value());
  EXPECT_EQ(buffer.size(), 1);
}

TEST(TryExtractMessageTest, MaxPayloadSizeAccepted) {
  // Maximum 16-bit payload length: 65535 bytes — this would be huge but
  // the function itself doesn't cap it. The caller (read_message) enforces
  // kMaxReadBufferSize. Here we just verify basic correctness for a valid
  // but empty message (payload_len=0).
  std::vector<uint8_t> buffer = {0x00, 0x00};  // 0-length payload
  auto msg = try_extract_message(buffer);
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg->size(), 2);
  EXPECT_TRUE(buffer.empty());
}
