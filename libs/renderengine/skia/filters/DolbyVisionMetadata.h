/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace android::renderengine::skia::dolby {

constexpr size_t kPayloadCapacity = 42 * 1024;
constexpr size_t kContentMetadataSize = sizeof(uint64_t) + kPayloadCapacity;

// QTI custom-content metadata is a little-endian uint64 length followed by the
// payload. Check the returned byte count as well as the embedded length: buffers
// without metadata must never reuse a preceding frame's Dolby payload.
inline std::optional<std::span<uint8_t>> getPayload(std::span<uint8_t> metadata) {
    constexpr std::array<uint8_t, 7> header = {0, 0, 0, 1, 0x7c, 1, 0x19};
    if (metadata.size() < sizeof(uint64_t) + header.size() ||
        metadata.size() > kContentMetadataSize) {
        return std::nullopt;
    }
    uint64_t size = 0;
    for (size_t i = 0; i < sizeof(size); ++i) {
        size |= static_cast<uint64_t>(metadata[i]) << (i * 8);
    }
    if (size < header.size() || size > kPayloadCapacity || size > metadata.size() - sizeof(size)) {
        return std::nullopt;
    }
    auto payload = metadata.subspan(sizeof(size), size);
    if (!std::equal(header.begin(), header.end(), payload.begin())) return std::nullopt;
    return payload;
}

} // namespace android::renderengine::skia::dolby
