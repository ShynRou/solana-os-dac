#pragma once

// The I2C bus bsp_buttons_start() opened (NULL before it, and in the
// emulator), for other devices on it such as the audio DAC. The driver
// serialises transfers on the bus, so devices can share it from any task.
// Its own header so components can use the bus without LVGL (bsp.h).
struct i2c_master_bus_t;
struct i2c_master_bus_t *bsp_i2c_bus(void);
