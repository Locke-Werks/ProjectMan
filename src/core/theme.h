#pragma once

#include <cstdint>

// The ARCHON / Specter Point design language, in one place, so the console and
// the desktop front ends are the same product rather than two that resemble
// each other.
//
// The rules these values serve: black on black, hairline borders, 2px corners,
// red as the only accent, and the status colours confined to badge chips.
namespace pm::theme {

struct Rgb {
    std::uint8_t r, g, b;
};

// Surfaces
inline constexpr Rgb kBlack    { 0x00, 0x00, 0x00 };  // page
inline constexpr Rgb kSurface  { 0x0A, 0x0A, 0x0A };  // cards, sections
inline constexpr Rgb kElevated { 0x14, 0x14, 0x14 };  // hover, selection
inline constexpr Rgb kBorder   { 0x1F, 0x1F, 0x1F };  // hairline
inline constexpr Rgb kBorderHi { 0x2A, 0x2A, 0x2A };  // hover hairline

// Accent. The only one.
inline constexpr Rgb kRed     { 0xFF, 0x00, 0x00 };
inline constexpr Rgb kRedDark { 0xCC, 0x00, 0x00 };

// Foreground roles
inline constexpr Rgb kFg1 { 0xFF, 0xFF, 0xFF };  // headlines
inline constexpr Rgb kFg2 { 0xD5, 0xD5, 0xD5 };  // body
inline constexpr Rgb kFg3 { 0x8A, 0x8A, 0x8A };  // secondary
inline constexpr Rgb kFg4 { 0x4A, 0x4A, 0x4A };  // captions, timestamps

// Status. Badge chips only, never a section accent.
inline constexpr Rgb kSuccess { 0x00, 0xCC, 0x66 };
inline constexpr Rgb kWarning { 0xFF, 0xB8, 0x00 };
inline constexpr Rgb kInfo    { 0x00, 0x88, 0xFF };

} // namespace pm::theme
