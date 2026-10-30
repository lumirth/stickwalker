#ifndef PW_STICK_IR_PACKET_VALIDATION_H
#define PW_STICK_IR_PACKET_VALIDATION_H

#include "protocol.h"
#include "records.h"

static int StickPacketCommandKnown(unsigned command)
{
  switch (command) {
  case IR_ACK:
  case IR_CMD_EEPROM_READ:
  case IR_CMD_EEPROM_REPLY:
  case IR_CMD_EEPROM_WRITE:
  case IR_CMD_EVENT_ALREADY_RECEIVED:
  case IR_CMD_EVENT_BUFFER_FULL:
  case IR_CMD_EVENT_COURSE_CHECK:
  case IR_CMD_EVENT_COURSE_DONE:
  case IR_CMD_EVENT_ITEM_CHECK:
  case IR_CMD_EVENT_ITEM_DONE:
  case IR_CMD_EVENT_MAP_CHECK:
  case IR_CMD_EVENT_MAP_DONE:
  case IR_CMD_EVENT_POKEMON_CHECK:
  case IR_CMD_EVENT_POKEMON_DONE:
  case IR_CMD_EVENT_REJECTED:
  case IR_CMD_EVENT_STAMP0:
  case IR_CMD_EVENT_STAMP0_CHECK:
  case IR_CMD_EVENT_STAMP1:
  case IR_CMD_EVENT_STAMP1_CHECK:
  case IR_CMD_EVENT_STAMP2:
  case IR_CMD_EVENT_STAMP2_CHECK:
  case IR_CMD_EVENT_STAMP3:
  case IR_CMD_EVENT_STAMP3_CHECK:
  case IR_CMD_EVENT_STAMP_COURSE_DONE:
  case IR_CMD_EVENT_STAMP_ITEM_DONE:
  case IR_CMD_EVENT_STAMP_MAP_DONE:
  case IR_CMD_EVENT_STAMP_POKEMON_DONE:
  case IR_CMD_FACTORY_SETUP:
  case IR_CMD_GIFT_COLLECTION_COMMIT:
  case IR_CMD_GIFT_REJECTED:
  case IR_CMD_GIFT_REQUEST:
  case IR_CMD_MOTION_TEST_SETUP:
  case IR_CMD_PAGE_COMPRESSED_A:
  case IR_CMD_PAGE_COMPRESSED_B:
  case IR_CMD_PAGE_REPLY:
  case IR_CMD_PAGE_WRITE_A:
  case IR_CMD_PAGE_WRITE_B:
  case IR_CMD_PEER_ALREADY_RECORDED:
  case IR_CMD_PEER_INFO:
  case IR_CMD_PEER_START:
  case IR_CMD_PEER_STATUS_REPLY:
  case IR_CMD_PEER_STATUS_REQUEST:
  case IR_CMD_PING_REQUEST:
  case IR_CMD_RAM_WRITE:
  case IR_CMD_RESET_ALL:
  case IR_CMD_RESET_KEEP_STEPS:
  case IR_CMD_STATUS_REQUEST:
  case IR_CMD_WALK_END_COMMIT:
  case IR_CMD_WALK_END_REJECTED:
  case IR_CMD_WALK_END_REQUEST:
  case IR_CMD_WALK_START_COMMIT:
  case IR_CMD_WALK_START_REJECTED:
  case IR_CMD_WALK_START_REQUEST:
  case IR_CMD_WALK_UPDATE_COMMIT:
  case IR_CMD_WALK_UPDATE_REJECTED:
  case IR_CMD_WALK_UPDATE_REQUEST:
  case IR_RESPONSE:
  case IR_SHUTDOWN:
    return 1;
  default:
    return 0;
  }
}

static int StickPacketBodyValid(unsigned command, const u8 *body,
                                unsigned length, unsigned remaining)
{
  if (length > IR_PAYLOAD_BYTES) return 0;
  switch (command) {
  case IR_CMD_RAM_WRITE:
    /* H8 RAM addresses have no meaning in the ESP32 address space. */
    return 0;
  case IR_CMD_EEPROM_WRITE:
    return length >= 1;
  case IR_CMD_EEPROM_READ:
    return length == 3 && body[2] && body[2] <= IR_PAYLOAD_BYTES;
  case IR_CMD_EEPROM_REPLY:
    return remaining && length == (remaining > 128 ? 128 : remaining);
  case IR_CMD_PAGE_WRITE_A:
  case IR_CMD_PAGE_WRITE_B:
  case IR_CMD_PAGE_COMPRESSED_A:
  case IR_CMD_PAGE_COMPRESSED_B:
    return length != 0;
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
    return length == sizeof(DeviceStatus);
  case IR_CMD_PEER_INFO:
    return length == sizeof(PeerInfo);
  case IR_CMD_FACTORY_SETUP:
    return length == sizeof(FactoryData);
  default:
    return 1;
  }
}
#endif
