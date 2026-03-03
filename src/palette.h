#pragma once
#include <stdint.h>

namespace Palette {
  static constexpr uint16_t BG         = 0x0842;
  static constexpr uint16_t PANEL      = 0x10A3;
  static constexpr uint16_t PANEL_2    = 0x18E4;
  static constexpr uint16_t FRAME      = 0x39E7;
  static constexpr uint16_t WHITE      = 0xFFFF;
  static constexpr uint16_t GREY       = 0xC618;

  static constexpr uint16_t LEAFS_BLUE = 0x2B5F;
  static constexpr uint16_t GOLD       = 0xFEA0;

  static constexpr uint16_t STATUS_EVEN = 0x07E0;
  static constexpr uint16_t STATUS_PP   = 0xFFE0;
  static constexpr uint16_t STATUS_PK   = 0xF800;
}
