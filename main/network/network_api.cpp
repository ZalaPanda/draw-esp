#include "network_api.h"

#include <cstring>
#include <cstdlib>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "mdns.h"
#include "network_manager.h"

namespace {
constexpr char kTag[] = "network-api";
httpd_handle_t server = nullptr;

esp_err_t send_json(httpd_req_t *request, cJSON *root, const char *status = "200 OK")
{
    char *body = cJSON_PrintUnformatted(root);
    if (body == nullptr) {
        cJSON_Delete(root);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_status(request, status);
    const esp_err_t error = httpd_resp_sendstr(request, body);
    std::free(body);
    cJSON_Delete(root);
    return error;
}

esp_err_t send_error(httpd_req_t *request, const char *status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed");
    }
    cJSON_AddBoolToObject(root, "success", false);
    cJSON_AddStringToObject(root, "error", message);
    return send_json(request, root, status);
}

esp_err_t receive_json(httpd_req_t *request, cJSON **root)
{
    constexpr size_t kMaximumBody = 512;
    if (request->content_len <= 0 || static_cast<size_t>(request->content_len) > kMaximumBody) {
        return ESP_ERR_INVALID_SIZE;
    }
    char body[kMaximumBody + 1] = {};
    size_t received = 0;
    while (received < static_cast<size_t>(request->content_len)) {
        const int count = httpd_req_recv(request, body + received,
                                         static_cast<size_t>(request->content_len) - received);
        if (count <= 0) {
            return ESP_FAIL;
        }
        received += static_cast<size_t>(count);
    }
    body[received] = '\0';
    *root = cJSON_ParseWithLength(body, received);
    return *root == nullptr ? ESP_ERR_INVALID_ARG : ESP_OK;
}

bool get_string(cJSON *root, const char *name, const char *&value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return false;
    }
    value = item->valuestring;
    return true;
}

esp_err_t handle_root(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(request,
        "ESP32 Network Image Display\n"
        "API: GET /api/device, GET /api/network, POST/DELETE /api/wifi, POST /api/ap/password\n");
}

esp_err_t handle_device(httpd_req_t *request)
{
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed");
    }
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "name", "ESP32 Network Image Display");
    cJSON_AddStringToObject(root, "model", "Waveshare ESP32-S3-Touch-AMOLED-2.16");
    cJSON_AddNumberToObject(root, "display_width", 480);
    cJSON_AddNumberToObject(root, "display_height", 480);
    return send_json(request, root);
}

esp_err_t handle_network(httpd_req_t *request)
{
    network::Status status = {};
    const esp_err_t error = network::get_status(status);
    if (error != ESP_OK) {
        return send_error(request, "500 Internal Server Error", esp_err_to_name(error));
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *sta = cJSON_CreateObject();
    cJSON *ap = cJSON_CreateObject();
    if (root == nullptr || sta == nullptr || ap == nullptr) {
        cJSON_Delete(root);
        cJSON_Delete(sta);
        cJSON_Delete(ap);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed");
    }
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddBoolToObject(sta, "configured", status.sta_configured);
    cJSON_AddBoolToObject(sta, "connected", status.sta_connected);
    cJSON_AddStringToObject(sta, "ip", status.sta_ip);
    cJSON_AddNumberToObject(sta, "rssi", status.sta_rssi);
    cJSON_AddBoolToObject(ap, "enabled", status.ap_enabled);
    cJSON_AddStringToObject(ap, "ip", status.ap_ip);
    cJSON_AddStringToObject(ap, "ssid", status.ap_ssid);
    cJSON_AddItemToObject(root, "sta", sta);
    cJSON_AddItemToObject(root, "ap", ap);
    return send_json(request, root);
}

esp_err_t handle_set_wifi(httpd_req_t *request)
{
    cJSON *root = nullptr;
    esp_err_t error = receive_json(request, &root);
    if (error != ESP_OK) {
        return send_error(request, error == ESP_ERR_INVALID_SIZE ? "413 Payload Too Large" : "400 Bad Request",
                          "Expected JSON body with ssid and password (maximum 512 bytes)");
    }
    const char *ssid = nullptr;
    const char *password = nullptr;
    if (!get_string(root, "ssid", ssid) || !get_string(root, "password", password)) {
        cJSON_Delete(root);
        return send_error(request, "400 Bad Request", "Both ssid and password must be strings");
    }
    error = network::set_station_credentials(ssid, password);
    cJSON_Delete(root);
    if (error != ESP_OK) {
        return send_error(request, "400 Bad Request", esp_err_to_name(error));
    }
    root = cJSON_CreateObject();
    if (root == nullptr) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed");
    }
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "message", "Wi-Fi credentials saved; STA connection started");
    return send_json(request, root, "202 Accepted");
}

esp_err_t handle_delete_wifi(httpd_req_t *request)
{
    const esp_err_t error = network::clear_station_credentials();
    if (error != ESP_OK) {
        return send_error(request, "500 Internal Server Error", esp_err_to_name(error));
    }
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed");
    }
    cJSON_AddBoolToObject(root, "success", true);
    return send_json(request, root);
}

esp_err_t handle_set_ap_password(httpd_req_t *request)
{
    cJSON *root = nullptr;
    const esp_err_t parse_error = receive_json(request, &root);
    if (parse_error != ESP_OK) {
        return send_error(request, "400 Bad Request", "Expected JSON body with a password");
    }
    const char *password = nullptr;
    if (!get_string(root, "password", password)) {
        cJSON_Delete(root);
        return send_error(request, "400 Bad Request", "Password must be a string");
    }
    const esp_err_t error = network::set_ap_password(password);
    cJSON_Delete(root);
    if (error != ESP_OK) {
        return send_error(request, "400 Bad Request", "AP password must contain 8-63 bytes");
    }
    root = cJSON_CreateObject();
    if (root == nullptr) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed");
    }
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "message", "AP password changed");
    return send_json(request, root);
}

esp_err_t register_handler(httpd_method_t method, const char *path, esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t uri = {};
    uri.uri = path;
    uri.method = method;
    uri.handler = handler;
    return httpd_register_uri_handler(server, &uri);
}
}

namespace network {
esp_err_t start_http_api()
{
    ESP_RETURN_ON_FALSE(server == nullptr, ESP_ERR_INVALID_STATE, kTag, "HTTP API already started");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 6144;
    config.lru_purge_enable = true;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), kTag, "HTTP server startup failed");
    ESP_RETURN_ON_ERROR(register_handler(HTTP_GET, "/", handle_root), kTag, "Root handler registration failed");
    ESP_RETURN_ON_ERROR(register_handler(HTTP_GET, "/api/device", handle_device), kTag, "Device handler registration failed");
    ESP_RETURN_ON_ERROR(register_handler(HTTP_GET, "/api/network", handle_network), kTag, "Network handler registration failed");
    ESP_RETURN_ON_ERROR(register_handler(HTTP_POST, "/api/wifi", handle_set_wifi), kTag, "Wi-Fi handler registration failed");
    ESP_RETURN_ON_ERROR(register_handler(HTTP_DELETE, "/api/wifi", handle_delete_wifi), kTag, "Wi-Fi delete handler registration failed");
    ESP_RETURN_ON_ERROR(register_handler(HTTP_POST, "/api/ap/password", handle_set_ap_password), kTag, "AP password handler registration failed");
    ESP_RETURN_ON_ERROR(mdns_service_add("ESP32 Display", "_http", "_tcp", config.server_port, nullptr, 0),
                        kTag, "mDNS HTTP service registration failed");
    ESP_LOGI(kTag, "HTTP API listening on port %d", config.server_port);
    return ESP_OK;
}
}
