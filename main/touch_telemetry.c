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
#include "amy_engine.h"

static const char *TAG = "touch_telemetry";

// ---------------------------------------------------------------------------
// Ports and protocol constants
// ---------------------------------------------------------------------------
#define TOUCH_TELEM_PORT     4210
#define TOUCH_CMD_PORT       4211
#define TOUCH_TELEM_HZ       30
#define TOUCH_TELEM_PERIOD_MS (1000 / TOUCH_TELEM_HZ)
#define TOUCH_TELEM_MAGIC    "SYNT"
#define TOUCH_CMD_MAGIC      "SYNC"
#define TOUCH_PROTO_VERSION  3   // bumped: v3 extends synth state to 28 bytes
#define TOUCH_CHANNEL_COUNT  8

// Command types
#define TOUCH_CMD_SET_THRESHOLD  1
#define TOUCH_CMD_SET_SYNTH_PARAM 2  // channel=param_id, threshold=value (0-10000)

// ---------------------------------------------------------------------------
// Packet definitions
// ---------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
    uint16_t raw;
    uint16_t baseline;
    uint16_t threshold;
    uint8_t  is_touching;
    uint8_t  intensity;
} touch_telemetry_channel_t;

// v3 synth state appended after channel data (28 bytes)
typedef struct __attribute__((packed)) {
    uint8_t  wave_id;
    uint8_t  filter_type;     // AMY_ENGINE_FILTER_LPF/BPF/HPF (was reserved in v2)
    uint16_t reverb_amount;
    uint16_t reverb_decay;
    uint16_t echo_amount;
    uint16_t echo_feedback;
    uint16_t filter_cutoff;
    uint16_t filter_resonance;
    uint16_t env_attack;
    uint16_t env_release;
    uint16_t filter_env_depth;
    uint16_t filter_env_decay;
    uint16_t lfo_rate;
    uint16_t lfo_depth;
    uint16_t chorus_amount;
} touch_telemetry_synth_t;

typedef struct __attribute__((packed)) {
    uint8_t                   magic[4];
    uint8_t                   version;
    uint8_t                   flags;
    uint16_t                  seq;
    uint8_t                   mac[6];
    uint8_t                   channel_count;
    uint8_t                   reserved;
    touch_telemetry_channel_t ch[TOUCH_CHANNEL_COUNT];
    touch_telemetry_synth_t   synth; // v2 addition
} touch_telemetry_packet_t;

typedef struct __attribute__((packed)) {
    uint8_t  magic[4];
    uint8_t  version;
    uint8_t  cmd;
    uint8_t  channel;   // threshold cmd: pad index; synth param cmd: param_id
    uint8_t  reserved;
    uint16_t threshold; // threshold cmd: threshold value; synth param cmd: 0-10000 value
} touch_cmd_packet_t;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static touch_sensor_t   s_touch_pads[TOUCH_CHANNEL_COUNT];
static volatile uint16_t s_thresholds[TOUCH_CHANNEL_COUNT] = {100, 100, 100, 100, 100, 100, 100, 100};
static const uint8_t    s_touch_channels[TOUCH_CHANNEL_COUNT] = {4, 5, 6, 7, 8, 12, 1, 2}; // ch0-7 mapped to these GPIOs (ESP32 touch channel numbers, not pin numbers)
static uint8_t          s_sta_mac[6] = {0};
static bool             s_started = false;
static bool             s_prev_touching[TOUCH_CHANNEL_COUNT] = {false};

static volatile touch_event_cb_t    s_event_cb    = NULL;
static volatile touch_param_cb_t    s_param_cb    = NULL;
static volatile touch_pressure_cb_t s_pressure_cb = NULL;
static volatile uint16_t            s_pressure_range = 200; // 2.0× default

// ---------------------------------------------------------------------------
// Public callback registration
// ---------------------------------------------------------------------------
void touch_telemetry_set_event_cb(touch_event_cb_t cb)        { s_event_cb    = cb; }
void touch_telemetry_set_param_cb(touch_param_cb_t cb)        { s_param_cb    = cb; }
void touch_telemetry_set_pressure_cb(touch_pressure_cb_t cb)  { s_pressure_cb = cb; }
void touch_telemetry_set_pressure_range(uint16_t range_x100)  { s_pressure_range = range_x100 ? range_x100 : 100; }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static uint16_t abs_delta_u16(const touch_sensor_t *s)
{
    int32_t d = (int32_t)s->raw_value - (int32_t)s->baseline_value;
    return (uint16_t)(d < 0 ? -d : d);
}

static uint8_t intensity_from_delta(uint16_t delta, uint16_t threshold)
{
    if (!threshold) return 0;
    uint32_t sc = ((uint32_t)delta * 100U) / threshold;
    return (uint8_t)(sc > 100U ? 100U : sc);
}

static int open_telem_socket(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) { ESP_LOGE(TAG, "telem socket: errno=%d", errno); return -1; }
    int en = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &en, sizeof(en));
    return sock;
}

static int open_cmd_socket(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) { ESP_LOGE(TAG, "cmd socket: errno=%d", errno); return -1; }
    int en = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &en, sizeof(en));
    struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(TOUCH_CMD_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "cmd bind: errno=%d", errno);
        close(sock);
        return -1;
    }
    return sock;
}

// ---------------------------------------------------------------------------
// Telemetry task — reads touch, fires edge callbacks, broadcasts UDP
// ---------------------------------------------------------------------------
static void touch_telemetry_task(void *arg)
{
    (void)arg;
    int      sock = -1;
    uint16_t seq  = 0;
    TickType_t last_wake = xTaskGetTickCount();
    TickType_t period    = pdMS_TO_TICKS(TOUCH_TELEM_PERIOD_MS);

    struct sockaddr_in dest = {
        .sin_family      = AF_INET,
        .sin_port        = htons(TOUCH_TELEM_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };

    while (1) {
        if (sock < 0) {
            sock = open_telem_socket();
            if (sock < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        }

        touch_telemetry_packet_t pkt = {0};
        memcpy(pkt.magic, TOUCH_TELEM_MAGIC, 4);
        pkt.version       = TOUCH_PROTO_VERSION;
        pkt.seq           = seq++;
        memcpy(pkt.mac, s_sta_mac, 6);
        pkt.channel_count = TOUCH_CHANNEL_COUNT;

        for (int i = 0; i < TOUCH_CHANNEL_COUNT; i++) {
            touch_sensor_sample(&s_touch_pads[i]);
            uint16_t delta     = abs_delta_u16(&s_touch_pads[i]);
            uint16_t threshold = s_thresholds[i];
            bool     touching  = (delta >= threshold);
            uint8_t  intensity = intensity_from_delta(delta, threshold);

            pkt.ch[i].raw        = s_touch_pads[i].raw_value;
            pkt.ch[i].baseline   = s_touch_pads[i].baseline_value;
            pkt.ch[i].threshold  = threshold;
            pkt.ch[i].is_touching = touching ? 1 : 0;
            pkt.ch[i].intensity  = intensity;

            // Fire edge callbacks for note on/off
            if (touching != s_prev_touching[i]) {
                s_prev_touching[i] = touching;
                touch_event_cb_t cb = s_event_cb;
                if (cb) cb((uint8_t)i, touching);
            }

            // Continuous pressure update while pad is held (30 Hz)
            if (touching) {
                touch_pressure_cb_t pcb = s_pressure_cb;
                if (pcb) {
                    uint16_t range = s_pressure_range;
                    float ceiling  = (float)threshold * (range / 100.0f);
                    float excess   = (float)delta - (float)threshold;
                    float norm     = (ceiling > 0.0f) ? (excess / ceiling) : 0.0f;
                    if (norm < 0.0f) norm = 0.0f;
                    if (norm > 1.0f) norm = 1.0f;
                    pcb((uint8_t)i, norm);
                }
            }
        }

        // Append current synth state (v3)
        amy_engine_state_t st;
        amy_engine_get_state(&st);
        pkt.synth.wave_id          = st.wave_id;
        pkt.synth.filter_type      = st.filter_type;
        pkt.synth.reverb_amount    = st.reverb_amount;
        pkt.synth.reverb_decay     = st.reverb_decay;
        pkt.synth.echo_amount      = st.echo_amount;
        pkt.synth.echo_feedback    = st.echo_feedback;
        pkt.synth.filter_cutoff    = st.filter_cutoff;
        pkt.synth.filter_resonance = st.filter_resonance;
        pkt.synth.env_attack       = st.env_attack;
        pkt.synth.env_release      = st.env_release;
        pkt.synth.filter_env_depth = st.filter_env_depth;
        pkt.synth.filter_env_decay = st.filter_env_decay;
        pkt.synth.lfo_rate         = st.lfo_rate;
        pkt.synth.lfo_depth        = st.lfo_depth;
        pkt.synth.chorus_amount    = st.chorus_amount;

        if (wifi_manager_is_connected()) {
            int sent = sendto(sock, &pkt, sizeof(pkt), 0, (struct sockaddr *)&dest, sizeof(dest));
            if (sent < 0) ESP_LOGW(TAG, "telem send: errno=%d", errno);
        }

        vTaskDelayUntil(&last_wake, period);
    }
}

// ---------------------------------------------------------------------------
// Command task — receives threshold and synth param commands from bridge
// ---------------------------------------------------------------------------
static void touch_cmd_task(void *arg)
{
    (void)arg;
    int     sock = -1;
    uint8_t rx_buf[64];

    while (1) {
        if (sock < 0) {
            sock = open_cmd_socket();
            if (sock < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        }

        struct sockaddr_in src = {0};
        socklen_t slen = sizeof(src);
        int len = recvfrom(sock, rx_buf, sizeof(rx_buf), 0, (struct sockaddr *)&src, &slen);
        if (len < (int)sizeof(touch_cmd_packet_t)) continue;

        const touch_cmd_packet_t *cmd = (const touch_cmd_packet_t *)rx_buf;
        if (memcmp(cmd->magic, TOUCH_CMD_MAGIC, 4) != 0) continue;
        if (cmd->version == 0) continue; // accept any non-zero version (command format is stable)

        if (cmd->cmd == TOUCH_CMD_SET_THRESHOLD) {
            if (cmd->channel >= TOUCH_CHANNEL_COUNT || cmd->threshold == 0) continue;
            uint16_t prev = s_thresholds[cmd->channel];
            s_thresholds[cmd->channel] = cmd->threshold;
            s_touch_pads[cmd->channel].threshold = cmd->threshold;
            if (prev != cmd->threshold)
                ESP_LOGI(TAG, "threshold ch%u: %u → %u", cmd->channel, prev, cmd->threshold);

        } else if (cmd->cmd == TOUCH_CMD_SET_SYNTH_PARAM) {
            touch_param_cb_t cb = s_param_cb;
            if (cb) cb(cmd->channel, cmd->threshold); // channel = param_id, threshold = value
            ESP_LOGI(TAG, "synth param %u = %u", cmd->channel, cmd->threshold);
        }
    }
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------
esp_err_t touch_telemetry_start(void)
{
    if (s_started) return ESP_OK;

    esp_read_mac(s_sta_mac, ESP_MAC_WIFI_STA);

    for (int i = 0; i < TOUCH_CHANNEL_COUNT; i++) {
        touch_sensor_init(&s_touch_pads[i], s_touch_channels[i], s_thresholds[i]);
        if (!s_touch_pads[i].chan_handle) {
            ESP_LOGE(TAG, "touch init failed ch%u", s_touch_channels[i]);
            return ESP_FAIL;
        }
    }

    // 6 KB: first iteration calls socket()+touch_sensor_enable()+sendto() in the same stack frame.
    // ESP-IDF socket/lwIP calls alone can consume 2-3 KB; too small a stack overflows into
    // adjacent heap blocks that contain the touch channel objects, corrupting the touch ISR.
    if (xTaskCreate(touch_telemetry_task, "touch_telem", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create telemetry task");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(touch_cmd_task, "touch_cmd", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create cmd task");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "started (telem_port=%d cmd_port=%d proto_v=%d)",
             TOUCH_TELEM_PORT, TOUCH_CMD_PORT, TOUCH_PROTO_VERSION);
    return ESP_OK;
}
