#ifndef PW_STICK_IR_PACKET_H
#define PW_STICK_IR_PACKET_H

#include "protocol.h"
#include "records.h"
#include "support/ir_state.h"

/* Admission is independent of optical decoding. A checksum-valid packet can
 * still be a message which leaves the peer's transmit turn open. */
enum {
  STICK_IR_REJECT = 0,
  STICK_IR_LISTEN = 1,
  STICK_IR_SERVICE = 2
};

static int StickCompressedPageValid(const u8 *body, unsigned length)
{
  unsigned input = 4;
  unsigned output = 0;
  if (length < 5 || body[1] != 128)
    return 0;
  while (output < 128) {
    unsigned flags;
    unsigned bit;
    if (input >= length)
      return 0;
    flags = body[input++];
    for (bit = 0; bit < 8 && output < 128; ++bit, flags <<= 1) {
      unsigned count = 1;
      if (flags & 0x80) {
        if (input + 1 >= length)
          return 0;
        count = (body[input] >> 4) + 3;
        if (body[input + 1] + 1u > output || output + count > 128)
          return 0;
        input += 2;
      } else {
        if (input >= length)
          return 0;
        ++input;
      }
      output += count;
    }
  }
  return 1;
}

/* Describe the action the native handler will actually take. In particular,
 * an established-link ACK, an unrelated page ACK, and HGSS's ignored 0x2E
 * keepalive do not grant a flash/storage turn. */
static unsigned StickPacketAction(unsigned command, unsigned argument,
                                  const u8 *body, unsigned length,
                                  unsigned phase, unsigned bulk,
                                  unsigned remaining)
{
  if (length > IR_PAYLOAD_BYTES)
    return STICK_IR_REJECT;
  switch (command) {
  case 0xFA:
    return !length && (argument == IR_CONSOLE_PARAMETER ||
                       argument == IR_WALKER_PARAMETER)
               ? STICK_IR_SERVICE : STICK_IR_REJECT;
  case 0xF8:
    if (length || argument != IR_WALKER_PARAMETER)
      return STICK_IR_REJECT;
    return phase < IR_PHASE_INITIATOR ? STICK_IR_SERVICE : STICK_IR_LISTEN;
  case 0x2E:
  case IR_CMD_PING_REPLY:
    return !length ? STICK_IR_LISTEN : STICK_IR_REJECT;
  case IR_CMD_PAGE_REPLY:
    if (length)
      return STICK_IR_REJECT;
    return bulk == PEER_BULK_SEND_ICON || bulk == PEER_BULK_SEND_NAME ||
                   bulk == PEER_BULK_SEND_RECORDS
               ? STICK_IR_SERVICE : STICK_IR_LISTEN;
  case IR_CMD_RAM_WRITE:
    /* The original H8 RAM address space does not exist on this target. */
    return STICK_IR_REJECT;
  case IR_CMD_EEPROM_WRITE:
    return length && (((argument << 8) | body[0]) + length - 1u <= 65536u)
               ? STICK_IR_SERVICE : STICK_IR_REJECT;
  case IR_CMD_EEPROM_READ:
    return length == 3 && body[2] && body[2] <= IR_PAYLOAD_BYTES &&
                   (((unsigned)body[0] << 8) | body[1]) + body[2] <= 65536u
               ? STICK_IR_SERVICE : STICK_IR_REJECT;
  case IR_CMD_EEPROM_REPLY:
    return (bulk == PEER_BULK_RECEIVE_ICON || bulk == PEER_BULK_RECEIVE_NAME ||
            bulk == PEER_BULK_RECEIVE_RECORDS) && remaining &&
                   length == (remaining > 128 ? 128 : remaining)
               ? STICK_IR_SERVICE : STICK_IR_REJECT;
  case IR_CMD_PAGE_WRITE_A:
  case IR_CMD_PAGE_WRITE_B:
    return length ? STICK_IR_SERVICE : STICK_IR_REJECT;
  case IR_CMD_PAGE_COMPRESSED_A:
  case IR_CMD_PAGE_COMPRESSED_B:
    return length == 128 || StickCompressedPageValid(body, length)
               ? STICK_IR_SERVICE : STICK_IR_REJECT;
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
    return length == sizeof(DeviceStatus) ? STICK_IR_SERVICE : STICK_IR_REJECT;
  case IR_CMD_PEER_INFO:
    return length == sizeof(PeerInfo) ? STICK_IR_SERVICE : STICK_IR_REJECT;
  case IR_CMD_FACTORY_SETUP:
    return length == sizeof(FactoryData) ? STICK_IR_SERVICE : STICK_IR_REJECT;
  case IR_CMD_MOTION_TEST_SETUP:
    return argument == 1 && length == sizeof(MotionThresholds)
               ? STICK_IR_SERVICE : STICK_IR_REJECT;
  case IR_CMD_EVENT_ALREADY_RECEIVED:
  case IR_CMD_EVENT_BUFFER_FULL:
    return !length || length == sizeof(((DeviceStatus *)0)->receivedEvents) + 1
               ? STICK_IR_SERVICE : STICK_IR_REJECT;
  case IR_CMD_GIFT_REJECTED:
    /* HGSS includes its 0x68-byte identity in this rejection; its other
     * rejection responses are empty. The native handler consumes neither. */
    return !length || length == sizeof(DeviceStatus)
               ? STICK_IR_SERVICE : STICK_IR_REJECT;
  case IR_CMD_RESET_ALL:
  case IR_CMD_RESET_KEEP_STEPS:
    /* HGSS includes its forty-byte clock/identity block in erase requests. */
    return !length || length == DEVICE_ID_BYTES
               ? STICK_IR_SERVICE : STICK_IR_REJECT;
  case 0xF4:
  case IR_CMD_PEER_START:
  case IR_CMD_PEER_ALREADY_RECORDED:
  case IR_CMD_STATUS_REQUEST:
  case IR_CMD_PING_REQUEST:
  case IR_CMD_WALK_START_REJECTED:
  case IR_CMD_WALK_START_COMMIT:
  case IR_CMD_WALK_END_REJECTED:
  case IR_CMD_WALK_END_COMMIT:
  case IR_CMD_WALK_UPDATE_REJECTED:
  case IR_CMD_WALK_UPDATE_COMMIT:
  case IR_CMD_GIFT_COLLECTION_COMMIT:
  case IR_CMD_EVENT_STAMP0:
  case IR_CMD_EVENT_STAMP1:
  case IR_CMD_EVENT_STAMP2:
  case IR_CMD_EVENT_STAMP3:
  case IR_CMD_EVENT_MAP_DONE:
  case IR_CMD_EVENT_POKEMON_DONE:
  case IR_CMD_EVENT_ITEM_DONE:
  case IR_CMD_EVENT_COURSE_DONE:
  case IR_CMD_EVENT_STAMP_MAP_DONE:
  case IR_CMD_EVENT_STAMP_POKEMON_DONE:
  case IR_CMD_EVENT_STAMP_ITEM_DONE:
  case IR_CMD_EVENT_STAMP_COURSE_DONE:
  case IR_CMD_EVENT_REJECTED:
    return !length ? STICK_IR_SERVICE : STICK_IR_REJECT;
  default:
    return STICK_IR_REJECT;
  }
}

#endif
