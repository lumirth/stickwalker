#define _POSIX_C_SOURCE 200809L
#include "hgss_peer.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A pipe adapter around the source-derived HGSS engine. Only T advances time;
// delivering a frame must not add an artificial processing delay.
static uint64_t now;
static uint8_t incoming[140];
static size_t pending;

static uint64_t clock_us(void *context)
{
    (void)context;
    return now;
}

static int read_wire(void *context, uint8_t *bytes, size_t capacity)
{
    (void)context;
    assert(pending <= capacity);
    memcpy(bytes, incoming, pending);
    int count = (int)pending;
    pending = 0;
    return count;
}

static int write_wire(void *context, const uint8_t *bytes, size_t count)
{
    (void)context;
    printf("TX ");
    for (size_t i = 0; i < count; i++) printf("%02x", bytes[i]);
    puts("");
    return 0;
}

static size_t unhex(const char *text, uint8_t *out)
{
    size_t count = 0;
    while (text[0] && text[1] && text[0] != '\n') {
        char byte[3] = {text[0], text[1], 0};
        out[count++] = (uint8_t)strtoul(byte, NULL, 16);
        text += 2;
    }
    return count;
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    HgssIo io = {NULL, clock_us, read_wire, write_wire,
                 (uint32_t)strtoul(argv[2], NULL, 16), 123456789, atoi(argv[3]) != 0};
    assert(!hgss_peer_open(&io, argv[1]));
    char *line = NULL;
    size_t capacity = 0;
    bool ready = false;
    while (getline(&line, &capacity, stdin) > 0) {
        if (line[0] == 'P') {
            char *name = line + 2, *text = strchr(name, ' ');
            assert(text);
            *text++ = 0;
            uint8_t *bytes = malloc(strlen(text) / 2 + 1);
            size_t count = unhex(text, bytes);
            assert(!hgss_peer_payload(name, bytes, count));
            free(bytes);
        } else {
            if (!ready) {
                assert(!hgss_peer_prepare());
                ready = true;
            }
            if (line[0] == 'F') {
                assert(strlen(line + 2) / 2 <= sizeof(incoming));
                pending = unhex(line + 2, incoming);
            } else if (line[0] == 'T') {
                now += strtoull(line + 2, NULL, 10);
            } else {
                assert(line[0] == 'S');
            }
            assert(!hgss_peer_step());
        }
        HgssState state;
        hgss_peer_state(&state);
        printf("STATE %u %u %u %u %d %u\nEND\n", state.sequence, state.command,
               state.remaining, state.pages, state.error, state.connected);
        fflush(stdout);
    }
    hgss_peer_close();
    free(line);
    return 0;
}
