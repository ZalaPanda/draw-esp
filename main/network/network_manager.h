#pragma once

#include <cstddef>
#include "esp_err.h"

namespace network {
struct Status {
    bool ap_enabled;
    bool sta_connected;
    bool sta_configured;
    char ap_ssid[33];
    char ap_ip[16];
    char sta_ip[16];
    int sta_rssi;
};

esp_err_t initialize();
esp_err_t toggle_ap();
esp_err_t set_station_credentials(const char *ssid, const char *password);
esp_err_t clear_station_credentials();
esp_err_t set_ap_password(const char *password);
esp_err_t get_status(Status &status);
}
