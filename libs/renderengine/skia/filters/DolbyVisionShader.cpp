/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "DolbyVisionShader.h"
#include "DolbyVisionMetadata.h"

#include <dlfcn.h>
#include <include/core/SkBitmap.h>
#include <include/core/SkImage.h>
#include <include/core/SkShader.h>
#include <include/effects/SkRuntimeEffect.h>
#include <log/log.h>
#include <renderengine/ColorSpaces.h>
#include <ui/GraphicBuffer.h>
#include <ui/GraphicBufferMapper.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace android::renderengine::skia {
namespace {

constexpr int64_t kCustomContentMetadata = 10029;

// Layouts recovered from the stock DolbyRenderer and DolbyEffectImpl 4.1.
// Color values use ISO primaries/transfer identifiers; range is 0=limited, 1=full.
struct ColorMetadata {
    int32_t primaries;
    int32_t range;
    int32_t transfer;
};
struct Uniform {
    std::string name;
    std::vector<uint8_t> value;
};
static_assert(sizeof(ColorMetadata) == 12);
#if defined(__aarch64__)
static_assert(sizeof(Uniform) == 48);
#endif

ColorMetadata colorMetadata(ui::Dataspace dataspace) {
    ColorMetadata color{1, 0xff, 1};
    switch (static_cast<ui::Dataspace>(dataspace & ui::Dataspace::STANDARD_MASK)) {
        case ui::Dataspace::STANDARD_BT2020:
        case ui::Dataspace::STANDARD_BT2020_CONSTANT_LUMINANCE:
            color.primaries = 9;
            break;
        case ui::Dataspace::STANDARD_DCI_P3:
            color.primaries = 12;
            break;
        default:
            break;
    }
    switch (static_cast<ui::Dataspace>(dataspace & ui::Dataspace::TRANSFER_MASK)) {
        case ui::Dataspace::TRANSFER_LINEAR:
            color.transfer = 8;
            break;
        case ui::Dataspace::TRANSFER_SRGB:
            color.transfer = 1;
            break;
        case ui::Dataspace::TRANSFER_SMPTE_170M:
            color.transfer = 6;
            break;
        case ui::Dataspace::TRANSFER_GAMMA2_2:
            color.transfer = 4;
            break;
        case ui::Dataspace::TRANSFER_GAMMA2_6:
            color.transfer = 3;
            break;
        case ui::Dataspace::TRANSFER_GAMMA2_8:
            color.transfer = 5;
            break;
        case ui::Dataspace::TRANSFER_ST2084:
            color.transfer = 16;
            break;
        case ui::Dataspace::TRANSFER_HLG:
            color.transfer = 18;
            break;
        default:
            break;
    }
    switch (static_cast<ui::Dataspace>(dataspace & ui::Dataspace::RANGE_MASK)) {
        case ui::Dataspace::RANGE_FULL:
            color.range = 1;
            break;
        case ui::Dataspace::RANGE_LIMITED:
            color.range = 0;
            break;
        case ui::Dataspace::RANGE_EXTENDED:
            color.range = 2;
            break;
        default:
            break;
    }
    return color;
}

template <typename T>
bool loadSymbol(void* library, const char* name, T& function) {
    function = reinterpret_cast<T>(dlsym(library, name));
    if (!function) ALOGE("DolbyVisionShader: missing symbol %s", name);
    return function != nullptr;
}

} // namespace

struct DolbyVisionShader::Impl {
    void* library = nullptr;
    void* effect = nullptr;
    bool attemptedLoad = false;
    bool ready = false;
    bool loggedActive = false;
    bool loggedFailure = false;
    std::array<uint8_t, dolby::kContentMetadataSize> metadata{};
    sk_sp<SkRuntimeEffect> runtimeEffect;

    // These are exported non-static methods, called with the opaque `this` as
    // their first argument (AAPCS64). Keep all such calls inside the annotated
    // methods: the proprietary library has no matching CFI type information.
    void* (*instantiate)() = nullptr;
    void (*destroy)(void*) = nullptr;
    void (*setSource)(void*, ColorMetadata*) = nullptr;
    void (*setTarget)(void*, ColorMetadata*) = nullptr;
    void (*setMetadata)(void*, uint8_t*, uint32_t) = nullptr;
    void (*enableInverseMatrix)(void*) = nullptr;
    void (*setDimmingRatio)(void*, float) = nullptr;
    const void* (*generateLut)(void*, int*) = nullptr;
    const char* (*getShaderString)(void*) = nullptr;
    std::vector<Uniform> (*getUniforms)(void*) = nullptr;

    __attribute__((no_sanitize("cfi-icall"))) ~Impl() {
        if (effect) destroy(effect);
        if (library) dlclose(library);
    }

    __attribute__((no_sanitize("cfi-icall"))) bool initialize() {
        if (attemptedLoad) return effect && runtimeEffect;
        attemptedLoad = true;
#if defined(__aarch64__)
        // Install in system_ext: a product-private library cannot reliably be
        // opened from SurfaceFlinger's default linker namespace.
        library = dlopen("/system_ext/lib64/libdolbyeffect_4.1.so", RTLD_NOW | RTLD_LOCAL);
        if (!library) {
            ALOGE("DolbyVisionShader: cannot load effect: %s", dlerror());
            return false;
        }
        bool loaded =
                loadSymbol(library, "_ZN11dolbyVision11DolbyEffect11instantiateEv", instantiate);
        loaded &= loadSymbol(library, "_ZN11dolbyVision15DolbyEffectImplD0Ev", destroy);
        loaded &= loadSymbol(library,
                             "_ZN11dolbyVision15DolbyEffectImpl22setSourceColorMetadataEPNS_"
                             "15DVColorMetaDataE",
                             setSource);
        loaded &= loadSymbol(library,
                             "_ZN11dolbyVision15DolbyEffectImpl22setTargetColorMetadataEPNS_"
                             "15DVColorMetaDataE",
                             setTarget);
        loaded &= loadSymbol(library, "_ZN11dolbyVision15DolbyEffectImpl13setDvMetadataEPhj",
                             setMetadata);
        loaded &= loadSymbol(library,
                             "_ZN11dolbyVision15DolbyEffectImpl30enableInverseMatrixNoDVSKPatchEv",
                             enableInverseMatrix);
        loaded &= loadSymbol(library, "_ZN11dolbyVision15DolbyEffectImpl15setDimmingRatioEf",
                             setDimmingRatio);
        loaded &= loadSymbol(library, "_ZN11dolbyVision15DolbyEffectImpl11generateLutEPi",
                             generateLut);
        loaded &= loadSymbol(library, "_ZN11dolbyVision15DolbyEffectImpl15getShaderStringEv",
                             getShaderString);
        loaded &= loadSymbol(library, "_ZN11dolbyVision15DolbyEffectImpl11getUniformsEv",
                             getUniforms);
        if (!loaded) return false;
        effect = instantiate();
        if (!effect) {
            reportFailure("effect initialization failed");
            return false;
        }

        // AOSP Skia performs ordinary YUV->RGB sampling. The stock library's
        // inverse-matrix mode undoes this before applying the Dolby transform.
        enableInverseMatrix(effect);
        const char* source = getShaderString(effect);
        if (!source || !*source) {
            reportFailure("empty Dolby shader source");
            return false;
        }
        auto result = SkRuntimeEffect::MakeForShader(SkString(source));
        if (!result.effect) {
            ALOGE("DolbyVisionShader: shader compilation failed: %s", result.errorText.c_str());
            return false;
        }
        runtimeEffect = std::move(result.effect);
        return true;
#else
        return false;
#endif
    }

    void reportFailure(const char* reason) {
        if (!loggedFailure) ALOGE("DolbyVisionShader: %s", reason);
        loggedFailure = true;
    }
};

DolbyVisionShader::DolbyVisionShader() : mImpl(std::make_unique<Impl>()) {}
DolbyVisionShader::~DolbyVisionShader() = default;

__attribute__((no_sanitize("cfi-icall"))) bool DolbyVisionShader::prepare(
        const GraphicBuffer& buffer, ui::Dataspace dataspace) {
    auto& impl = *mImpl;
    impl.ready = false;
    const auto transfer = static_cast<ui::Dataspace>(dataspace & ui::Dataspace::TRANSFER_MASK);
    if (transfer != ui::Dataspace::TRANSFER_ST2084 && transfer != ui::Dataspace::TRANSFER_HLG) {
        return false;
    }
    // Never reuse metadata left over from a different layer or an earlier frame.
    const int32_t bytes = GraphicBufferMapper::get().getVendorMetadata(buffer.handle, "QTI",
                                                                       kCustomContentMetadata,
                                                                       impl.metadata.data(),
                                                                       impl.metadata.size());
    if (bytes < 0 || static_cast<size_t>(bytes) > impl.metadata.size()) {
        return false;
    }
    auto payload = dolby::getPayload(std::span(impl.metadata).first(bytes));
    if (!payload) return false;
    if (!impl.initialize()) return false;
    auto source = colorMetadata(dataspace);
    impl.setSource(impl.effect, &source);
    impl.setMetadata(impl.effect, payload->data(), payload->size());
    impl.ready = true;
    return true;
}

__attribute__((no_sanitize("cfi-icall"))) sk_sp<SkShader> DolbyVisionShader::makeShader(
        sk_sp<SkShader> input, ui::Dataspace output, float dimmingRatio) {
    auto& impl = *mImpl;
    if (!impl.ready || !input) return nullptr;
    impl.ready = false;
    auto target = colorMetadata(output);
    impl.setTarget(impl.effect, &target);
    impl.setDimmingRatio(impl.effect, dimmingRatio);
    int edge = 0;
    const void* lut = impl.generateLut(impl.effect, &edge);
    if (!lut || edge < 2 || edge > 65) {
        impl.reportFailure("invalid Dolby LUT");
        return nullptr;
    }

    // Stock packs a cubic RGB10 LUT into a (edge * edge) by edge image.
    SkBitmap bitmap;
    auto info = SkImageInfo::Make(edge * edge, edge, kRGB_101010x_SkColorType, kOpaque_SkAlphaType);
    if (!bitmap.tryAllocPixels(info)) return nullptr;
    memcpy(bitmap.getPixels(), lut, static_cast<size_t>(edge) * edge * edge * sizeof(uint32_t));
    bitmap.setImmutable();
    auto image = SkImages::RasterFromBitmap(bitmap);
    if (!image) return nullptr;
    const auto matrix = SkMatrix::Scale(1.f / (edge * edge), 1.f / edge);
    auto lutShader = image->makeRawShader(SkSamplingOptions(SkFilterMode::kLinear), matrix);
    if (!lutShader) return nullptr;

    SkRuntimeShaderBuilder builder(impl.runtimeEffect);
    const auto uniforms = impl.getUniforms(impl.effect);
    for (const auto& uniform : uniforms) {
        const auto* declaration = impl.runtimeEffect->findUniform(uniform.name.c_str());
        if (!declaration || declaration->sizeInBytes() != uniform.value.size()) {
            impl.reportFailure("Dolby uniform ABI mismatch");
            return nullptr;
        }
        // Uniform data includes matrices and scalars in the library's byte layout.
        builder.uniform(uniform.name.c_str()).set(uniform.value.data(), uniform.value.size());
    }
    if (!impl.runtimeEffect->findChild("child") || !impl.runtimeEffect->findChild("lut_cube")) {
        impl.reportFailure("Dolby shader ABI mismatch");
        return nullptr;
    }
    builder.child("child") = std::move(input);
    builder.child("lut_cube") = std::move(lutShader);
    auto shader = builder.makeShader();
    if (shader) {
        // The LUT produces encoded output-dataspace colors. Preserve that
        // contract if a surrounding color-transform shader evaluates us in
        // a linear working space. Raw input/LUT samples remain unmanaged.
        shader = shader->makeWithWorkingColorSpace(toSkColorSpace(output));
    }
    if (shader && !impl.loggedActive) {
        ALOGI("DolbyVisionShader: Dolby GPU composition active (LUT edge %d)", edge);
        impl.loggedActive = true;
    }
    return shader;
}

} // namespace android::renderengine::skia
