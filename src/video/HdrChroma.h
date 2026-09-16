#pragma once

namespace llcv::hdr {

// A user-selected interpretation, not a claim that contradictory metadata is
// equivalent. Auto remains strict; never infer this override from device names.
enum class ChromaLocation { Auto, TopLeft, Left };

inline constexpr const wchar_t* ChromaLocationName(ChromaLocation location) {
    switch (location) {
    case ChromaLocation::TopLeft: return L"TopLeft";
    case ChromaLocation::Left: return L"Left";
    default: return L"Auto";
    }
}

} // namespace llcv::hdr
