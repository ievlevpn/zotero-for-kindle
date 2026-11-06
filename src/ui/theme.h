#pragma once

#include <cstdint>

// Visual constants shared by all screens (see mockup C, docs/superpowers/specs).
namespace zk::theme {

inline constexpr const char* kSans = "Amazon Ember";
inline constexpr const char* kSerif = "Bookerly";

// Gray levels (0 = black ink, 255 = white).
inline constexpr uint8_t kInk = 0;
inline constexpr uint8_t kSecondary = 85;
inline constexpr uint8_t kTertiary = 120;
inline constexpr uint8_t kRule = 220;
inline constexpr uint8_t kPaper = 251;

// Base spacing unit in device pixels (300 dpi): 12 px ≈ 1 mm.
inline constexpr int kUnit = 12;

}  // namespace zk::theme
