#include "power_management.h"
#include "board_config.h"

#include <cstdint>
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

namespace {
constexpr char kTag[] = "power";
constexpr uint8_t kAddress = 0x34;
constexpr uint8_t kChipIdRegister = 0x03;
constexpr uint8_t kChipId = 0x4A;
constexpr uint8_t kAdcControlRegister = 0x30;
constexpr uint8_t kVoltageAdcChannels = 0x0D;
constexpr int kTimeoutMs = 1000;
i2c_master_bus_handle_t bus = nullptr;
i2c_master_dev_handle_t pmic = nullptr;

esp_err_t read_register(uint8_t reg, uint8_t &value)
{
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(pmic, &reg, 1, &value, 1, kTimeoutMs),
                        kTag, "AXP2101 register 0x%02x read failed", reg);
    return ESP_OK;
}
}

namespace power_management {
esp_err_t initialize()
{
    ESP_RETURN_ON_FALSE(bus == nullptr, ESP_ERR_INVALID_STATE, kTag, "PMIC already initialized");

    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = board::kI2cSda;
    bus_config.scl_io_num = board::kI2cScl;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &bus), kTag, "I2C bus creation failed");

    i2c_device_config_t device_config = {};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = kAddress;
    device_config.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &device_config, &pmic),
                        kTag, "AXP2101 device creation failed");

    uint8_t chip_id = 0;
    ESP_RETURN_ON_ERROR(read_register(kChipIdRegister, chip_id), kTag, "PMIC identification failed");
    ESP_RETURN_ON_FALSE(chip_id == kChipId, ESP_ERR_NOT_SUPPORTED, kTag,
                        "Unexpected PMIC ID 0x%02x (expected AXP2101 0x%02x)", chip_id, kChipId);

    uint8_t adc_control = 0;
    ESP_RETURN_ON_ERROR(read_register(kAdcControlRegister, adc_control), kTag, "ADC control read failed");
    // Enable battery, VBUS and system ADCs without changing rails, charging or PWR-key behavior.
    const uint8_t command[] = {kAdcControlRegister,
                               static_cast<uint8_t>(adc_control | kVoltageAdcChannels)};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(pmic, command, sizeof(command), kTimeoutMs),
                        kTag, "Voltage ADC initialization failed");
    ESP_RETURN_ON_ERROR(read_register(kAdcControlRegister, adc_control), kTag, "ADC verification failed");
    ESP_RETURN_ON_FALSE(adc_control == command[1], ESP_ERR_INVALID_RESPONSE,
                        kTag, "Voltage ADC configuration did not read back correctly");

    ESP_LOGI(kTag, "AXP2101 detected at 0x34; voltage ADCs enabled, existing power settings preserved");
    return ESP_OK;
}
}
