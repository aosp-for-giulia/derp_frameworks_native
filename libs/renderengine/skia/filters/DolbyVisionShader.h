/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <include/core/SkRefCnt.h>
#include <ui/GraphicTypes.h>

#include <memory>

class SkShader;

namespace android {
class GraphicBuffer;

namespace renderengine::skia {

// Optional adapter for the Oplus DolbyEffect 4.1 ABI. Owned and called by the
// RenderEngine thread. No Skia objects cross the proprietary library boundary.
class DolbyVisionShader {
public:
    DolbyVisionShader();
    ~DolbyVisionShader();

    // Updates metadata for this layer. False means use the ordinary color pipeline.
    bool prepare(const GraphicBuffer&, ui::Dataspace);
    sk_sp<SkShader> makeShader(sk_sp<SkShader> input, ui::Dataspace output, float dimmingRatio);

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace renderengine::skia
} // namespace android
