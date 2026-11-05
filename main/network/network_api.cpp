#include "network_api.h"

#include <cstring>
#include <cstdlib>
#include <algorithm>

#include "cJSON.h"
#include "display/display_driver.h"
#include "image/image_decoder.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mdns.h"
#include "network_manager.h"

namespace {
constexpr char kTag[] = "network-api";
constexpr size_t kMaximumImageUpload = 1024 * 1024;
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
    static constexpr char page[] = R"HTML(<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP32 Display</title><style>body{font:16px system-ui;max-width:680px;margin:2rem auto;padding:0 1rem;color:#18202a}section{padding:1rem;margin:1rem 0;border:1px solid #ccd3dc;border-radius:10px}label{display:block;margin:.6rem 0}input,select,button{font:inherit;padding:.5rem}pre{white-space:pre-wrap}</style><h1>ESP32 Image Display</h1><section><h2>Status</h2><pre id="status">Loading…</pre><button onclick="refresh()">Refresh</button></section><section><h2>Upload image</h2><input id="image" type="file" accept="image/jpeg,image/png,image/bmp"><img id="preview" style="display:none;max-width:100%;margin-top:1rem"><label>Fit <select id="fit"><option>contain</option><option>cover</option><option>stretch</option></select></label><button onclick="upload()">Upload</button> <button onclick="clearDisplay()">Clear display</button></section><section><h2>Wi-Fi</h2><form id="wifi"><label>SSID <input name="ssid" required maxlength="32"></label><label>Password <input name="password" type="password" maxlength="64"></label><button>Save and connect</button></form><p id="message"></p></section><script>const msg=x=>document.getElementById('message').textContent=x;async function refresh(){try{let [d,n]=await Promise.all(['/api/device','/api/network'].map(x=>fetch(x).then(r=>r.json())));document.getElementById('status').textContent=JSON.stringify({device:d,network:n},null,2)}catch(e){msg(e.message)}}document.getElementById('image').onchange=e=>{let f=e.target.files[0],p=document.getElementById('preview');if(f){p.src=URL.createObjectURL(f);p.style.display='block'}};async function upload(){let f=document.getElementById('image').files[0];if(!f)return msg('Choose an image first');let r=await fetch('/api/image?fit='+document.getElementById('fit').value,{method:'POST',headers:{'Content-Type':f.type},body:f});msg(await r.text())}async function clearDisplay(){let r=await fetch('/api/image',{method:'DELETE'});msg(await r.text())}document.getElementById('wifi').onsubmit=async e=>{e.preventDefault();let o=Object.fromEntries(new FormData(e.target));let r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)});msg(await r.text());refresh()};refresh();setInterval(refresh,10000)</script></html>)HTML";
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
}

esp_err_t handle_clear_image(httpd_req_t *request)
{
    const esp_err_t error = display::clear();
    if (error != ESP_OK) return send_error(request, "500 Internal Server Error", esp_err_to_name(error));
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed");
    cJSON_AddBoolToObject(root, "success", true);
    return send_json(request, root);
}

esp_err_t handle_upload_image(httpd_req_t *request)
{
    if (request->content_len <= 0 || static_cast<size_t>(request->content_len) > kMaximumImageUpload) {
        return send_error(request, "413 Payload Too Large", "Image must be between 1 byte and 1 MiB");
    }
    char content_type[64] = {};
    if (httpd_req_get_hdr_value_str(request, "Content-Type", content_type, sizeof(content_type)) != ESP_OK) {
        return send_error(request, "415 Unsupported Media Type", "Content-Type is required");
    }
    image::Format format;
    if (std::strcmp(content_type, "image/jpeg") == 0) format = image::Format::jpeg;
    else if (std::strcmp(content_type, "image/png") == 0) format = image::Format::png;
    else if (std::strcmp(content_type, "image/bmp") == 0) format = image::Format::bmp;
    else return send_error(request, "415 Unsupported Media Type", "Supported types: image/jpeg, image/png, image/bmp");

    auto *upload = static_cast<uint8_t *>(heap_caps_malloc(request->content_len,
                                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (upload == nullptr) return send_error(request, "503 Service Unavailable", "Not enough PSRAM for image upload");
    size_t received = 0;
    while (received < static_cast<size_t>(request->content_len)) {
        const int count = httpd_req_recv(request, reinterpret_cast<char *>(upload) + received,
            std::min(static_cast<size_t>(4096), static_cast<size_t>(request->content_len) - received));
        if (count <= 0) {
            heap_caps_free(upload);
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Image upload was interrupted");
        }
        received += static_cast<size_t>(count);
    }
    const bool signature_matches = format == image::Format::jpeg
        ? received >= 3 && upload[0] == 0xFF && upload[1] == 0xD8 && upload[2] == 0xFF
        : format == image::Format::png
            ? received >= 8 && upload[0] == 0x89 && upload[1] == 'P' && upload[2] == 'N' && upload[3] == 'G' &&
              upload[4] == 0x0D && upload[5] == 0x0A && upload[6] == 0x1A && upload[7] == 0x0A
            : received >= 2 && upload[0] == 'B' && upload[1] == 'M';
    if (!signature_matches) {
        heap_caps_free(upload);
        return send_error(request, "400 Bad Request", "Image signature does not match Content-Type");
    }

    display::FitMode fit = display::FitMode::contain;
    char query[64] = {};
    char fit_value[16] = {};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "fit", fit_value, sizeof(fit_value)) == ESP_OK) {
        if (std::strcmp(fit_value, "cover") == 0) fit = display::FitMode::cover;
        else if (std::strcmp(fit_value, "stretch") == 0) fit = display::FitMode::stretch;
        else if (std::strcmp(fit_value, "contain") != 0) {
            heap_caps_free(upload);
            return send_error(request, "400 Bad Request", "fit must be contain, cover, or stretch");
        }
    }
    image::DecodedImage decoded;
    esp_err_t decode_error = ESP_ERR_INVALID_ARG;
    switch (format) {
    case image::Format::jpeg: decode_error = image::decode_jpeg(upload, received, decoded); break;
    case image::Format::png: decode_error = image::decode_png(upload, received, decoded); break;
    case image::Format::bmp: decode_error = image::decode_bmp(upload, received, decoded); break;
    }
    heap_caps_free(upload);
    if (decode_error != ESP_OK) {
        const char *message = decode_error == ESP_ERR_NO_MEM ? "Not enough memory to decode image" :
            "Image is malformed, unsupported, or exceeds 480x480 pixels";
        const char *status = decode_error == ESP_ERR_NO_MEM ? "503 Service Unavailable" : "400 Bad Request";
        return send_error(request, status, message);
    }
    const esp_err_t display_error = display::show_rgb565(decoded.pixels, decoded.width, decoded.height, fit);
    const int width = decoded.width;
    const int height = decoded.height;
    image::release(decoded);
    if (display_error != ESP_OK) return send_error(request, "500 Internal Server Error", esp_err_to_name(display_error));
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed");
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddNumberToObject(root, "width", width);
    cJSON_AddNumberToObject(root, "height", height);
    cJSON_AddStringToObject(root, "format", format == image::Format::jpeg ? "jpeg" :
                                             format == image::Format::png ? "png" : "bmp");
    cJSON_AddStringToObject(root, "fit", fit == display::FitMode::cover ? "cover" :
                                      fit == display::FitMode::stretch ? "stretch" : "contain");
    return send_json(request, root);
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
    config.max_uri_handlers = 10;
    config.stack_size = 6144;
    config.lru_purge_enable = true;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), kTag, "HTTP server startup failed");
    ESP_RETURN_ON_ERROR(register_handler(HTTP_GET, "/", handle_root), kTag, "Root handler registration failed");
    ESP_RETURN_ON_ERROR(register_handler(HTTP_POST, "/api/image", handle_upload_image), kTag, "Image upload handler registration failed");
    ESP_RETURN_ON_ERROR(register_handler(HTTP_DELETE, "/api/image", handle_clear_image), kTag, "Image clear handler registration failed");
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
