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

#include "util/json_builder.h"

using namespace cvknxd;

TEST(JsonBuilderTest, Empty) {
  JsonBuilder jb;
  EXPECT_TRUE(jb.str().empty());
}

TEST(JsonBuilderTest, SimpleObject) {
  JsonBuilder jb;
  jb.start_object();
  jb.add_string("v", "0.0.2");
  jb.add_string("s", "abc");
  jb.end_object();
  EXPECT_EQ(jb.str(), R"({"v":"0.0.2","s":"abc"})");
}

TEST(JsonBuilderTest, NestedObject) {
  JsonBuilder jb;
  jb.start_object();
  jb.add_key("d");
  jb.start_object();
  jb.add_string("KNX:1/2/3", "0c6f");
  jb.add_string("KNX:4/5/6", "42");
  jb.end_object();
  jb.add_number("i", 1);
  jb.end_object();
  EXPECT_EQ(jb.str(), R"({"d":{"KNX:1/2/3":"0c6f","KNX:4/5/6":"42"},"i":1})");
}

TEST(JsonBuilderTest, SpecialCharsEscaped) {
  JsonBuilder jb;
  jb.start_object();
  jb.add_string("key", "val\"ue");
  jb.end_object();
  EXPECT_EQ(jb.str(), R"({"key":"val\"ue"})");
}

TEST(JsonBuilderTest, ClearAndReuse) {
  JsonBuilder jb;
  jb.start_object();
  jb.add_string("a", "1");
  jb.end_object();
  EXPECT_EQ(jb.str(), R"({"a":"1"})");

  jb.clear();
  jb.start_object();
  jb.add_string("b", "2");
  jb.end_object();
  EXPECT_EQ(jb.str(), R"({"b":"2"})");
}

TEST(JsonBuilderTest, Take) {
  JsonBuilder jb;
  jb.start_object();
  jb.add_string("v", "0.0.2");
  jb.end_object();

  std::string result = jb.take();
  EXPECT_EQ(result, R"({"v":"0.0.2"})");
  EXPECT_TRUE(jb.str().empty());
}

TEST(JsonBuilderTest, RawInsert) {
  JsonBuilder jb;
  jb.start_object();
  jb.add_raw(R"("prebuilt":{"x":1})");
  jb.add_string("extra", "val");
  jb.end_object();
  EXPECT_EQ(jb.str(), R"({"prebuilt":{"x":1},"extra":"val"})");
}

TEST(JsonBuilderTest, AddNumber) {
  JsonBuilder jb;
  jb.start_object();
  jb.add_number("i", 0);
  jb.end_object();
  EXPECT_EQ(jb.str(), R"({"i":0})");
}

TEST(JsonBuilderTest, AddNumberNegative) {
  JsonBuilder jb;
  jb.start_object();
  jb.add_number("x", -42);
  jb.end_object();
  EXPECT_EQ(jb.str(), R"({"x":-42})");
}

TEST(JsonBuilderTest, AddNumberWithString) {
  JsonBuilder jb;
  jb.start_object();
  jb.add_string("v", "0.0.2");
  jb.add_number("i", 7);
  jb.end_object();
  EXPECT_EQ(jb.str(), R"({"v":"0.0.2","i":7})");
}
