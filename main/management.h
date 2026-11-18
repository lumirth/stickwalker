#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pokewalker_host.h"

enum {
    MANAGE_STATUS = 1,
    MANAGE_SETTINGS,
    MANAGE_READ,
    MANAGE_WRITE,
    MANAGE_PREPARE,
    MANAGE_IDENTIFY,
};
enum { MANAGE_OK, MANAGE_BUSY, MANAGE_INVALID, MANAGE_FAILED };
enum { MANAGE_PAYLOAD = 4096, MANAGE_PALETTE_ONLY = 0x80 };

#pragma pack(push, 1)
typedef struct {
    uint8_t palette;
    uint8_t reverse;
    uint8_t layout;
    uint8_t mappings[4];
    uint16_t colors[6][4];
    char names[6][16];
} ManagementSettings;

typedef struct {
    char version[32];
    uint8_t build[32];
    uint8_t mac[6];
    uint8_t state;
    uint16_t battery_mv;
    uint8_t external_power;
    ManagementSettings settings;
} ManagementStatus;

typedef struct {
    uint32_t offset;
    uint16_t length;
} ManagementRead;
#pragma pack(pop)

_Static_assert(sizeof(ManagementStatus) == 225, "Management status wire layout");

void management_init(void);
void management_poll(void);
unsigned management_request(unsigned op, const uint8_t *in, size_t count,
                            uint8_t *out, size_t *length);
