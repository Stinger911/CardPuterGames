#pragma once
#include "driver/i2c_master.h"

#include "keys.hpp"

// Клавиатура Cardputer ADV: контроллер TCA8418 (0x34) на внутренней шине I2C.
// Задача опрашивает контроллер и складывает действия из keys.hpp в очередь; главный цикл их забирает.
bool keyboard_start(i2c_master_bus_handle_t bus);

// Следующее действие из очереди или KeyAction::None.
KeyAction keyboard_next();
