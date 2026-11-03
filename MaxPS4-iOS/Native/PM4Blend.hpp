#pragma once
#include <cstdint>
#include <cmath>

namespace maxps4::gpu {
// CB_BLEND0_CONTROL: AMD combine codes differ from the Metal/Vulkan enums.
struct PM4BlendChannel {
    unsigned source, operation, destination;
    bool ignoresFactors() const { return operation == 2 || operation == 3; }
    static bool supportedFactor(unsigned code) {
        return code <= 10 || code == 13 || code == 14 || code == 19 || code == 20;
    }
    bool supported() const {
        return operation <= 4 && (ignoresFactors() || (supportedFactor(source) && supportedFactor(destination)));
    }
};
struct PM4BlendState {
    bool enabled;
    PM4BlendChannel rgb, alpha;
    bool supported() const { return !enabled || (rgb.supported() && alpha.supported()); }
};
inline PM4BlendState decodePM4Blend(std::uint32_t control) {
    const PM4BlendChannel rgb{control & 31, (control >> 5) & 7, (control >> 8) & 31};
    const PM4BlendChannel alpha = (control >> 29) & 1 ?
        PM4BlendChannel{(control >> 16) & 31, (control >> 21) & 7, (control >> 24) & 31} : rgb;
    return {bool((control >> 30) & 1), rgb, alpha};
}
inline float pm4BlendConstant(float value) {
    return std::isnan(value) ? 0.0f : std::fmin(std::fmax(value, 0.0f), 1.0f);
}
} // namespace maxps4::gpu
