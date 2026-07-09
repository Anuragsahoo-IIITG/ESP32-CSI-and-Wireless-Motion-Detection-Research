#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"

static const uint8_t SENDER_MAC[6] = {0xf4, 0x65, 0x0b, 0x54, 0xa6, 0x5c};
#define WIFI_SSID         "Google Pixel 8"
#define WIFI_PASS         "biteload"
#define TAG               "CSI_RECV"

#define WINDOW_SIZE    100
#define SUBCARRIER_NUM  52
#define CALIB_PACKETS   60
#define MOTION_RATIO   2.5f

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static float csi_buf[WINDOW_SIZE][SUBCARRIER_NUM];
static int   buf_idx     = 0, buf_count = 0;
static float baseline    = 0.0f;
static int   calib_count = 0;
static bool  calibrated  = false;
static volatile uint32_t s_csi_packet_count = 0;
static esp_timer_handle_t s_reconnect_timer = NULL;

static int extract_amplitude(const int8_t *raw, int len, float *amp) {
    int n = 0;
    for (int i = 0; i + 1 < len && n < SUBCARRIER_NUM; i += 2)
        amp[n++] = sqrtf((float)raw[i] * raw[i] + (float)raw[i+1] * raw[i+1]);
    return n;
}

static float variance_1d(const float *a, int n) {
    if (n < 2) return 0.0f;
    float sum = 0.0f;
    for (int i = 0; i < n; i++) sum += a[i];
    float mean = sum / n, v = 0.0f;
    for (int i = 0; i < n; i++) { float d = a[i] - mean; v += d * d; }
    return v / n;
}

static float compute_motion_metric(void) {
    int cnt = (buf_count < WINDOW_SIZE) ? buf_count : WINDOW_SIZE;
    if (cnt < 5) return 0.0f;
    float total = 0.0f, col[WINDOW_SIZE];
    for (int s = 0; s < SUBCARRIER_NUM; s++) {
        for (int i = 0; i < cnt; i++) col[i] = csi_buf[i][s];
        total += variance_1d(col, cnt);
    }
    return total / SUBCARRIER_NUM;
}

static void wifi_csi_cb(void *ctx, wifi_csi_info_t *d) {
    if (!d || !d->buf || d->len < 4) return;
    if (memcmp(d->mac, SENDER_MAC, 6) != 0) return;

    float amp[SUBCARRIER_NUM];
    int n = extract_amplitude(d->buf, d->len, amp);
    if (n < 10) return;

    s_csi_packet_count++;
    memcpy(csi_buf[buf_idx % WINDOW_SIZE], amp, n * sizeof(float));
    buf_idx++;
    buf_count++;

    if (!calibrated) {
        if (buf_count < 5) return;
        float metric = compute_motion_metric();
        baseline = (baseline * calib_count + metric) / (calib_count + 1);
        calib_count++;

        if (calib_count % 10 == 0) {
            ESP_LOGI(TAG, "Calibrating %d/%d  metric=%.4f", calib_count, CALIB_PACKETS, metric);
            printf("CALIB,%d,%d,%.4f\n", calib_count, CALIB_PACKETS, metric);
            fflush(stdout);
        }
        if (calib_count >= CALIB_PACKETS) {
            calibrated = true;
            ESP_LOGI(TAG, "✔ Calibrated! Baseline=%.4f", baseline);
            printf("CALIB_DONE,%.4f\n", baseline);
            fflush(stdout);
        }
        return;
    }

    float metric = compute_motion_metric();
    float ratio  = metric / (baseline + 1e-4f);
    bool  motion = (ratio > MOTION_RATIO);

    // Only adapt the baseline when the room is quiet
    if (!motion) {
        baseline = baseline * 0.98f + metric * 0.02f;
    }

    // Print the machine-readable line and flush immediately
    printf("DATA,%.4f,%.4f,%d\n", ratio, metric, motion ? 1 : 0);
    fflush(stdout);
}

static void reconnect_timer_cb(void *arg) {
    ESP_LOGI(TAG, "Retrying connection...");
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Disconnected, reason=%d. Retrying in 5s...", disc->reason);
        esp_timer_start_once(s_reconnect_timer, 5000000);
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        esp_timer_stop(s_reconnect_timer);
    }
}

static void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .name     = "wifi_reconnect"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));

    esp_event_handler_instance_t inst_any, inst_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &inst_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &inst_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .capable = true, .required = false },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "Connected to hotspot: %s", WIFI_SSID);
}

static void wifi_promiscuous_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type) {}

static void csi_init(void) {
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_MGMT,
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_rx_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_LOGI(TAG, "Promiscuous mode ON");

    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(wifi_csi_cb, NULL));
    wifi_csi_config_t config = {
        .lltf_en = true, .htltf_en = true, .stbc_htltf2_en = true,
        .ltf_merge_en = true, .channel_filter_en = false,
        .manu_scale = false, .shift = 0, .dump_ack_en = false,
    };
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&config));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
    ESP_LOGI(TAG, "CSI ON — stand still for %d packets (calibration)…", CALIB_PACKETS);
}

static void csi_debug_task(void *pv) {
    uint32_t last = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint32_t current = s_csi_packet_count;
        ESP_LOGI(TAG, "[DEBUG] CSI packets in last 5s: %lu | calib: %d/%d | calibrated: %s",
                 (unsigned long)(current - last), calib_count, CALIB_PACKETS,
                 calibrated ? "YES" : "NO");
        last = current;
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_sta();
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    csi_init();
    xTaskCreate(csi_debug_task, "csi_dbg", 2048, NULL, 3, NULL);
}