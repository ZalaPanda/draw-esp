#include "power_management.h"
#include "board_config.h"

#include <cstdint>
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char kTag[] = "power";
constexpr uint8_t kAddress = 0x34;
constexpr uint8_t kChipIdRegister = 0x03;
constexpr uint8_t kChipId = 0x4A;
constexpr uint8_t kAdcControlRegister = 0x30;
constexpr uint8_t kVoltageAdcChannels = 0x0D;
constexpr uint8_t kIrqEnableRegister = 0x41;
constexpr uint8_t kIrqStatusRegister = 0x49;
constexpr uint8_t kPowerKeyShortPress = 1U << 3;
constexpr int kTimeoutMs = 1000;
i2c_master_bus_handle_t bus = nullptr;
i2c_master_dev_handle_t pmic = nullptr;
bool power_key_monitor_started = false;
void (*power_key_short_press_callback)() = nullptr;

esp_err_t read_register(uint8_t reg, uint8_t &value)
{
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(pmic, &reg, 1, &value, 1, kTimeoutMs),
                        kTag, "AXP2101 register 0x%02x read failed", reg);
    return ESP_OK;
}

esp_err_t write_register(uint8_t reg, uint8_t value)
{
    const uint8_t command[] = {reg, value};
    return i2c_master_transmit(pmic, command, sizeof(command), kTimeoutMs);
}

void power_key_task(void *)
{
    unsigned read_failures = 0;
    for (;;) {
        uint8_t status = 0;
        const esp_err_t read_error = read_register(kIrqStatusRegister, status);
        if (read_error != ESP_OK) {
            if ((read_failures++ % 40) == 0) {
                ESP_LOGW(kTag, "PWR-key IRQ read failed: %s", esp_err_to_name(read_error));
            }
        } else if ((status & kPowerKeyShortPress) != 0) {
            read_failures = 0;
            // AXP2101 IRQ status is write-one-to-clear. Clear only our bit and
            // leave unrelated pending PMIC events untouched.
            const esp_err_t clear_error = write_register(kIrqStatusRegister, kPowerKeyShortPress);
            if (clear_error != ESP_OK) {
                ESP_LOGE(kTag, "PWR-key IRQ clear failed: %s", esp_err_to_name(clear_error));
            } else if (power_key_short_press_callback != nullptr) {
                power_key_short_press_callback();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}
}

namespace power_management {
i2c_master_bus_handle_t i2c_bus()
{
    return bus;
}

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

esp_err_t start_power_key_monitor(void (*on_short_press)())
{
    ESP_RETURN_ON_FALSE(pmic != nullptr, ESP_ERR_INVALID_STATE, kTag, "PMIC is not initialized");
    ESP_RETURN_ON_FALSE(!power_key_monitor_started, ESP_ERR_INVALID_STATE, kTag,
                        "PWR-key monitor already started");
    ESP_RETURN_ON_FALSE(on_short_press != nullptr, ESP_ERR_INVALID_ARG, kTag,
                        "PWR-key callback is required");

    uint8_t irq_enable = 0;
    ESP_RETURN_ON_ERROR(read_register(kIrqEnableRegister, irq_enable), kTag,
                        "PWR-key IRQ enable read failed");
    const uint8_t updated_irq_enable = irq_enable | kPowerKeyShortPress;
    if (updated_irq_enable != irq_enable) {
        ESP_RETURN_ON_ERROR(write_register(kIrqEnableRegister, updated_irq_enable), kTag,
                            "PWR-key short-press IRQ enable failed");
        ESP_RETURN_ON_ERROR(read_register(kIrqEnableRegister, irq_enable), kTag,
                            "PWR-key IRQ enable verification failed");
        ESP_RETURN_ON_FALSE((irq_enable & kPowerKeyShortPress) != 0, ESP_ERR_INVALID_RESPONSE,
                            kTag, "PWR-key IRQ enable did not read back");
    }

    // Discard a short-press latch that may have been set before the monitor
    // started, so boot-time handling cannot toggle AP unexpectedly.
    uint8_t pending_status = 0;
    ESP_RETURN_ON_ERROR(read_register(kIrqStatusRegister, pending_status), kTag,
                        "PWR-key pending IRQ read failed");
    if ((pending_status & kPowerKeyShortPress) != 0) {
        ESP_RETURN_ON_ERROR(write_register(kIrqStatusRegister, kPowerKeyShortPress), kTag,
                            "Stale PWR-key IRQ clear failed");
    }

    power_key_short_press_callback = on_short_press;
    ESP_RETURN_ON_FALSE(xTaskCreate(power_key_task, "pwr-key", 3072, nullptr, 4, nullptr) == pdPASS,
                        ESP_ERR_NO_MEM, kTag, "PWR-key monitor task creation failed");
    power_key_monitor_started = true;
    ESP_LOGI(kTag, "AXP2101 PWR short-press monitor ready (25 ms I2C polling)");
    return ESP_OK;
}
}
