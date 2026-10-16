#pragma once

namespace boilon::release {

// Printed at boot by both variants. Bump the version for every build that goes
// onto the device; the git tag is the same text with "-" for the space
// (boilon-ai-player-v1.0.0). Upstream's PROJECT_VER, which XiaoZhi's server and
// console see, stays unchanged.
inline constexpr char kName[] = "boilon-ai-player v1.4.0";
// The version alone, "v1.0.0": the boot page shows it (M7).
inline constexpr const char* kVersion = kName + sizeof("boilon-ai-player ") - 1;

}  // namespace boilon::release
