#include "care_portal.h"
#include "care_store.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define CARE_JPEG_CAPACITY 65536
#define CARE_SHARE_SECONDS 300
#define CARE_HTML_CHUNK_SIZE 2048

extern const uint8_t dashboard_start[] asm("_binary_dashboard_html_start");
extern const uint8_t dashboard_end[] asm("_binary_dashboard_html_end");

static SemaphoreHandle_t portal_lock;
static QueueHandle_t commands;
static care_status_t runtime_status;
static char access_point_name[24];
static char device_key[13];
static char station_ip[16];
static bool station_connected;
static uint32_t disconnect_reason;
static uint8_t *jpeg_cache;
static size_t jpeg_size;
static uint32_t jpeg_sequence;
static int64_t jpeg_updated_us;
static int64_t sharing_until_us;
static httpd_handle_t control_server;
static httpd_handle_t video_server;
static bool video_available;

static const char *http_method_name(int method)
{
    switch (method) {
        case HTTP_GET: return "GET";
        case HTTP_POST: return "POST";
        case HTTP_PUT: return "PUT";
        case HTTP_DELETE: return "DELETE";
        case HTTP_HEAD: return "HEAD";
        default: return "OTHER";
    }
}

static void log_http_result(httpd_req_t *request, esp_err_t result)
{
    ESP_LOGI("care_http", "%s %s -> %s", http_method_name(request->method), request->uri,
        esp_err_to_name(result));
}

static bool sharing_locked(void)
{
    return sharing_until_us > esp_timer_get_time();
}

bool care_portal_sharing(void)
{
    if (!portal_lock) return false;
    xSemaphoreTake(portal_lock, portMAX_DELAY);
    bool enabled = sharing_locked();
    xSemaphoreGive(portal_lock);
    return enabled;
}

esp_err_t care_portal_set_sharing(bool enabled)
{
    if (!portal_lock || (enabled && !video_available)) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(portal_lock, portMAX_DELAY);
    if (enabled && !jpeg_cache) jpeg_cache = heap_caps_malloc(CARE_JPEG_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (enabled && !jpeg_cache) {
        xSemaphoreGive(portal_lock);
        return ESP_ERR_NO_MEM;
    }
    sharing_until_us = enabled ? esp_timer_get_time() + CARE_SHARE_SECONDS * 1000000LL : 0;
    jpeg_size = 0;
    xSemaphoreGive(portal_lock);
    return ESP_OK;
}

void care_portal_publish_status(const care_status_t *status)
{
    if (!portal_lock) return;
    xSemaphoreTake(portal_lock, portMAX_DELAY);
    runtime_status = *status;
    if (!status->monitoring) {
        sharing_until_us = 0;
        jpeg_size = 0;
    }
    xSemaphoreGive(portal_lock);
}

void care_portal_publish_jpeg(const uint8_t *data, size_t size)
{
    if (!portal_lock || size == 0 || size > CARE_JPEG_CAPACITY) return;
    if (xSemaphoreTake(portal_lock, pdMS_TO_TICKS(10)) != pdTRUE) return;
    if (sharing_locked() && jpeg_cache) {
        memcpy(jpeg_cache, data, size);
        jpeg_size = size;
        jpeg_updated_us = esp_timer_get_time();
        jpeg_sequence++;
    }
    xSemaphoreGive(portal_lock);
}

bool care_portal_next_command(care_command_t *command)
{
    return commands && xQueueReceive(commands, command, 0) == pdTRUE;
}

esp_err_t care_portal_enqueue_command(care_command_t command)
{
    if (!commands || command > CARE_COMMAND_REMINDER_ACK) return ESP_ERR_INVALID_STATE;
    return xQueueSend(commands, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void care_portal_get_status(care_status_t *status)
{
    if (!status) return;
    if (!portal_lock) {
        memset(status, 0, sizeof(*status));
        return;
    }
    xSemaphoreTake(portal_lock, portMAX_DELAY);
    *status = runtime_status;
    xSemaphoreGive(portal_lock);
}

void care_portal_network_text(char *buffer, size_t size)
{
    if (!control_server) {
        snprintf(buffer, size, "Network: unavailable\nCheck serial log\nCamera still usable");
        return;
    }
    snprintf(buffer, size, "%s\nKey:%s\n192.168.4.1", access_point_name, device_key);
}

static esp_err_t json_reply(httpd_req_t *request, const char *status, const char *body)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    return httpd_resp_sendstr(request, body);
}

static bool authorized(httpd_req_t *request, bool query_allowed)
{
    char supplied[32] = {0};
    bool found = httpd_req_get_hdr_value_str(request, "X-Care-Key", supplied, sizeof(supplied)) == ESP_OK;
    if (!found && query_allowed) {
        char query[96];
        found = httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK &&
            httpd_query_key_value(query, "key", supplied, sizeof(supplied)) == ESP_OK;
    }
    unsigned difference = (unsigned)(strlen(supplied) ^ 12);
    for (size_t index = 0; index < 12; ++index) difference |= (unsigned)(supplied[index] ^ device_key[index]);
    if (found && difference == 0) return true;
    json_reply(request, "401 Unauthorized", "{\"error\":\"Device key required; see SETTINGS / Network\"}");
    return false;
}

static bool read_form(httpd_req_t *request, char *buffer, size_t capacity)
{
    char content_type[96];
    if (httpd_req_get_hdr_value_str(request, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strncmp(content_type, "application/x-www-form-urlencoded", 33) != 0) {
        json_reply(request, "415 Unsupported Media Type", "{\"error\":\"Form encoding required\"}");
        return false;
    }
    if (request->content_len == 0 || request->content_len >= capacity) {
        json_reply(request, "413 Payload Too Large", "{\"error\":\"Invalid body size\"}");
        return false;
    }
    size_t received = 0;
    while (received < request->content_len) {
        int amount = httpd_req_recv(request, buffer + received, request->content_len - received);
        if (amount <= 0) return false;
        received += amount;
    }
    if (memchr(buffer, 0, received)) return false;
    buffer[received] = 0;
    return true;
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool form_value(const char *form, const char *name, char *output, size_t capacity)
{
    char encoded[256];
    if (httpd_query_key_value(form, name, encoded, sizeof(encoded)) != ESP_OK) return false;
    size_t target = 0;
    for (size_t source = 0; encoded[source]; ++source) {
        unsigned char value = encoded[source];
        if (value == '+') value = ' ';
        else if (value == '%') {
            if (!encoded[source + 1] || !encoded[source + 2]) return false;
            int high = hex_value(encoded[source + 1]);
            int low = hex_value(encoded[source + 2]);
            if (high < 0 || low < 0) return false;
            value = (unsigned char)(high * 16 + low);
            source += 2;
        }
        if (value < 32 || value == 127 || target + 1 >= capacity) return false;
        output[target++] = (char)value;
    }
    output[target] = 0;
    return true;
}

static bool form_number(const char *form, const char *name, uint32_t maximum, uint32_t *value)
{
    char text[16];
    if (!form_value(form, name, text, sizeof(text)) || !text[0] || strspn(text, "0123456789") != strlen(text)) return false;
    unsigned long long parsed = strtoull(text, NULL, 10);
    if (parsed > maximum) return false;
    *value = (uint32_t)parsed;
    return true;
}

static void json_escape(const char *input, char *output, size_t capacity)
{
    size_t used = 0;
    for (size_t index = 0; input[index] && used + 7 < capacity; ++index) {
        unsigned char value = input[index];
        if (value == '"' || value == '\\') output[used++] = '\\';
        if (value < 32) used += (size_t)snprintf(output + used, capacity - used, "\\u%04x", value);
        else output[used++] = (char)value;
    }
    output[used] = 0;
}

static esp_err_t health_handler(httpd_req_t *request)
{
    esp_err_t result = httpd_resp_set_type(request, "text/plain; charset=utf-8");
    if (result == ESP_OK) result = httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (result == ESP_OK) result = httpd_resp_sendstr(request, "CARE PORTAL OK\n");
    log_http_result(request, result);
    return result;
}

static esp_err_t not_found_handler(httpd_req_t *request, httpd_err_code_t error)
{
    (void)error;
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    esp_err_t result = httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "CARE PORTAL: route not found");
    log_http_result(request, result);
    return result;
}

static esp_err_t dashboard_handler(httpd_req_t *request)
{
    esp_err_t result = httpd_resp_set_type(request, "text/html; charset=utf-8");
    if (result == ESP_OK) result = httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (result == ESP_OK) result = httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    if (result == ESP_OK) result = httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
    if (result == ESP_OK) result = httpd_resp_set_hdr(request, "Content-Security-Policy", "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src 'self' http: blob:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'");
    size_t remaining = dashboard_end - dashboard_start;
    if (remaining > 0 && dashboard_end[-1] == 0) remaining--;
    for (size_t offset = 0; result == ESP_OK && offset < remaining; offset += CARE_HTML_CHUNK_SIZE) {
        size_t length = remaining - offset;
        if (length > CARE_HTML_CHUNK_SIZE) length = CARE_HTML_CHUNK_SIZE;
        result = httpd_resp_send_chunk(request, (const char *)dashboard_start + offset, length);
    }
    if (result == ESP_OK) result = httpd_resp_send_chunk(request, NULL, 0);
    log_http_result(request, result);
    return result;
}

static esp_err_t status_handler(httpd_req_t *request)
{
    if (!authorized(request, false)) return ESP_OK;
    care_status_t snapshot;
    char ip[16];
    bool connected;
    bool shared;
    uint32_t reason;
    uint32_t remaining;
    xSemaphoreTake(portal_lock, portMAX_DELAY);
    snapshot = runtime_status;
    connected = station_connected;
    shared = sharing_locked();
    reason = disconnect_reason;
    remaining = shared ? (uint32_t)((sharing_until_us - esp_timer_get_time()) / 1000000) : 0;
    memcpy(ip, station_ip, sizeof(ip));
    xSemaphoreGive(portal_lock);
    care_config_t config;
    care_store_get_config(&config);
    char escaped_ssid[208];
    json_escape(config.ssid, escaped_ssid, sizeof(escaped_ssid));
    char body[1536];
    snprintf(body, sizeof(body),
        "{\"firmware\":\"care-portal-1\",\"platform\":\"ESP-IDF\",\"mode\":\"DEMO\","
        "\"uptime\":%" PRIu64 ",\"camera_ready\":%s,\"camera_error\":%s,\"monitoring\":%s,"
        "\"calibrated\":%s,\"base\":%u,\"samples\":%lu,\"pose\":%u,\"motion\":%u,"
        "\"alert\":%u,\"test_alert\":%s,\"confirm_remaining\":%lu,\"reminder_due\":%s,"
        "\"sharing\":%s,\"share_remaining\":%lu,\"video_available\":%s,"
        "\"ap_ssid\":\"%s\",\"ap_ip\":\"192.168.4.1\",\"wifi_connected\":%s,"
        "\"wifi_ssid\":\"%s\",\"station_ip\":\"%s\",\"wifi_reason\":%lu,"
        "\"free_heap\":%lu,\"min_heap\":%lu,\"storage_errors\":%lu,"
        "\"sensors_connected\":%s,\"imu_connected\":%s,\"imu_x_mg\":%d,\"imu_y_mg\":%d,\"imu_z_mg\":%d,"
        "\"imu_magnitude_mg\":%u,\"imu_shock_mg\":%u,\"imu_samples\":%lu,\"imu_errors\":%lu,"
        "\"audio_connected\":%s,\"audio_level\":%u,\"audio_samples\":%lu,\"audio_errors\":%lu,"
        "\"ble_ready\":%s,\"cloud_connected\":false}",
        (uint64_t)esp_timer_get_time() / 1000000,
        snapshot.camera_ready ? "true" : "false", snapshot.camera_error ? "true" : "false",
        snapshot.monitoring ? "true" : "false", snapshot.baseline_ready ? "true" : "false",
        snapshot.baseline_samples, (unsigned long)snapshot.samples, snapshot.pose, snapshot.motion,
        snapshot.alert_state, snapshot.test_alert ? "true" : "false", (unsigned long)snapshot.confirm_remaining,
        snapshot.reminder_due ? "true" : "false", shared ? "true" : "false", (unsigned long)remaining,
        video_available ? "true" : "false", access_point_name, connected ? "true" : "false", escaped_ssid, ip,
        (unsigned long)reason, (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(), (unsigned long)care_store_error_count(),
        snapshot.imu_ready ? "true" : "false", snapshot.imu_ready ? "true" : "false",
        snapshot.imu_x_mg, snapshot.imu_y_mg, snapshot.imu_z_mg, snapshot.imu_magnitude_mg,
        snapshot.imu_shock_mg, (unsigned long)snapshot.imu_samples, (unsigned long)snapshot.imu_errors,
        snapshot.audio_ready ? "true" : "false", snapshot.audio_level,
        (unsigned long)snapshot.audio_samples, (unsigned long)snapshot.audio_errors,
        snapshot.ble_ready ? "true" : "false");
    return json_reply(request, "200 OK", body);
}

static esp_err_t config_get_handler(httpd_req_t *request)
{
    if (!authorized(request, false)) return ESP_OK;
    care_config_t config;
    care_store_get_config(&config);
    char body[256];
    snprintf(body, sizeof(body), "{\"pose\":%u,\"recovery\":%u,\"motion\":%u,\"hold\":%u,\"delay\":%u,\"reminder\":%u}",
        config.pose_threshold, config.recovery_threshold, config.motion_limit, config.hold_samples,
        config.confirm_seconds, config.reminder_minutes);
    return json_reply(request, "200 OK", body);
}

static esp_err_t config_post_handler(httpd_req_t *request)
{
    if (!authorized(request, false)) return ESP_OK;
    char form[384];
    if (!read_form(request, form, sizeof(form))) return ESP_FAIL;
    care_config_t config;
    care_store_get_config(&config);
    uint32_t pose, recovery, motion, hold, delay, reminder;
    if (!form_number(form, "pose", 100, &pose) || !form_number(form, "recovery", 100, &recovery) ||
        !form_number(form, "motion", 100, &motion) || !form_number(form, "hold", 20, &hold) ||
        !form_number(form, "delay", 60, &delay) || !form_number(form, "reminder", 1440, &reminder)) {
        return json_reply(request, "400 Bad Request", "{\"error\":\"Invalid numeric setting\"}");
    }
    config.pose_threshold = pose;
    config.recovery_threshold = recovery;
    config.motion_limit = motion;
    config.hold_samples = hold;
    config.confirm_seconds = delay;
    config.reminder_minutes = reminder;
    if (!care_config_valid(&config)) return json_reply(request, "400 Bad Request", "{\"error\":\"Settings out of range; recovery must be lower than pose\"}");
    if (care_store_save_config(&config) != ESP_OK) return json_reply(request, "500 Internal Server Error", "{\"error\":\"Could not save settings\"}");
    care_store_record(CARE_EVENT_SETTINGS, 0, 0);
    return json_reply(request, "200 OK", "{\"saved\":true,\"apply\":\"next_monitoring_session\"}");
}

static esp_err_t wifi_handler(httpd_req_t *request)
{
    if (!authorized(request, false)) return ESP_OK;
    char form[512];
    if (!read_form(request, form, sizeof(form))) return ESP_FAIL;
    care_config_t config;
    care_store_get_config(&config);
    char ssid[33], password[65];
    if (!form_value(form, "ssid", ssid, sizeof(ssid)) || !form_value(form, "password", password, sizeof(password))) {
        return json_reply(request, "400 Bad Request", "{\"error\":\"Invalid SSID or password\"}");
    }
    if (!ssid[0]) {
        config.ssid[0] = 0;
        config.wifi_password[0] = 0;
    } else {
        if (strlen(password) < 8 || strlen(password) > 63) return json_reply(request, "400 Bad Request", "{\"error\":\"WPA2 password must be 8-63 bytes\"}");
        snprintf(config.ssid, sizeof(config.ssid), "%s", ssid);
        snprintf(config.wifi_password, sizeof(config.wifi_password), "%s", password);
    }
    if (care_store_save_config(&config) != ESP_OK) return json_reply(request, "500 Internal Server Error", "{\"error\":\"Could not save Wi-Fi\"}");
    return json_reply(request, "200 OK", "{\"saved\":true,\"connected\":false,\"note\":\"Check status after reconnect attempt\"}");
}

static esp_err_t command_handler(httpd_req_t *request)
{
    if (!authorized(request, false)) return ESP_OK;
    char form[128], name[32];
    if (!read_form(request, form, sizeof(form))) return ESP_FAIL;
    if (!form_value(form, "action", name, sizeof(name))) return json_reply(request, "400 Bad Request", "{\"error\":\"Missing action\"}");
    static const char *names[] = {"start", "stop", "cancel", "test", "share_on", "share_off", "reminder_ack"};
    for (unsigned index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (strcmp(name, names[index]) != 0) continue;
        care_command_t command = (care_command_t)index;
        xSemaphoreTake(portal_lock, portMAX_DELAY);
        bool alert_active = runtime_status.alert_state != 0;
        bool monitoring = runtime_status.monitoring;
        xSemaphoreGive(portal_lock);
        if (alert_active && (command == CARE_COMMAND_START || command == CARE_COMMAND_TEST)) {
            return json_reply(request, "409 Conflict", "{\"error\":\"Cancel the current alert first\"}");
        }
        if (command == CARE_COMMAND_SHARE_ON && (!monitoring || !video_available)) {
            return json_reply(request, "409 Conflict", "{\"error\":\"Start monitoring before sharing video\"}");
        }
        if (xQueueSend(commands, &command, 0) != pdTRUE) return json_reply(request, "503 Service Unavailable", "{\"error\":\"Command queue full\"}");
        return json_reply(request, "202 Accepted", "{\"queued\":true}");
    }
    return json_reply(request, "400 Bad Request", "{\"error\":\"Unknown action\"}");
}

static esp_err_t history_handler(httpd_req_t *request)
{
    if (!authorized(request, false)) return ESP_OK;
    care_event_t events[CARE_HISTORY_CAPACITY];
    size_t count = care_store_history(events, CARE_HISTORY_CAPACITY);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (httpd_resp_sendstr_chunk(request, "{\"events\":[") != ESP_OK) return ESP_FAIL;
    char entry[256];
    for (size_t index = 0; index < count; ++index) {
        snprintf(entry, sizeof(entry), "%s{\"id\":%lu,\"boot\":%lu,\"uptime\":%" PRIu64 ",\"time\":%" PRId64 ",\"kind\":\"%s\",\"pose\":%u,\"motion\":%u}",
            index ? "," : "", (unsigned long)events[index].id, (unsigned long)events[index].boot_id,
            events[index].uptime_seconds, events[index].unix_seconds,
            care_event_name((care_event_kind_t)events[index].kind), events[index].pose, events[index].motion);
        if (httpd_resp_sendstr_chunk(request, entry) != ESP_OK) return ESP_FAIL;
    }
    if (httpd_resp_sendstr_chunk(request, "]}") != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t history_clear_handler(httpd_req_t *request)
{
    if (!authorized(request, false)) return ESP_OK;
    char form[64], value[16];
    if (!read_form(request, form, sizeof(form))) return ESP_FAIL;
    if (!form_value(form, "confirm", value, sizeof(value)) || strcmp(value, "clear") != 0) {
        return json_reply(request, "400 Bad Request", "{\"error\":\"Confirmation required\"}");
    }
    if (care_store_clear_history() != ESP_OK) return json_reply(request, "500 Internal Server Error", "{\"error\":\"Could not clear history\"}");
    return json_reply(request, "200 OK", "{\"cleared\":true}");
}

static esp_err_t time_handler(httpd_req_t *request)
{
    if (!authorized(request, false)) return ESP_OK;
    char form[64];
    uint32_t epoch;
    if (!read_form(request, form, sizeof(form))) return ESP_FAIL;
    if (!form_number(form, "epoch", 4102444800UL, &epoch) || epoch < 1704067200) return json_reply(request, "400 Bad Request", "{\"error\":\"Invalid clock value\"}");
    struct timeval now = {.tv_sec = (time_t)epoch};
    if (settimeofday(&now, NULL) != 0) return json_reply(request, "500 Internal Server Error", "{\"error\":\"Clock sync failed\"}");
    return json_reply(request, "200 OK", "{\"synced\":true}");
}

static esp_err_t stream_handler(httpd_req_t *request)
{
    if (!authorized(request, true)) return ESP_OK;
    if (!care_portal_sharing()) return json_reply(request, "403 Forbidden", "{\"error\":\"Video sharing is OFF\"}");
    uint8_t *frame = heap_caps_malloc(CARE_JPEG_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame) return json_reply(request, "503 Service Unavailable", "{\"error\":\"Video memory unavailable\"}");
    httpd_resp_set_type(request, "multipart/x-mixed-replace;boundary=careframe");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    uint32_t previous_sequence = 0;
    int64_t last_sent_us = esp_timer_get_time();
    esp_err_t result = ESP_OK;
    while (true) {
        xSemaphoreTake(portal_lock, portMAX_DELAY);
        bool enabled = sharing_locked();
        size_t size = enabled && jpeg_size && jpeg_sequence != previous_sequence &&
            esp_timer_get_time() - jpeg_updated_us < 3000000 ? jpeg_size : 0;
        if (size) {
            memcpy(frame, jpeg_cache, size);
            previous_sequence = jpeg_sequence;
        }
        xSemaphoreGive(portal_lock);
        if (!enabled || esp_timer_get_time() - last_sent_us > 5000000) break;
        if (size) {
            char header[96];
            int length = snprintf(header, sizeof(header), "\r\n--careframe\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned)size);
            result = httpd_resp_send_chunk(request, header, length);
            if (result == ESP_OK) result = httpd_resp_send_chunk(request, (const char *)frame, size);
            if (result != ESP_OK) break;
            last_sent_us = esp_timer_get_time();
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    free(frame);
    if (result == ESP_OK) result = httpd_resp_send_chunk(request, NULL, 0);
    return result;
}

static void wifi_event(void *argument, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)argument;
    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        xSemaphoreTake(portal_lock, portMAX_DELAY);
        station_connected = true;
        disconnect_reason = 0;
        snprintf(station_ip, sizeof(station_ip), IPSTR, IP2STR(&event->ip_info.ip));
        xSemaphoreGive(portal_lock);
        care_store_record(CARE_EVENT_WIFI_CONNECTED, 0, 0);
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = data;
        xSemaphoreTake(portal_lock, portMAX_DELAY);
        bool was_connected = station_connected;
        station_connected = false;
        station_ip[0] = 0;
        disconnect_reason = event->reason;
        xSemaphoreGive(portal_lock);
        if (was_connected) care_store_record(CARE_EVENT_WIFI_LOST, 0, 0);
    }
}

static void wifi_reconnect_task(void *argument)
{
    (void)argument;
    char applied_ssid[33] = {0};
    char applied_password[65] = {0};
    unsigned attempts = 0;
    while (true) {
        care_config_t config;
        care_store_get_config(&config);
        bool changed = strcmp(config.ssid, applied_ssid) != 0 || strcmp(config.wifi_password, applied_password) != 0;
        if (changed) {
            esp_wifi_disconnect();
            wifi_config_t station = {0};
            memcpy(station.sta.ssid, config.ssid, strlen(config.ssid));
            memcpy(station.sta.password, config.wifi_password, strlen(config.wifi_password));
            station.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
            esp_err_t result = esp_wifi_set_config(WIFI_IF_STA, &station);
            if (result != ESP_OK) {
                ESP_LOGE("care_net", "Station config failed: %s", esp_err_to_name(result));
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
            snprintf(applied_ssid, sizeof(applied_ssid), "%s", config.ssid);
            snprintf(applied_password, sizeof(applied_password), "%s", config.wifi_password);
            attempts = 0;
        }
        xSemaphoreTake(portal_lock, portMAX_DELAY);
        bool connected = station_connected;
        xSemaphoreGive(portal_lock);
        if (applied_ssid[0] && !connected) {
            esp_wifi_connect();
            if (attempts < 6) attempts++;
        } else attempts = 0;
        vTaskDelay(pdMS_TO_TICKS(attempts >= 6 ? 30000 : 10000));
    }
}
esp_err_t care_portal_init(void)
{
    portal_lock = xSemaphoreCreateMutex();
    commands = xQueueCreate(8, sizeof(care_command_t));
    if (!portal_lock || !commands) return ESP_ERR_NO_MEM;
    care_config_t config;
    care_store_get_config(&config);
    memcpy(device_key, config.device_key, sizeof(device_key));
    uint8_t mac[6];
    esp_err_t result = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (result != ESP_OK) return result;
    snprintf(access_point_name, sizeof(access_point_name), "AI-Care-%02X%02X", mac[4], mac[5]);
    result = esp_netif_init();
    if (result != ESP_OK) return result;
    result = esp_event_loop_create_default();
    if (result != ESP_OK) return result;
    if (!esp_netif_create_default_wifi_ap() || !esp_netif_create_default_wifi_sta()) return ESP_ERR_NO_MEM;
    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&wifi_init);
    if (result != ESP_OK) return result;
    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result != ESP_OK) return result;
    if (result != ESP_OK) return result;
    result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    if (result != ESP_OK) return result;
    result = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    if (result != ESP_OK) return result;
    wifi_config_t access_point = {0};
    memcpy(access_point.ap.ssid, access_point_name, strlen(access_point_name));
    access_point.ap.ssid_len = strlen(access_point_name);
    memcpy(access_point.ap.password, device_key, strlen(device_key));
    access_point.ap.channel = 1;
    access_point.ap.max_connection = 3;
    access_point.ap.authmode = WIFI_AUTH_WPA2_PSK;
    result = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (result != ESP_OK) return result;
    result = esp_wifi_set_config(WIFI_IF_AP, &access_point);
    if (result != ESP_OK) return result;
    result = esp_wifi_start();
    if (result != ESP_OK) return result;
    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.stack_size = 8192;
    server_config.max_uri_handlers = 12;
    server_config.max_open_sockets = 3;
    server_config.lru_purge_enable = true;
    server_config.recv_wait_timeout = 3;
    server_config.send_wait_timeout = 3;
    result = httpd_start(&control_server, &server_config);
    if (result != ESP_OK) return result;
    result = httpd_register_err_handler(control_server, HTTPD_404_NOT_FOUND, not_found_handler);
    if (result != ESP_OK) return result;
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = dashboard_handler},
        {.uri = "/health", .method = HTTP_GET, .handler = health_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler},
        {.uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_handler},
        {.uri = "/api/command", .method = HTTP_POST, .handler = command_handler},
        {.uri = "/api/history", .method = HTTP_GET, .handler = history_handler},
        {.uri = "/api/history/clear", .method = HTTP_POST, .handler = history_clear_handler},
        {.uri = "/api/time", .method = HTTP_POST, .handler = time_handler},
    };
    for (size_t index = 0; index < sizeof(routes) / sizeof(routes[0]); ++index) {
        result = httpd_register_uri_handler(control_server, &routes[index]);
        if (result != ESP_OK) return result;
    }
    server_config.server_port = 81;
    server_config.ctrl_port++;
    server_config.stack_size = 4096;
    server_config.max_uri_handlers = 1;
    server_config.max_open_sockets = 1;
    result = httpd_start(&video_server, &server_config);
    if (result == ESP_OK) {
        httpd_uri_t stream_route = {.uri = "/stream", .method = HTTP_GET, .handler = stream_handler};
        result = httpd_register_uri_handler(video_server, &stream_route);
    }
    video_available = result == ESP_OK;
    if (!video_available) ESP_LOGE("care_net", "Video server unavailable: %s", esp_err_to_name(result));
    if (xTaskCreate(wifi_reconnect_task, "care_wifi", 4096, NULL, 1, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI("care_net", "Portal ready: %s at http://192.168.4.1 (key on device Network page)", access_point_name);
    return ESP_OK;
}
