#include "care_ble.h"

#include <stdio.h>
#include <string.h>

#include "care_indicator.h"
#include "care_portal.h"
#include "esp_log.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
#include "freertos/FreeRTOS.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "os/os_mbuf.h"
#endif

static const char *TAG = "care_ble";
static bool ble_started;

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
#define CARE_BLE_SERVICE_UUID 0xFFF0
#define CARE_BLE_STATUS_UUID 0xFFF1
#define CARE_BLE_COMMAND_UUID 0xFFF2

static uint8_t own_address_type;
static const ble_uuid16_t service_uuid = BLE_UUID16_INIT(CARE_BLE_SERVICE_UUID);
static const ble_uuid16_t status_uuid = BLE_UUID16_INIT(CARE_BLE_STATUS_UUID);
static const ble_uuid16_t command_uuid = BLE_UUID16_INIT(CARE_BLE_COMMAND_UUID);
static void ble_advertise(void);

void ble_store_config_init(void);

static int queue_command_text(const char *text, size_t length)
{
    static const char *names[] = {"start", "stop", "cancel", "test", "share_on", "share_off", "reminder_ack"};
    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (strlen(names[index]) == length && strncmp(text, names[index], length) == 0) {
            return care_portal_enqueue_command((care_command_t)index) == ESP_OK ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int ble_status_access(uint16_t connection_handle, uint16_t attribute_handle,
    struct ble_gatt_access_ctxt *context, void *argument)
{
    (void)connection_handle;
    (void)attribute_handle;
    (void)argument;
    if (context->op == BLE_GATT_ACCESS_OP_READ_CHR && ble_uuid_cmp(context->chr->uuid, &status_uuid.u) == 0) {
        care_status_t status;
        care_portal_get_status(&status);
        char text[160];
        int length = snprintf(text, sizeof(text),
            "m=%u,p=%u,v=%u,a=%u,imu=%u,x=%d,y=%d,z=%d,g=%u,s=%u",
            status.monitoring ? 1 : 0, status.pose, status.motion, status.alert_state,
            status.imu_ready ? 1 : 0, status.imu_x_mg, status.imu_y_mg, status.imu_z_mg,
            status.imu_magnitude_mg, status.imu_shock_mg);
        return os_mbuf_append(context->om, text, length) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (context->op == BLE_GATT_ACCESS_OP_WRITE_CHR && ble_uuid_cmp(context->chr->uuid, &command_uuid.u) == 0) {
        char text[32] = {0};
        uint16_t length = 0;
        if (ble_hs_mbuf_to_flat(context->om, text, sizeof(text) - 1, &length) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (length == 1 && text[0] >= '1' && text[0] <= '7') {
            return care_portal_enqueue_command((care_command_t)(text[0] - '1')) == ESP_OK ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        return queue_command_text(text, length);
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &status_uuid.u,
                .access_cb = ble_status_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &command_uuid.u,
                .access_cb = ble_status_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {0}
        },
    },
    {0}
};

static int ble_gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) {
                ble_advertise();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ble_advertise();
            break;
        default:
            break;
    }
    return 0;
}

static void ble_advertise(void)
{
    struct ble_hs_adv_fields fields;
    struct ble_gap_adv_params parameters;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    fields.uuids16 = (ble_uuid16_t[]) {BLE_UUID16_INIT(CARE_BLE_SERVICE_UUID)};
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    int result = ble_gap_adv_set_fields(&fields);
    if (result != 0) {
        ESP_LOGE(TAG, "BLE advertisement data failed: %d", result);
        return;
    }
    memset(&parameters, 0, sizeof(parameters));
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    result = ble_gap_adv_start(own_address_type, NULL, BLE_HS_FOREVER, &parameters, ble_gap_event, NULL);
    if (result != 0) ESP_LOGE(TAG, "BLE advertisement start failed: %d", result);
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset: %d", reason);
}

static void ble_on_sync(void)
{
    int result = ble_hs_id_infer_auto(0, &own_address_type);
    if (result != 0) {
        ESP_LOGE(TAG, "BLE address setup failed: %d", result);
        return;
    }
    care_indicator_set_ble(true);
    ble_advertise();
    ESP_LOGI(TAG, "BLE advertising as AI-Care-E421");
}

static void ble_host_task(void *argument)
{
    (void)argument;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t care_ble_init(void)
{
    if (ble_started) return ESP_OK;
    esp_err_t result = nimble_port_init();
    if (result != ESP_OK) return result;
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_svc_gap_init();
    int rc = 0;
    ble_svc_gatt_init();
    rc = ble_gatts_count_cfg(services);
    if (rc != 0) return ESP_FAIL;
    rc = ble_gatts_add_svcs(services);
    if (rc != 0) return ESP_FAIL;
    rc = ble_svc_gap_device_name_set("AI-Care-E421");
    if (rc != 0) return ESP_FAIL;
    ble_store_config_init();
    ble_started = true;
    nimble_port_freertos_init(ble_host_task);
    return ESP_OK;
}
#else
esp_err_t care_ble_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

bool care_ble_ready(void)
{
    return ble_started;
}
