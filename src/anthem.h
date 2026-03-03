#pragma once

#include <Arduino.h>

#include "types.h"

namespace Anthem {

void begin();
void prime(const GameState &g);
void tick(const GameState &g);
bool playNow();

}  // namespace Anthem
