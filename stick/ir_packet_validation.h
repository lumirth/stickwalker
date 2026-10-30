#ifndef PW_STICK_IR_PACKET_VALIDATION_H
#define PW_STICK_IR_PACKET_VALIDATION_H

#include "protocol.h"
#include "records.h"
#include "support/ir_state.h"

/* A packet can be valid link activity without giving us the next turn. In
 * particular, an ignored maintenance command must leave reception running. */
enum {
  STICK_IR_DISCARD = -1,
  STICK_IR_LISTEN = 0,
  STICK_IR_SERVICE = 1
};

static int StickCompressedPageValid(const u8 *packed, unsigned length)
{
  unsigned input = 4;
  unsigned output = 0;
  unsigned bit;
  u8 flags;

  if (length < 5 || packed[1] != EEPROM_PAGE_BYTES)
    return 0;
  while (output < EEPROM_PAGE_BYTES) {
    if (input >= length) return 0;
    flags = packed[input++];
    for (bit = 0; bit < 8 && output < EEPROM_PAGE_BYTES; bit++) {
      if (flags & 0x80) {
        unsigned count;
        unsigned behind;
        if (input + 1 >= length) return 0;
        count = (packed[input] >> 4) + 3;
        behind = packed[input + 1] + 1;
        input += 2;
        if (behind > output || output + count > EEPROM_PAGE_BYTES) return 0;
        output += count;
      } else {
        if (input >= length) return 0;
        input++;
        output++;
      }
      flags <<= 1;
    }
  }
  return 1;
}

/* Validate everything a native handler will consume before it may write or
 * construct a reply in the shared packet buffer. Native completion semantics
 * stay in ir.c. The H8 RAM-write diagnostic has no ESP32 address equivalent. */
static int StickPacketDisposition(unsigned command, unsigned argument,
                                  const u8 *body, unsigned length,
                                  unsigned remaining, unsigned bulkPhase)
{
  if (length > IR_PAYLOAD_BYTES) return STICK_IR_DISCARD;
  switch (command) {
  case IR_CMD_RAM_WRITE:
    return STICK_IR_DISCARD;
  case IR_CMD_EEPROM_WRITE:
    return length >= 1 ? STICK_IR_SERVICE : STICK_IR_DISCARD;
  case IR_CMD_EEPROM_READ:
    return length == 3 && body[2] && body[2] <= IR_PAYLOAD_BYTES ?
        STICK_IR_SERVICE : STICK_IR_DISCARD;
  case IR_CMD_EEPROM_REPLY:
    return (bulkPhase == PEER_BULK_RECEIVE_ICON ||
            bulkPhase == PEER_BULK_RECEIVE_NAME ||
            bulkPhase == PEER_BULK_RECEIVE_RECORDS) && remaining &&
           length == (remaining > IR_PAYLOAD_BYTES ? IR_PAYLOAD_BYTES : remaining) ?
        STICK_IR_SERVICE : STICK_IR_DISCARD;
  case IR_CMD_PAGE_REPLY:
    return !length && (bulkPhase == PEER_BULK_SEND_ICON ||
                       bulkPhase == PEER_BULK_SEND_NAME ||
                       bulkPhase == PEER_BULK_SEND_RECORDS) ?
        STICK_IR_SERVICE : STICK_IR_DISCARD;
  case IR_CMD_PAGE_COMPRESSED_A:
  case IR_CMD_PAGE_COMPRESSED_B:
    return length == EEPROM_PAGE_BYTES || StickCompressedPageValid(body, length) ?
        STICK_IR_SERVICE : STICK_IR_DISCARD;
  case IR_CMD_PAGE_WRITE_A:
  case IR_CMD_PAGE_WRITE_B:
    return length ? STICK_IR_SERVICE : STICK_IR_DISCARD;
  case IR_CMD_PEER_STATUS_REQUEST:
  case IR_CMD_PEER_STATUS_REPLY:
  case IR_CMD_WALK_START_REQUEST:
  case IR_CMD_WALK_END_REQUEST:
  case IR_CMD_WALK_UPDATE_REQUEST:
  case IR_CMD_GIFT_REQUEST:
  case IR_CMD_EVENT_MAP_CHECK:
  case IR_CMD_EVENT_POKEMON_CHECK:
  case IR_CMD_EVENT_ITEM_CHECK:
  case IR_CMD_EVENT_COURSE_CHECK:
  case IR_CMD_EVENT_STAMP0_CHECK:
  case IR_CMD_EVENT_STAMP1_CHECK:
  case IR_CMD_EVENT_STAMP2_CHECK:
  case IR_CMD_EVENT_STAMP3_CHECK:
    return length == sizeof(DeviceStatus) ? STICK_IR_SERVICE : STICK_IR_DISCARD;
  case IR_CMD_PEER_INFO:
    return length == sizeof(PeerInfo) ? STICK_IR_SERVICE : STICK_IR_DISCARD;
  case IR_CMD_FACTORY_SETUP:
    return length == sizeof(FactoryData) ? STICK_IR_SERVICE : STICK_IR_DISCARD;
  case IR_CMD_MOTION_TEST_SETUP:
    return argument == 1 && length == sizeof(MotionThresholds) ?
        STICK_IR_SERVICE : STICK_IR_DISCARD;
  case IR_ACK:
  case IR_RESPONSE:
    return !length ? STICK_IR_SERVICE : STICK_IR_DISCARD;
  case IR_CMD_STATUS_REQUEST:
  case IR_CMD_PING_REQUEST:
  case IR_CMD_PEER_START:
  case IR_CMD_RESET_ALL:
  case IR_CMD_RESET_KEEP_STEPS:
  case IR_CMD_WALK_START_COMMIT:
  case IR_CMD_WALK_END_COMMIT:
  case IR_CMD_WALK_UPDATE_COMMIT:
  case IR_CMD_GIFT_COLLECTION_COMMIT:
  case IR_CMD_EVENT_MAP_DONE:
  case IR_CMD_EVENT_POKEMON_DONE:
  case IR_CMD_EVENT_ITEM_DONE:
  case IR_CMD_EVENT_COURSE_DONE:
  case IR_CMD_EVENT_STAMP_MAP_DONE:
  case IR_CMD_EVENT_STAMP_POKEMON_DONE:
  case IR_CMD_EVENT_STAMP_ITEM_DONE:
  case IR_CMD_EVENT_STAMP_COURSE_DONE:
  case IR_CMD_EVENT_STAMP0:
  case IR_CMD_EVENT_STAMP1:
  case IR_CMD_EVENT_STAMP2:
  case IR_CMD_EVENT_STAMP3:
  case IR_CMD_EVENT_ALREADY_RECEIVED:
  case IR_CMD_EVENT_BUFFER_FULL:
    return STICK_IR_SERVICE;
  case IR_SHUTDOWN:
  case IR_CMD_PEER_ALREADY_RECORDED:
  case IR_CMD_WALK_START_REJECTED:
  case IR_CMD_WALK_END_REJECTED:
  case IR_CMD_WALK_UPDATE_REJECTED:
  case IR_CMD_GIFT_REJECTED:
  case IR_CMD_EVENT_REJECTED:
    return STICK_IR_LISTEN;
  default:
    /* Match native ignored-command activity, including header-only 0x2E. */
    return STICK_IR_LISTEN;
  }
}
#endif
