#include "core/Base64.h"

#include <gtest/gtest.h>

using namespace materializr;

TEST(Base64, RoundTripsEveryByteValue) {
    std::vector<uint8_t> data(256);
    for (int i = 0; i < 256; ++i) data[static_cast<size_t>(i)] = static_cast<uint8_t>(i);

    std::string encoded = base64Encode(data.data(), data.size());
    std::vector<uint8_t> decoded;
    ASSERT_TRUE(base64Decode(encoded, decoded));
    EXPECT_EQ(decoded, data);
}

TEST(Base64, RoundTripsEveryPaddingCase) {
    for (size_t len : {3u, 4u, 5u}) { // len % 3 == 0, 1, 2
        std::vector<uint8_t> data(len);
        for (size_t i = 0; i < len; ++i) data[i] = static_cast<uint8_t>(i * 7 + 1);
        std::vector<uint8_t> decoded;
        ASSERT_TRUE(base64Decode(base64Encode(data.data(), data.size()), decoded));
        EXPECT_EQ(decoded, data);
    }
}

TEST(Base64, DecodeRejectsAnInvalidCharacter) {
    std::vector<uint8_t> decoded;
    EXPECT_FALSE(base64Decode("not valid base64!!", decoded));
}
