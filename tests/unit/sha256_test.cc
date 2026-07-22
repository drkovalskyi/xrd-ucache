#include "vendor/sha256.h"

#include <gtest/gtest.h>

using namespace ucache;

TEST(Sha256, KnownVectors) {
  EXPECT_EQ(sha256Hex(""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(sha256Hex("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256, MillionA) {
  std::string a(1000000, 'a');
  EXPECT_EQ(sha256Hex(a),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, PaddingBoundaries) {
  // 55/56/63/64/119/120 bytes cross the one- vs two-block padding edges;
  // assert stability (self-consistency) and distinctness.
  std::string prev;
  for (size_t n : {55u, 56u, 63u, 64u, 119u, 120u}) {
    std::string s(n, 'x');
    auto h = sha256Hex(s);
    EXPECT_EQ(h.size(), 64u);
    EXPECT_NE(h, prev);
    EXPECT_EQ(h, sha256Hex(s));
    prev = h;
  }
}
