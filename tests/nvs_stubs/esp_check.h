#pragma once

#include "esp_err.h"
#include "esp_log.h"

#define ESP_RETURN_ON_ERROR(expression, ...) do { \
    const esp_err_t test_error = (expression); \
    if (test_error != ESP_OK) { test_log(__VA_ARGS__); return test_error; } \
} while (0)
#define ESP_RETURN_ON_FALSE(condition, error, ...) do { \
    if (!(condition)) { test_log(__VA_ARGS__); return (error); } \
} while (0)
