/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "driver/i2c_master.h"

namespace tough_hal {

/// The internal I2C bus (SDA 21 / SCL 22), created on first use. The IDF
/// master driver serialises transactions per bus, so the AXP192 and the touch
/// controller may be used from different tasks.
i2c_master_bus_handle_t i2c_bus();

/// A device on that bus at kI2cHz, created on first use per address.
i2c_master_dev_handle_t i2c_device(uint8_t addr);

}  // namespace tough_hal
