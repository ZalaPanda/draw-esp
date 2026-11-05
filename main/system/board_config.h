#pragma once

#include <cstddef>
#include "driver/gpio.h"
#include "driver/spi_master.h"

namespace board {
constexpr gpio_num_t kI2cSda = GPIO_NUM_15;
constexpr gpio_num_t kI2cScl = GPIO_NUM_14;
constexpr gpio_num_t kDisplayCs = GPIO_NUM_12;
constexpr gpio_num_t kDisplayClock = GPIO_NUM_38;
constexpr gpio_num_t kDisplayData0 = GPIO_NUM_4;
constexpr gpio_num_t kDisplayData1 = GPIO_NUM_5;
constexpr gpio_num_t kDisplayData2 = GPIO_NUM_6;
constexpr gpio_num_t kDisplayData3 = GPIO_NUM_7;
constexpr gpio_num_t kDisplayReset = GPIO_NUM_39;
constexpr gpio_num_t kUserButton = GPIO_NUM_18;
// PWR is connected to AXP2101 PWRON; its IRQ output is not routed to an ESP32 GPIO.
constexpr spi_host_device_t kDisplayHost = SPI2_HOST;
constexpr int kDisplayWidth = 480;
constexpr int kDisplayHeight = 480;
constexpr size_t kPsramBytes = 8 * 1024 * 1024;
}
