#include "diagnostics.h"

#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include <string.h>

// Change MAGIC whenever the RTC layout below changes.
enum { RESETS = 4, MAGIC = 0x53574401 };

typedef struct {
    uint8_t reason;
    uint8_t activity;
    uint16_t reserved;
    uint32_t uptime_s;
} Reset;

typedef struct {
    uint8_t result;
    uint8_t phase;
    uint8_t last_command;
    uint8_t reserved;
    uint16_t packets;
    uint16_t damaged;
} Link;

#pragma pack(push, 1)
typedef struct {
    uint8_t version;
    uint8_t resets;
    Reset recent[RESETS];
    Link link;
} Record;
#pragma pack(pop)

// RTC memory survives a reset but not a power cut.
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    uint32_t uptime_s;
    uint8_t activity;
    Link link;
} crumb;
static Record record = {.version = 1};

static void store(void)
{
    nvs_handle_t nvs;
    if (nvs_open("stick", NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_blob(nvs, "diagnostics", &record, sizeof(record));
    nvs_commit(nvs);
    nvs_close(nvs);
}

void diagnostics_init(void)
{
    nvs_handle_t nvs;
    if (nvs_open("stick", NVS_READONLY, &nvs) == ESP_OK) {
        Record saved;
        size_t size = sizeof(saved);
        if (nvs_get_blob(nvs, "diagnostics", &saved, &size) == ESP_OK && size == sizeof(saved) && saved.version == 1) {
            record = saved;
        }
        nvs_close(nvs);
    }
    esp_reset_reason_t reason = esp_reset_reason();
    bool abnormal = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT ||
                    reason == ESP_RST_WDT || reason == ESP_RST_BROWNOUT;
    if (abnormal) {
        memmove(&record.recent[1], &record.recent[0], sizeof(Reset) * (RESETS - 1));
        record.recent[0] = (Reset){.reason = (uint8_t)reason,
                                   .activity = crumb.magic == MAGIC ? crumb.activity : 0,
                                   .uptime_s = crumb.magic == MAGIC ? crumb.uptime_s : 0};
        if (record.resets < UINT8_MAX) record.resets++;
        store();
    }
    if (crumb.magic == MAGIC) {
        record.link = crumb.link;
    } else {
        memset(&crumb.link, 0, sizeof(crumb.link));
    }
    crumb.magic = MAGIC;
    crumb.uptime_s = 0;
    crumb.activity = ACTIVITY_BOOT;
}

void diagnostics_mark(Activity activity)
{
    crumb.activity = (uint8_t)activity;
    crumb.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
}

void diagnostics_link(uint8_t result, uint8_t phase, uint8_t last_command, uint16_t packets, uint16_t damaged)
{
    // Kept in memory: a flash write here would delay the next connection.
    record.link = crumb.link = (Link){.result = result, .phase = phase, .last_command = last_command,
                                      .packets = packets, .damaged = damaged};
}

unsigned diagnostics_status(uint8_t *out)
{
    memcpy(out, &record, sizeof(record));
    return sizeof(record);
}
