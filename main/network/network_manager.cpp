#include "network_manager.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

namespace {
constexpr char kTag[] = "network";
constexpr char kNvsNamespace[] = "network";
constexpr char kSsidKey[] = "sta_ssid";
constexpr char kPasswordKey[] = "sta_pass";
constexpr char kApPasswordKey[] = "ap_pass";
constexpr char kApSsidPrefix[] = "ESP32-Display-";
constexpr int kReconnectDelayUs = 5000000;
constexpr size_t kHostnameMaxLength = 32;

SemaphoreHandle_t network_mutex = nullptr;
esp_netif_t *ap_netif = nullptr;
esp_netif_t *sta_netif = nullptr;
esp_timer_handle_t reconnect_timer = nullptr;
std::atomic<bool> initialized{false};
std::atomic<bool> ap_enabled{false};
std::atomic<bool> sta_configured{false};
char ap_ssid[33] = {};
char ap_password[64] = {};
char sta_ssid[33] = {};
char sta_password[65] = {};

bool is_valid_hostname(const char *hostname)
{
    const size_t length = std::strlen(hostname);
    if (length == 0 || length > kHostnameMaxLength) {
        return false;
    }
    const auto is_alphanumeric = [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9');
    };
    if (!is_alphanumeric(hostname[0]) || !is_alphanumeric(hostname[length - 1])) {
        return false;
    }
    for (size_t i = 1; i + 1 < length; ++i) {
        if (!is_alphanumeric(hostname[i]) && hostname[i] != '-') {
            return false;
        }
    }
    return true;
}

esp_err_t read_nvs_string(nvs_handle_t handle, const char *key, char *value,
                          size_t capacity, bool &present)
{
    size_t required = 0;
    esp_err_t error = nvs_get_str(handle, key, nullptr, &required);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        present = false;
        value[0] = '\0';
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(error, kTag, "NVS read for %s failed", key);
    ESP_RETURN_ON_FALSE(required <= capacity, ESP_ERR_INVALID_SIZE, kTag,
                        "NVS value %s exceeds buffer", key);
    ESP_RETURN_ON_ERROR(nvs_get_str(handle, key, value, &required), kTag,
                        "NVS read for %s failed", key);
    present = true;
    return ESP_OK;
}

esp_err_t write_nvs_string(const char *key, const char *value)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(kNvsNamespace, NVS_READWRITE, &handle), kTag,
                        "Network NVS open failed");
    esp_err_t error = nvs_set_str(handle, key, value);
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

esp_err_t write_station_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(kNvsNamespace, NVS_READWRITE, &handle), kTag,
                        "Network NVS open failed");
    esp_err_t error = nvs_set_str(handle, kSsidKey, ssid);
    if (error == ESP_OK) {
        error = nvs_set_str(handle, kPasswordKey, password);
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

void generate_ap_password(const uint8_t mac[6])
{
    // Full factory MAC makes the setup key unique per unit without touching eFuses.
    std::snprintf(ap_password, sizeof(ap_password), "Display-%02X%02X%02X%02X%02X%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (CONFIG_APP_AP_PASSWORD[0] != '\0') {
        const size_t override_length = std::strlen(CONFIG_APP_AP_PASSWORD);
        if (override_length >= 8 && override_length <= 63) {
            std::strncpy(ap_password, CONFIG_APP_AP_PASSWORD, sizeof(ap_password) - 1);
            return;
        }
        ESP_LOGE(kTag, "APP_AP_PASSWORD must contain 8-63 bytes; using the MAC-derived setup password");
    }
}

void make_ap_config(wifi_config_t &config)
{
    std::memset(&config, 0, sizeof(config));
    std::strncpy(reinterpret_cast<char *>(config.ap.ssid), ap_ssid, sizeof(config.ap.ssid));
    std::strncpy(reinterpret_cast<char *>(config.ap.password), ap_password, sizeof(config.ap.password));
    config.ap.ssid_len = std::strlen(ap_ssid);
    config.ap.channel = 1;
    config.ap.max_connection = 4;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.pmf_cfg.required = false;
}

esp_err_t apply_ap_config()
{
    wifi_config_t config = {};
    make_ap_config(config);
    return esp_wifi_set_config(WIFI_IF_AP, &config);
}

void reconnect_timer_callback(void *)
{
    if (initialized.load() && sta_configured.load()) {
        const esp_err_t error = esp_wifi_connect();
        if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(kTag, "STA reconnect attempt failed: %s", esp_err_to_name(error));
        }
    }
}

void wifi_event_handler(void *, esp_event_base_t base, int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (sta_configured.load()) {
            ESP_LOGW(kTag, "STA disconnected; reconnecting in 5 seconds");
            const esp_err_t timer_error = esp_timer_start_once(reconnect_timer, kReconnectDelayUs);
            if (timer_error != ESP_OK && timer_error != ESP_ERR_INVALID_STATE) {
                ESP_LOGE(kTag, "STA reconnect timer failed: %s", esp_err_to_name(timer_error));
            }
        }
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ap_enabled.store(true);
        ESP_LOGI(kTag, "SoftAP started: %s", ap_ssid);
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        ap_enabled.store(false);
        ESP_LOGI(kTag, "SoftAP stopped");
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        const auto *event = static_cast<wifi_event_ap_staconnected_t *>(event_data);
        ESP_LOGI(kTag, "Client connected to SoftAP (AID=%u)", static_cast<unsigned>(event->aid));
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const auto *event = static_cast<ip_event_got_ip_t *>(event_data);
        esp_timer_stop(reconnect_timer);
        ESP_LOGI(kTag, "STA obtained IP " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

esp_err_t load_station_settings()
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(kNvsNamespace, NVS_READWRITE, &handle), kTag,
                        "Network NVS open failed");
    bool have_ssid = false;
    bool have_password = false;
    bool have_ap_password = false;
    esp_err_t error = read_nvs_string(handle, kSsidKey, sta_ssid, sizeof(sta_ssid), have_ssid);
    if (error == ESP_OK) {
        error = read_nvs_string(handle, kPasswordKey, sta_password, sizeof(sta_password), have_password);
    }
    if (error == ESP_OK) {
        error = read_nvs_string(handle, kApPasswordKey, ap_password, sizeof(ap_password), have_ap_password);
    }
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(error, kTag, "Station settings read failed");

    if (have_ap_password && std::strlen(ap_password) >= 8 && std::strlen(ap_password) <= 63) {
        // Persisted user-selected value takes precedence over build defaults.
    } else {
        ap_password[0] = '\0';
        have_ap_password = false;
    }

    if (!have_ssid && CONFIG_APP_STA_SSID[0] != '\0') {
        const size_t ssid_length = std::strlen(CONFIG_APP_STA_SSID);
        const size_t password_length = std::strlen(CONFIG_APP_STA_PASSWORD);
        ESP_RETURN_ON_FALSE(ssid_length <= 32 &&
                            (password_length == 0 || (password_length >= 8 && password_length <= 64)),
                            ESP_ERR_INVALID_SIZE,
                            kTag, "Initial STA credentials exceed Wi-Fi limits");
        ESP_RETURN_ON_ERROR(nvs_open(kNvsNamespace, NVS_READWRITE, &handle), kTag,
                            "Network NVS open failed");
        error = nvs_set_str(handle, kSsidKey, CONFIG_APP_STA_SSID);
        if (error == ESP_OK) {
            error = nvs_set_str(handle, kPasswordKey, CONFIG_APP_STA_PASSWORD);
        }
        if (error == ESP_OK) {
            error = nvs_commit(handle);
        }
        nvs_close(handle);
        ESP_RETURN_ON_ERROR(error, kTag, "Initial STA credentials could not be stored");
        std::strncpy(sta_ssid, CONFIG_APP_STA_SSID, sizeof(sta_ssid) - 1);
        std::strncpy(sta_password, CONFIG_APP_STA_PASSWORD, sizeof(sta_password) - 1);
        have_ssid = true;
    } else if (have_ssid && !have_password) {
        sta_password[0] = '\0';
    }
    sta_configured.store(have_ssid && sta_ssid[0] != '\0');
    if (have_ap_password) {
        return ESP_OK;
    }
    uint8_t mac[6] = {};
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), kTag, "SoftAP MAC read failed");
    generate_ap_password(mac);
    ESP_RETURN_ON_ERROR(write_nvs_string(kApPasswordKey, ap_password), kTag,
                        "Initial AP password save failed");
    return ESP_OK;
}

esp_err_t set_station_config()
{
    wifi_config_t config = {};
    if (sta_configured.load()) {
        std::strncpy(reinterpret_cast<char *>(config.sta.ssid), sta_ssid, sizeof(config.sta.ssid));
        std::strncpy(reinterpret_cast<char *>(config.sta.password), sta_password, sizeof(config.sta.password));
        config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        config.sta.pmf_cfg.capable = true;
        config.sta.pmf_cfg.required = false;
    }
    return esp_wifi_set_config(WIFI_IF_STA, &config);
}

esp_err_t reapply_station_config()
{
    if (sta_configured.load()) {
        const wifi_mode_t mode = ap_enabled.load() ? WIFI_MODE_APSTA : WIFI_MODE_STA;
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(mode), kTag, "STA mode activation failed");
        ESP_RETURN_ON_ERROR(set_station_config(), kTag, "STA configuration failed");
        const esp_err_t connect_error = esp_wifi_connect();
        if (connect_error != ESP_OK && connect_error != ESP_ERR_WIFI_CONN) {
            return connect_error;
        }
    }
    return ESP_OK;
}
}

namespace network {
esp_err_t initialize()
{
    ESP_RETURN_ON_FALSE(!initialized.load(), ESP_ERR_INVALID_STATE, kTag, "Network already initialized");
    ESP_RETURN_ON_FALSE(is_valid_hostname(CONFIG_APP_HOSTNAME), ESP_ERR_INVALID_ARG, kTag,
                        "APP_HOSTNAME must be 1-32 ASCII letters, digits, or hyphens, and start/end with a letter or digit");
    network_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(network_mutex != nullptr, ESP_ERR_NO_MEM, kTag, "Network mutex allocation failed");

    uint8_t mac[6] = {};
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), kTag, "SoftAP MAC read failed");
    std::snprintf(ap_ssid, sizeof(ap_ssid), "%s%02X%02X", kApSsidPrefix, mac[4], mac[5]);
    ESP_RETURN_ON_ERROR(load_station_settings(), kTag, "Station settings initialization failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), kTag, "Network interface initialization failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), kTag, "Default event loop creation failed");
    ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(ap_netif != nullptr, ESP_FAIL, kTag, "SoftAP interface creation failed");
    sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(sta_netif != nullptr, ESP_FAIL, kTag, "STA interface creation failed");
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(sta_netif, CONFIG_APP_HOSTNAME), kTag,
                        "STA hostname setup failed");

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_config), kTag, "Wi-Fi initialization failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), kTag,
                        "Wi-Fi RAM storage configuration failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr),
                        kTag, "Wi-Fi event handler registration failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr),
                        kTag, "IP event handler registration failed");

    esp_timer_create_args_t reconnect_args = {};
    reconnect_args.callback = reconnect_timer_callback;
    reconnect_args.name = "sta-reconnect";
    ESP_RETURN_ON_ERROR(esp_timer_create(&reconnect_args, &reconnect_timer), kTag,
                        "STA reconnect timer creation failed");

    const wifi_mode_t startup_mode = sta_configured.load() ? WIFI_MODE_STA : WIFI_MODE_AP;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(startup_mode), kTag, "Wi-Fi startup mode failed");
    if (startup_mode == WIFI_MODE_AP) {
        ESP_RETURN_ON_ERROR(apply_ap_config(), kTag, "SoftAP configuration failed");
    }
    if (sta_configured.load()) {
        ESP_RETURN_ON_ERROR(set_station_config(), kTag, "STA configuration failed");
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "Wi-Fi startup failed");
    initialized.store(true);
    ap_enabled.store(startup_mode == WIFI_MODE_AP);

    if (sta_configured.load()) {
        ESP_RETURN_ON_ERROR(esp_wifi_connect(), kTag, "Initial STA connection request failed");
    }
    ESP_RETURN_ON_ERROR(mdns_init(), kTag, "mDNS initialization failed");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(CONFIG_APP_HOSTNAME), kTag, "mDNS hostname setup failed");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set("ESP32 Network Image Display"), kTag,
                        "mDNS instance setup failed");

    if (sta_configured.load()) {
        ESP_LOGI(kTag, "STA ready; AP disabled at startup; hostname=%s, mDNS=%s.local",
                 CONFIG_APP_HOSTNAME, CONFIG_APP_HOSTNAME);
    } else {
        ESP_LOGI(kTag, "AP ready; SSID=%s, IP=192.168.4.1, mDNS=%s.local",
                 ap_ssid, CONFIG_APP_HOSTNAME);
    }
    ESP_LOGW(kTag, "AP setup password (when AP is enabled): %s", ap_password);
    return ESP_OK;
}

esp_err_t toggle_ap()
{
    ESP_RETURN_ON_FALSE(initialized.load(), ESP_ERR_INVALID_STATE, kTag, "Network is not initialized");
    xSemaphoreTake(network_mutex, portMAX_DELAY);
    const bool enable_ap = !ap_enabled.load();
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (enable_ap) {
        mode = sta_configured.load() ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    } else if (sta_configured.load()) {
        mode = WIFI_MODE_STA;
    }
    esp_err_t error = esp_wifi_set_mode(mode);
    if (error == ESP_OK && enable_ap) {
        error = apply_ap_config();
        if (error != ESP_OK) {
            const wifi_mode_t previous_mode = sta_configured.load() ? WIFI_MODE_STA : WIFI_MODE_NULL;
            esp_wifi_set_mode(previous_mode);
            xSemaphoreGive(network_mutex);
            return error;
        }
    }
    if (error == ESP_OK) {
        ap_enabled.store(enable_ap);
        if (!enable_ap && sta_configured.load()) {
            error = esp_wifi_connect();
            if (error == ESP_ERR_WIFI_CONN) {
                error = ESP_OK;
            }
        }
        ESP_LOGI(kTag, "AP %s by PWR short press; STA mode is preserved", enable_ap ? "enabled" : "disabled");
    }
    xSemaphoreGive(network_mutex);
    return error;
}

esp_err_t set_station_credentials(const char *ssid, const char *password)
{
    ESP_RETURN_ON_FALSE(initialized.load(), ESP_ERR_INVALID_STATE, kTag, "Network is not initialized");
    ESP_RETURN_ON_FALSE(ssid != nullptr && password != nullptr, ESP_ERR_INVALID_ARG, kTag,
                        "SSID and password are required");
    const size_t ssid_length = std::strlen(ssid);
    const size_t password_length = std::strlen(password);
    ESP_RETURN_ON_FALSE(ssid_length > 0 && ssid_length <= 32, ESP_ERR_INVALID_ARG, kTag,
                        "SSID must contain 1-32 bytes");
    ESP_RETURN_ON_FALSE(password_length == 0 || (password_length >= 8 && password_length <= 64),
                        ESP_ERR_INVALID_ARG, kTag,
                        "Wi-Fi password must be empty or contain 8-64 bytes");

    xSemaphoreTake(network_mutex, portMAX_DELAY);
    esp_err_t error = write_station_credentials(ssid, password);
    if (error == ESP_OK) {
        std::strncpy(sta_ssid, ssid, sizeof(sta_ssid) - 1);
        std::strncpy(sta_password, password, sizeof(sta_password) - 1);
        sta_configured.store(true);
        error = reapply_station_config();
    }
    xSemaphoreGive(network_mutex);
    return error;
}

esp_err_t clear_station_credentials()
{
    ESP_RETURN_ON_FALSE(initialized.load(), ESP_ERR_INVALID_STATE, kTag, "Network is not initialized");
    xSemaphoreTake(network_mutex, portMAX_DELAY);
    nvs_handle_t handle;
    esp_err_t error = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        const esp_err_t ssid_error = nvs_erase_key(handle, kSsidKey);
        const esp_err_t pass_error = nvs_erase_key(handle, kPasswordKey);
        if (ssid_error != ESP_OK && ssid_error != ESP_ERR_NVS_NOT_FOUND) {
            error = ssid_error;
        } else if (pass_error != ESP_OK && pass_error != ESP_ERR_NVS_NOT_FOUND) {
            error = pass_error;
        } else {
            error = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (error == ESP_OK) {
        sta_ssid[0] = '\0';
        sta_password[0] = '\0';
        sta_configured.store(false);
        if (ap_enabled.load()) {
            error = esp_wifi_set_mode(WIFI_MODE_AP);
        } else {
            error = esp_wifi_set_mode(WIFI_MODE_NULL);
        }
    }
    xSemaphoreGive(network_mutex);
    return error;
}

esp_err_t set_ap_password(const char *password)
{
    ESP_RETURN_ON_FALSE(initialized.load(), ESP_ERR_INVALID_STATE, kTag, "Network is not initialized");
    ESP_RETURN_ON_FALSE(password != nullptr, ESP_ERR_INVALID_ARG, kTag, "AP password is required");
    const size_t length = std::strlen(password);
    ESP_RETURN_ON_FALSE(length >= 8 && length <= 63, ESP_ERR_INVALID_ARG, kTag,
                        "AP password must contain 8-63 bytes");
    xSemaphoreTake(network_mutex, portMAX_DELAY);
    esp_err_t error = write_nvs_string(kApPasswordKey, password);
    if (error == ESP_OK) {
        std::strncpy(ap_password, password, sizeof(ap_password) - 1);
        if (ap_enabled.load()) {
            error = apply_ap_config();
        }
    }
    xSemaphoreGive(network_mutex);
    return error;
}

esp_err_t get_status(Status &status)
{
    status = {};
    status.ap_enabled = ap_enabled.load();
    status.sta_configured = sta_configured.load();
    wifi_ap_record_t record = {};
    status.sta_connected = status.sta_configured && esp_wifi_sta_get_ap_info(&record) == ESP_OK;
    std::strncpy(status.ap_ssid, ap_ssid, sizeof(status.ap_ssid) - 1);
    if (status.sta_connected) {
        status.sta_rssi = record.rssi;
    }
    if (ap_netif != nullptr) {
        esp_netif_ip_info_t info = {};
        if (esp_netif_get_ip_info(ap_netif, &info) == ESP_OK) {
            std::snprintf(status.ap_ip, sizeof(status.ap_ip), IPSTR, IP2STR(&info.ip));
        }
    }
    if (sta_netif != nullptr) {
        esp_netif_ip_info_t info = {};
        if (esp_netif_get_ip_info(sta_netif, &info) == ESP_OK) {
            std::snprintf(status.sta_ip, sizeof(status.sta_ip), IPSTR, IP2STR(&info.ip));
        }
    }
    return ESP_OK;
}
}
