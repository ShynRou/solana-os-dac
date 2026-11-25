#pragma once

// TI TLV320DAC3100 over I2C: PLL from BCLK, I2S 16-bit slave, headphone
// out, class-D speaker left off. Registers from the datasheet (SLAS671) and
// Adafruit's CircuitPython driver (adafruit_tlv320.py).

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

bool dac_probe(void);                       // registers the device on the BSP's bus
esp_err_t dac_start(uint32_t rate);         // I2S must already be clocking
esp_err_t dac_sleep(bool asleep);           // headphone driver, DAC and PLL off / on
void dac_stop(void);
