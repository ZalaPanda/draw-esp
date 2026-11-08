#include "display/brightness_settings.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {
esp_err_t init_error = ESP_OK;
esp_err_t open_error = ESP_OK;
esp_err_t read_error = ESP_ERR_NVS_NOT_FOUND;
esp_err_t write_error = ESP_OK;
esp_err_t commit_error = ESP_OK;
uint8_t stored = 0;
uint8_t pending = 0;
unsigned opened = 0;
unsigned closed = 0;
unsigned writes = 0;
unsigned commits = 0;
}

esp_err_t nvs_flash_init() { return init_error; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    assert(std::strcmp(name, "display") == 0 && mode == NVS_READWRITE);
    if (open_error != ESP_OK) return open_error;
    *handle = 1;
    ++opened;
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    assert(handle == 1 && std::strcmp(key, "brightness") == 0);
    if (read_error == ESP_OK) *value = stored;
    return read_error;
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    assert(handle == 1 && std::strcmp(key, "brightness") == 0);
    ++writes;
    pending = value;
    return write_error;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1);
    ++commits;
    if (commit_error == ESP_OK) stored = pending;
    return commit_error;
}
void nvs_close(nvs_handle_t handle)
{
    assert(handle == 1);
    ++closed;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *scenario = argv[1];
    unsigned expected = 80;
    if (std::strcmp(scenario, "restore") == 0) {
        read_error = ESP_OK;
        stored = 30;
        expected = 30;
    } else if (std::strcmp(scenario, "invalid-low") == 0) {
        read_error = ESP_OK;
        stored = 9;
    } else if (std::strcmp(scenario, "invalid-high") == 0) {
        read_error = ESP_OK;
        stored = 101;
    } else if (std::strcmp(scenario, "type") == 0) {
        read_error = ESP_ERR_NVS_TYPE_MISMATCH;
    } else if (std::strcmp(scenario, "read-failure") == 0) {
        read_error = ESP_FAIL;
    } else if (std::strcmp(scenario, "open-failure") == 0) {
        open_error = ESP_FAIL;
    } else if (std::strcmp(scenario, "init-failure") == 0) {
        init_error = ESP_FAIL;
    }
    unsigned percent = 0;
    const esp_err_t error = display::initialize_brightness_settings(percent);
    if (read_error == ESP_FAIL || open_error == ESP_FAIL || init_error == ESP_FAIL) {
        assert(error == ESP_FAIL && percent == 0 && opened == closed);
        assert(display::save_brightness(30) == ESP_ERR_INVALID_STATE);
    } else {
        assert(error == ESP_OK && percent == expected && opened == closed);
        assert(writes == 0 && commits == 0);
        assert(display::save_brightness(expected) == ESP_OK && writes == 0);
        assert(display::save_brightness(0) == ESP_ERR_INVALID_ARG && writes == 0);
        assert(display::save_brightness(101) == ESP_ERR_INVALID_ARG && writes == 0);
        assert(display::initialize_brightness_settings(percent) == ESP_ERR_INVALID_STATE);
        if (std::strcmp(scenario, "write-failure") == 0 ||
            std::strcmp(scenario, "commit-failure") == 0 ||
            std::strcmp(scenario, "save-open-failure") == 0) {
            if (std::strcmp(scenario, "write-failure") == 0) write_error = ESP_FAIL;
            if (std::strcmp(scenario, "commit-failure") == 0) commit_error = ESP_FAIL;
            if (std::strcmp(scenario, "save-open-failure") == 0) open_error = ESP_FAIL;
            assert(display::save_brightness(20) == ESP_FAIL && opened == closed);
            assert(commits == (write_error == ESP_FAIL || open_error == ESP_FAIL ? 0U : 1U));
            const unsigned before = writes;
            assert(display::save_brightness(expected) == ESP_OK && writes == before);
            write_error = commit_error = open_error = ESP_OK;
        }
        assert(display::save_brightness(10) == ESP_OK && stored == 10 && opened == closed);
        const unsigned before = writes;
        assert(display::save_brightness(10) == ESP_OK && writes == before);
        assert(display::save_brightness(100) == ESP_OK && stored == 100 && opened == closed);
        const unsigned after = writes;
        assert(display::save_brightness(100) == ESP_OK && writes == after);
        assert(commits > 0);
    }
    std::printf("Brightness settings: %s passed\n", scenario);
}
