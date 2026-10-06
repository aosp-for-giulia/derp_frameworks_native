/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../skia/filters/DolbyVisionMetadata.h"

#include <gtest/gtest.h>

#include <vector>

namespace android::renderengine::skia::dolby {
namespace {

std::vector<uint8_t> metadata(uint64_t size, size_t capacity = kContentMetadataSize) {
    std::vector<uint8_t> bytes(capacity);
    for (size_t i = 0; i < sizeof(size); ++i) bytes[i] = size >> (8 * i);
    const uint8_t prefix[] = {0, 0, 0, 1, 0x7c, 1, 0x19};
    std::copy(std::begin(prefix), std::end(prefix), bytes.begin() + sizeof(size));
    return bytes;
}

TEST(DolbyVisionMetadata, AcceptsPaddedAndExactLengthPayloads) {
    for (const size_t storage : {size_t(23), kContentMetadataSize}) {
        auto bytes = metadata(15, storage);
        auto payload = getPayload(bytes);
        ASSERT_TRUE(payload);
        EXPECT_EQ(payload->size(), 15u);
        EXPECT_EQ(payload->data(), bytes.data() + 8);
    }
}

TEST(DolbyVisionMetadata, RejectsTruncatedHeaderAndPayload) {
    auto bytes = metadata(15);
    for (size_t length = 0; length < 23; ++length) {
        EXPECT_FALSE(getPayload(std::span(bytes).first(length)));
    }
}

TEST(DolbyVisionMetadata, ValidatesUntrustedLengthIncludingOverflow) {
    for (uint64_t length : {uint64_t(0), uint64_t(6), uint64_t(kPayloadCapacity + 1), UINT64_MAX}) {
        auto bytes = metadata(length);
        EXPECT_FALSE(getPayload(bytes));
    }
    auto bytes = metadata(kPayloadCapacity);
    ASSERT_TRUE(getPayload(bytes));
    bytes.push_back(0);
    EXPECT_FALSE(getPayload(bytes));
}

TEST(DolbyVisionMetadata, RejectsNonDolbyAndDoesNotReusePreviousFrame) {
    auto bytes = metadata(15);
    ASSERT_TRUE(getPayload(bytes));
    bytes[12] ^= 1;
    EXPECT_FALSE(getPayload(bytes));
    bytes[12] ^= 1;
    // A failed/short mapper response must not expose the old bytes in storage.
    EXPECT_FALSE(getPayload(std::span(bytes).first(0)));
    bytes[0] = 0;
    EXPECT_FALSE(getPayload(bytes));
}

} // namespace
} // namespace android::renderengine::skia::dolby
