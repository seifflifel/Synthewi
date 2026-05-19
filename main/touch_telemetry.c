#include "touch_telemetry.h"

#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_mac.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "touch_control.h"
#include "wifi_manager.h"

static const char *TAG = "touch_telemetry";

#define TOUCH_TELEM_PORT 4210
#define TOUCH_CMD_PORT 4211
#define TOUCH_TELEM_HZ 30
#define TOUCH_TELEM_PERIOD_MS (1000 / TOUCH_TELEM_HZ)
#define TOUCH_TELEM_MAGIC "SYNT"
#define TOUCH_CMD_MAGIC "SYNC"
#define TOUCH_PROTO_VERSION 1
#define TOUCH_CMD_SET_THRESHOLD 1
#define TOUCH_CHANNEL_COUNT 4

typedef struct __attribute__((packed)) {
    uint16_t raw;
    uint16_t baseline;
    uint16_t threshold;
    uint8_t is_touching;
    uint8_t intensity;
} touch_telemetry_channel_t;

typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint8_t version;
    uint8_t flags;
    uint16_t seq;
    uint8_t mac[6];
    uint8_t channel_count;
    uint8_t reserved;
    touch_telemetry_channel_t ch[TOUCH_CHANNEL_COUNT];
} touch_telemetry_packet_t;

typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint8_t version;
    uint8_t cmd;
    uint8_t channel;
    uint8_t reserved;
    uint16_t threshold;
} touch_cmd_packet_t;

static touch_sensor_t s_touch_pads[TOUCH_CHANNEL_COUNT];
static volatile uint16_t s_thresholds[TOUCH_CHANNEL_COUNT] = {100, 100, 100, 100};
static const uint8_t s_touch_channels[TOUCH_CHANNEL_COUNT] = {4, 5, 6, 7};
static uint8_t s_sta_mac[6] = {0};
static bool s_started = false;

static uint16_t abs_delta_u16(const touch_sensor_t *sensor)
{
    int32_t d = (int32_t)sensor->raw_value - (int32_t)sensor->baseline_value;
    if (d < 0) {
        d = -d;
    }
    return (uint16_t)d;
}

static uint8_t intensity_from_delta(uint16_t abs_delta, uint16_t threshold)
{
    if (threshold == 0) {
        return 0;
    }
    uint32_t scaled = ((uint32_t)abs_delta * 100U) / (uint32_t)threshold;
    if (scaled > 100U) {
        scaled = 100U;
    }
    return (uint8_t)scaled;
}

static int open_telem_socket(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Telemetry socket open failed: errno=%d", errno);
        return -1;
    }

    int enable = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable)) < 0) {
        ESP_LOGW(TAG, "Failed to enable broadcast: errno=%d", errno);
    }

    return sock;
}

static int open_cmd_socket(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Command socket open failed: errno=%d", errno);
        return -1;
    }

    int enable = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0) {
        ESP_LOGW(TAG, "Failed to set reuseaddr: errno=%d", errno);
    }

    struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = 200000,
    };
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        ESP_LOGW(TAG, "Failed to set recv timeout: errno=%d", errno);
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TOUCH_CMD_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Command socket bind failed: errno=%d", errno);
        close(sock);
        return -1;
    }

    return sock;
}

static void touch_telemetry_task(void *arg)
{
    (void)arg;
    int sock = -1;
    uint16_t seq = 0;
    TickType_t last_wake = xTaskGetTickCount();
    TickType_t period = pdMS_TO_TICKS(TOUCH_TELEM_PERIOD_MS);

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(TOUCH_TELEM_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };

    while (1) {
        if (sock < 0) {
            sock = open_telem_socket();
            if (sock < 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        touch_telemetry_packet_t pkt = {0};
        memcpy(pkt.magic, TOUCH_TELEM_MAGIC, sizeof(pkt.magic));
        pkt.version = TOUCH_PROTO_VERSION;
        pkt.flags = 0;
        pkt.seq = seq++;
        memcpy(pkt.mac, s_sta_mac, sizeof(pkt.mac));
        pkt.channel_count = TOUCH_CHANNEL_COUNT;

        for (int i = 0; i < TOUCH_CHANNEL_COUNT; i++) {
            touch_sensor_sample(&s_touch_pads[i]);
            uint16_t abs_delta = abs_delta_u16(&s_touch_pads[i]);
            uint16_t threshold = s_thresholds[i];
            bool touching = abs_delta >= threshold;
            uint8_t intensity = intensity_from_delta(abs_delta, threshold);

            pkt.ch[i].raw = s_touch_pads[i].raw_value;
            pkt.ch[i].baseline = s_touch_pads[i].baseline_value;
            pkt.ch[i].threshold = threshold;
            pkt.ch[i].is_touching = touching ? 1 : 0;
            pkt.ch[i].intensity = intensity;
        }

        if (wifi_manager_is_connected()) {
            int sent = sendto(sock, &pkt, sizeof(pkt), 0, (struct sockaddr *)&dest, sizeof(dest));
            if (sent < 0) {
                ESP_LOGW(TAG, "Telemetry send failed: errno=%d", errno);
            }
        }

        vTaskDelayUntil(&last_wake, period);
    }
}

static void touch_cmd_task(void *arg)
{
    (void)arg;
    int sock = -1;
    uint8_t rx_buf[64];

    while (1) {
        if (sock < 0) {
            sock = open_cmd_socket();
            if (sock < 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        struct sockaddr_in source = {0};
        socklen_t slen = sizeof(source);
        int len = recvfrom(sock, rx_buf, sizeof(rx_buf), 0, (struct sockaddr *)&source, &slen);
        if (len < 0) {
            continue;
        }
        if (len < (int)sizeof(touch_cmd_packet_t)) {
            continue;
        }

        const touch_cmd_packet_t *cmd = (const touch_cmd_packet_t *)rx_buf;
        if (memcmp(cmd->magic, TOUCH_CMD_MAGIC, sizeof(cmd->magic)) != 0) {
            continue;
        }
        if (cmd->version != TOUCH_PROTO_VERSION) {
            continue;
        }
        if (cmd->cmd != TOUCH_CMD_SET_THRESHOLD) {
            continue;
        }
        if (cmd->channel >= TOUCH_CHANNEL_COUNT) {
            continue;
        }
        if (cmd->threshold == 0) {
            ESP_LOGW(TAG, "Ignoring zero threshold on ch%u", cmd->channel);
            continue;
        }

        uint16_t prev = s_thresholds[cmd->channel];
        s_thresholds[cmd->channel] = cmd->threshold;
        s_touch_pads[cmd->channel].threshold = cmd->threshold;

        if (prev != cmd->threshold) {
            ESP_LOGI(TAG, "Threshold update: ch%u %u -> %u", cmd->channel, prev, cmd->threshold);
        }
    }
}

esp_err_t touch_telemetry_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    (void)esp_read_mac(s_sta_mac, ESP_MAC_WIFI_STA);

    for (int i = 0; i < TOUCH_CHANNEL_COUNT; i++) {
        touch_sensor_init(&s_touch_pads[i], s_touch_channels[i], s_thresholds[i]);
        if (s_touch_pads[i].chan_handle == NULL) {
            ESP_LOGE(TAG, "Touch init failed for channel %u", s_touch_channels[i]);
            return ESP_FAIL;
        }
    }

    if (xTaskCreate(touch_telemetry_task, "touch_telemetry", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create touch_telemetry task");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(touch_cmd_task, "touch_cmd", 2048, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start touch command task");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "Touch telemetry started (port=%d cmd_port=%d)", TOUCH_TELEM_PORT, TOUCH_CMD_PORT);

    return ESP_OK;
}
