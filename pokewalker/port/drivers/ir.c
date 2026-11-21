#include "port_internal.h"
#include "flags.h"
#include "types.h"
#include "eeprom_address.h"
#include "application/pw_nt7508.h"
#include "application/pw_eeprom_m95512.h"
#include "support/ir.h"
#include "application/pw_friend.h"
#include "application/pw_storage.h"
#include "support/lib_common.h"
#include "startup/h8_resetprg.h"
#include "pokewalker_host.h"
#include "pokewalker.h"
#include "record_io.h"
#include <string.h>
#include "project.h"

/* Return the sum of big-endian byte pairs, seeded with 2, after folding the
 * carry twice. The frame builder and checker clear the header checksum bytes
 * before calling; SendPacket writes the result low byte first. */
uint PacketChecksum(u8 *bytes, u32 length)
{
  u32 checksum = 2;
  u32 index = 0;

  while (index < length) {
    u8 value = *bytes++;

    if (index & 1) {
      checksum += value;
    } else {
      checksum += (uint)value << 8;
    }
    ++index;
  }

  checksum = (checksum >> 16) + (u16)checksum;
  checksum += checksum >> 16;

  return (u16)checksum;
}

typedef enum { IR_BACKOFF, IR_LISTEN, IR_BURST } IrTurn;
static IrTurn ir_turn;
static uint64_t ir_turn_deadline;
static bool ir_burst_corrupt;
static bool ir_completion_sent;
static uint8_t link_last_command;
static uint16_t link_packets, link_damaged;
static uint64_t ir_probe_deadline;
static uint64_t ir_transmit_ended;
static uint64_t ir_reference_us;
static uint64_t ir_observed_us;
static PwInfraredEvent ir_event;
static bool ir_has_byte;
static bool ir_failed;

static u16 ir_ticks_at(uint64_t us)
{
  return (u16)((us * 32768) / 1000000);
}

/* Initialize both EEPROM cursors and reset transfer progress at the start of
 * each peer phase. */
static void BeginBulkPhase(u8 phase, uint sourceAddress,
                           uint destinationAddress, uint byteCount)
{
  g_work.irc.work.bulkPhase = phase;
  g_work.irc.work.bulkSourceEepromAddress = sourceAddress;
  g_work.irc.work.bulkDestinationEepromAddress = destinationAddress;
  g_work.irc.work.bulkBytesRemaining = byteCount;
  g_work.irc.work.bulkChunksCompleted = 0;
}

/* Motion, sound and infrared communication reuse this workspace. */
Workspace g_work;

u8 *IrPayload(void)
{
  return (g_work.irc.packet + IR_HEADER_BYTES);
}

void IrTransmitByte(u8 value);

/* The eight-byte header contains a command, its parameter, a little-endian
 * checksum and four session-token bytes. Payload follows immediately. */
typedef struct {
  u8 command;
  u8 argument;
  u8 checksumLo;
  u8 checksumHi;
  u8 sessionToken[4];
} IrcHeader;

typedef struct {
  IrcHeader header;
  u8 payload[IR_PAYLOAD_BYTES];
} IrcPacket;

_Static_assert(sizeof(IrcHeader) == IR_HEADER_BYTES, "Wire header size");
_Static_assert(offsetof(IrcPacket, payload) == IR_HEADER_BYTES, "Wire payload offset");
_Static_assert(sizeof(IrcPacket) == IR_HEADER_BYTES + IR_PAYLOAD_BYTES,
               "Maximum wire frame size");

void SendPacket(u8 payloadLength, u8 command, u8 argument)
{
  IrcPacket packet;
  if (payloadLength > IR_PAYLOAD_BYTES) {
    ir_failed = true;
    return;
  }
  memcpy(packet.payload, IrPayload(), payloadLength);
  ir_turn = IR_LISTEN;
  packet.header = (IrcHeader){.command = command, .argument = argument};
  pw_write_be32(packet.header.sessionToken, g_work.irc.work.sessionToken);
  if (command == IR_CMD_STATUS_REPLY || command == IR_CMD_PEER_STATUS_REQUEST ||
      command == IR_CMD_PEER_STATUS_REPLY) {
    DeviceStatus status;
    memcpy(&status, packet.payload, sizeof(status));
    pw_status_swap(&status);
    memcpy(packet.payload, &status, sizeof(status));
  } else if (command == IR_CMD_PEER_INFO) {
    PeerInfo peer;
    memcpy(&peer, packet.payload, sizeof(peer));
    pw_peer_swap(&peer);
    memcpy(packet.payload, &peer, sizeof(peer));
  }
  u16 packet_length = sizeof(packet.header) + payloadLength;
  u16 checksum = PacketChecksum((u8 *)&packet, packet_length);
  packet.header.checksumLo = checksum;
  packet.header.checksumHi = checksum >> 8;
  ir_transmit_ended = pw_host_infrared_transmit((u8 *)&packet, packet_length);
  if (ir_transmit_ended == UINT64_MAX) {
    ir_failed = true;
  } else if (g_work.irc.work.completionAction != IR_ACTION_NONE ||
             command == IR_CMD_PEER_START) {
    /* The peer initiator sends its final packet before accepting the reply. */
    ir_completion_sent = true;
  }
}

void IrTransmitByte(u8 value)
{
  if (!ir_failed) {
    ir_transmit_ended = pw_host_infrared_transmit(&value, 1);
    ir_failed = ir_transmit_ended == UINT64_MAX;
  }
}

/* Establish native communication ownership before opening the physical service.
 */
void IrHardwareStart(void)
{
  ir_failed = false;
  ir_completion_sent = false;
  link_last_command = 0;
  link_packets = link_damaged = 0;
  ir_turn = IR_LISTEN;
  ir_has_byte = false;
  ir_observed_us = 0;
  if (!pw_host_communication(true)) {
    ir_failed = true;
    return;
  }
  if (!pw_host_infrared_open()) {
    pw_host_communication(false);
    ir_failed = true;
  }
}

#define IR_ACK 0xF8u
#define IR_RESPONSE 0xFAu
#define IR_CONNECT 0xFCu
#define IR_SHUTDOWN 0xF4u

#define PW_IR_RX_WINDOW_CAPACITY ((uint)(IR_HEADER_BYTES + IR_PAYLOAD_BYTES))
#define PW_IR_INACTIVITY_TICKS 0xc80
#define PW_IR_TIMEOUT_RETRY_LIMIT 0x14u
#define PW_IR_CHECKSUM_FAIL_LIMIT 0x14u
#define PW_IR_RETRY_JITTER_MASK 0x0Fu
#define PW_IR_RETRY_JITTER_TICKS 0x60u
#define PW_IR_FRAME_GAP_TICKS 4

#define PW_IR_BULK_MAXIMUM_CHUNK_LENGTH EEPROM_PAGE_BYTES

typedef char IrcReceiveStorageHoldsMaximumFrame
    [sizeof(g_work.irc.packet) >= PW_IR_RX_WINDOW_CAPACITY ? 1 : -1];
typedef char IrcDecodeStorageHoldsMaximumChunk
    [sizeof(g_work.irc.eepromScratch) >= PW_IR_BULK_MAXIMUM_CHUNK_LENGTH ? 1
                                                                         : -1];

static void ir_begin_turn(void)
{
  ir_reference_us = ir_transmit_ended && ir_transmit_ended != UINT64_MAX
      ? ir_transmit_ended : pw_host_now_us();
  ir_transmit_ended = 0;
  ir_turn_deadline = ir_reference_us +
      ((uint64_t)PW_IR_INACTIVITY_TICKS * 1000000 + 32767) / 32768;
  ir_turn = IR_LISTEN;
  ir_burst_corrupt = false;
  g_state.irTimerReference = ir_ticks_at(ir_reference_us);
  g_state.irReceivedBytes = 0;
}

/* Start probing with a token from the PRNG state, clear session progress,
 * and send the one-byte connection probe. */
void IrBegin(void)
{
  IrHardwareStart();
  g_state.irResult = IR_RESULT_NONE;
  g_work.irc.work.localHandshakeToken = g_state.randomState;
  g_work.irc.work.sessionToken = g_work.irc.work.localHandshakeToken;
  g_work.irc.work.handshakePhase = IR_PHASE_PROBING;
  g_work.irc.work.timeoutRetryCount = 0;
  g_work.irc.work.checksumFailureCount = 0;
  g_work.irc.work.completionAction = IR_ACTION_NONE;
  g_work.irc.work.sessionFlags.bits.receivedBurst = 0;
  g_work.irc.work.writeOnlyZeroByte = 0;
  g_work.irc.work.bulkPhase = PEER_BULK_IDLE;
  g_work.irc.work.timeoutRetryCount++;
  IrTransmitByte(IR_CONNECT);
  ir_begin_turn();
}

void IrFinish(void);

/* The peer-status exchange has its own request/reply role. */
static u8 IsPeerStatusResponder(void)
{
  return g_ui.view.ir.role.bits.peerStatusResponder;
}

/* Peer-info construction reuses the page-decompression buffer. */
static Pokemon *CoursePokemonBuffer(void)
{
  Pokemon *record;

  record = (Pokemon *)g_work.irc.eepromScratch;
  return record;
}

/* Fill outgoing peer info with local trainer and Pokemon details and current
 * step totals. */
static void BuildPeerInfo(void)
{
  PeerInfo *peerInfo;
  Pokemon *pokemon;
  int i;

  pokemon = CoursePokemonBuffer();
  peerInfo = (PeerInfo *)IrPayload();
  peerInfo->dailySteps = g_state.dailySteps;
  peerInfo->hourSteps = g_state.hourSteps;
  EepromRead(
      PW_EEPROM_MEMBER_ADDRESS(EEPROM_COURSE, CourseResources, values.pokemon),
      pokemon, sizeof(Pokemon));
  peerInfo->fixedFacing = pokemon->fixedFacing;
  peerInfo->idLe = pokemon->idLe;
  peerInfo->form = pokemon->form;
  peerInfo->sex = pokemon->sex;
  peerInfo->shiny = pokemon->shiny;
  EepromMirrorRead(EEPROM_STATUS_PRIMARY, EEPROM_STATUS_BACKUP,
                   (u8 *)&g_work.irc.statusB.status, sizeof(DeviceStatus));
  peerInfo->compatibilityLe = g_work.irc.statusB.status.consoleCompatibilityLe;
  peerInfo->gameVersionLe = g_work.irc.statusB.status.gameVersionLe;
  i = 0;
  do {
    peerInfo->trainerName[i] = g_work.irc.statusB.status.trainerNameData[i];
    i++;
  } while (i < TRAINER_NAME_BYTES);
  EepromRead((u16)PW_EEPROM_MEMBER_ADDRESS(EEPROM_COURSE, CourseResources,
                                           values.nickname),
             peerInfo->nickname, sizeof(peerInfo->nickname));
}

/* A read request carries the remote EEPROM address high byte first,
 * followed by a capped byte count. */
static void RequestBulkChunk(void)
{
  u16 chunk;
  u16 src;
  u8 *out;

  const u16 remaining = g_work.irc.work.bulkBytesRemaining;
  chunk = remaining;
  if (chunk > PW_IR_BULK_MAXIMUM_CHUNK_LENGTH) {
    chunk = PW_IR_BULK_MAXIMUM_CHUNK_LENGTH;
  }
  out = IrPayload();
  src = g_work.irc.work.bulkSourceEepromAddress;
  out[0] = (src >> 8);
  out[1] = src;
  out[2] = chunk;
  SendPacket(3, IR_CMD_EEPROM_READ, IR_WALKER_PARAMETER);
}

static bool decode_page(const u8 *in, size_t size, u8 out[128])
{
  if (size < 5 || in[1] != 128) {
    return false;
  }
  size_t src = 4, dst = 0;
  while (dst < 128) {
    if (src >= size) {
      return false;
    }
    u8 flags = in[src++];
    for (unsigned bit = 0; bit < 8 && dst < 128; bit++, flags <<= 1) {
      if (!(flags & 128)) {
        if (src >= size) {
          return false;
        }
        out[dst++] = in[src++];
      } else {
        if (src + 2 > size) {
          return false;
        }
        unsigned count = (in[src] >> 4) + 3;
        unsigned back = in[src + 1] + 1;
        src += 2;
        if (back > dst || count > 128 - dst) {
          return false;
        }
        while (count--) {
          out[dst] = out[dst - back];
          dst++;
        }
      }
    }
  }
  return true;
}
static bool packet_payload_valid(u8 command, u8 argument, const u8 *payload,
                                 u8 n)
{
  switch (command) {
  case IR_CMD_PEER_STATUS_REQUEST:
  case IR_CMD_PEER_STATUS_REPLY:
  case IR_CMD_WALK_START_REQUEST:
  case IR_CMD_WALK_END_REQUEST:
  case IR_CMD_WALK_UPDATE_REQUEST:
  case IR_CMD_GIFT_REQUEST:
    return n == sizeof(DeviceStatus);
  case IR_CMD_EVENT_COURSE_CHECK:
  case IR_CMD_EVENT_ITEM_CHECK:
  case IR_CMD_EVENT_POKEMON_CHECK:
  case IR_CMD_EVENT_MAP_CHECK:
  case IR_CMD_EVENT_STAMP0_CHECK:
  case IR_CMD_EVENT_STAMP1_CHECK:
  case IR_CMD_EVENT_STAMP2_CHECK:
  case IR_CMD_EVENT_STAMP3_CHECK:
    return n == sizeof(DeviceStatus) &&
           payload[offsetof(DeviceStatus, receiptIndex)] <
               sizeof(((DeviceStatus *)0)->receivedEvents) * 8;
  case IR_CMD_PEER_INFO:
    return n == sizeof(PeerInfo);
  case IR_CMD_EEPROM_READ:
    return n == 3 && payload[2] <= IR_PAYLOAD_BYTES &&
           ((u32)payload[0] << 8) + payload[1] + payload[2] <= 65536;
  case IR_CMD_EEPROM_WRITE:
    return n >= 1 && ((u32)argument << 8) + payload[0] + n - 1 <= 65536;
  case IR_CMD_EEPROM_REPLY:
    return n > 0 && n <= g_work.irc.work.bulkBytesRemaining &&
           (u32)g_work.irc.work.bulkDestinationEepromAddress + n <= 65536 &&
           (g_work.irc.work.bulkPhase == PEER_BULK_RECEIVE_ICON ||
            g_work.irc.work.bulkPhase == PEER_BULK_RECEIVE_NAME ||
            g_work.irc.work.bulkPhase == PEER_BULK_RECEIVE_RECORDS);
  case IR_CMD_PAGE_COMPRESSED_A:
  case IR_CMD_PAGE_COMPRESSED_B:
    return n == IR_PAYLOAD_BYTES || n >= 5;
  case IR_CMD_PAGE_WRITE_A:
  case IR_CMD_PAGE_WRITE_B:
    return n > 0;
  case IR_CMD_RAM_WRITE:
  case IR_CMD_FACTORY_SETUP:
  case IR_CMD_MOTION_TEST_SETUP:
    return false;
  default:
    return true;
  }
}
static void ir_schedule_probe(uint64_t now)
{
  u16 jitter = (((RandomNext() >> 5) & PW_IR_RETRY_JITTER_MASK) *
                PW_IR_RETRY_JITTER_TICKS);
  ir_probe_deadline = now + ((uint64_t)jitter * 1000000 + 32767) / 32768;
  ir_turn = IR_BACKOFF;
}

static void ir_turn_timeout(uint64_t now);

static void ir_send_probe(void)
{
  if (g_work.irc.work.timeoutRetryCount >= PW_IR_TIMEOUT_RETRY_LIMIT) {
    ir_turn_timeout(pw_host_now_us());
    return;
  }
  g_work.irc.work.timeoutRetryCount++;
  ir_turn = IR_LISTEN;
  g_work.irc.work.handshakePhase = IR_PHASE_PROBING;
  IrTransmitByte(IR_CONNECT);
  DisplaySelectBank((ir_ticks_at(pw_host_now_us()) >> 14) & 1);
  ir_begin_turn();
}

static void ir_turn_timeout(uint64_t now)
{
  if (g_work.irc.work.handshakePhase >= IR_PHASE_INITIATOR ||
      g_work.irc.work.timeoutRetryCount >= PW_IR_TIMEOUT_RETRY_LIMIT) {
    g_state.irResult = g_work.irc.work.sessionFlags.bits.receivedBurst
        ? IR_RESULT_CONNECTION_ERROR : IR_RESULT_NO_RESPONSE;
    IrFinish();
    return;
  }
  ir_schedule_probe(now);
}

/* SCI3 bytes retain their capture times. Quiet observations advance Timer W
 * only after all earlier bytes have been delivered by the physical decoder. */
void IrProtocolTick(void)
{
  u16 timerSample;
  u16 displayBank;
  u32 receivedToken;
  u16 receivedChecksum;
  u16 computedChecksum;
  IrcHeader *header;
  DeviceStatus *payloadStatus;
  u8 payloadLength;
  u8 *payload;
  u8 command;
  u8 argument;

  if (ir_failed) {
    g_state.irResult = IR_RESULT_CONNECTION_ERROR;
    IrFinish();
    return;
  }
  uint64_t now = pw_host_now_us();
  if (ir_turn == IR_BACKOFF) {
    if (now >= ir_probe_deadline) ir_send_probe();
    return;
  }
  // Capture timestamps delimit bursts; elapsed host time bounds the turn even
  // if the decoder cannot produce a quiet horizon. One in-flight frame may drain.
  if (now > ir_turn_deadline + (ir_turn == IR_BURST ? 12000 : 0)) {
    ir_has_byte = false;
    ir_turn_timeout(now);
    return;
  }
  if (!ir_has_byte && pw_host_infrared_receive(&ir_event)) {
    ir_observed_us = ir_event.time_us;
    ir_has_byte = ir_event.byte;
  }
  u8 receivedBytes = g_state.irReceivedBytes;
  if (ir_has_byte) {
    if (ir_event.time_us <= ir_reference_us) {
      ir_has_byte = false;
      return;
    }
    u16 gap = (u16)(ir_ticks_at(ir_event.time_us) - g_state.irTimerReference);
    /* A damaged burst holds no bytes, but a later byte past the frame gap
     * starts the next packet rather than joining the damaged one. */
    if (ir_burst_corrupt && !receivedBytes && gap > PW_IR_FRAME_GAP_TICKS) {
      if (link_damaged < UINT16_MAX) link_damaged++;
      ir_burst_corrupt = false;
    }
    if (!receivedBytes || gap <= PW_IR_FRAME_GAP_TICKS) {
      ir_has_byte = false;
      ir_turn = IR_BURST;
      ir_burst_corrupt |= ir_event.errors != 0 || receivedBytes >= PW_IR_RX_WINDOW_CAPACITY;
      if (!ir_burst_corrupt) {
        g_work.irc.packet[g_state.irReceivedBytes++] = ir_event.value;
      }
      ir_reference_us = ir_event.time_us;
      g_state.irTimerReference = ir_ticks_at(ir_reference_us);
      return;
    }
  }
  if (ir_observed_us <= ir_reference_us) return;
  u16 elapsedTicks = (u16)(ir_ticks_at(ir_observed_us) - g_state.irTimerReference);
  if (elapsedTicks <= PW_IR_FRAME_GAP_TICKS) return;
  ir_turn = IR_LISTEN;
  if (ir_burst_corrupt) {
    if (link_damaged < UINT16_MAX) link_damaged++;
    ir_burst_corrupt = false;
    g_state.irReceivedBytes = 0;
    return;
  }
  if (!receivedBytes) return;
  g_state.irReceivedBytes = 0;
  g_work.irc.work.sessionFlags.bits.receivedBurst = 1;
  if (receivedBytes == 1) {
    /* Handle CONNECT as a single-byte probe. */
    if (g_work.irc.packet[0] != IR_CONNECT) {
      return;
    }
    pw_host_infrared_detected();
    /* The probing phase answers CONNECT. */
    switch (g_work.irc.work.handshakePhase) {
    case IR_PHASE_PROBING:
      ir_turn = IR_LISTEN;
      g_work.irc.work.handshakePhase = IR_PHASE_REPLY_SENT;
      SendPacket(0, IR_RESPONSE, IR_WALKER_PARAMETER);
      ir_begin_turn();
      break;
    case IR_PHASE_REPLY_SENT:
      break;
    case IR_PHASE_RESPONDER:
    case IR_PHASE_INITIATOR:
      break;
    }
    return;
  }

  if (receivedBytes < IR_HEADER_BYTES) {
    return;
  }
  header = (IrcHeader *)g_work.irc.packet;
  receivedChecksum = (header->checksumLo + (header->checksumHi << 8));
  header->checksumLo = 0;
  header->checksumHi = 0;
  computedChecksum = PacketChecksum(g_work.irc.packet, receivedBytes);
  if (receivedChecksum != computedChecksum) {
    g_work.irc.work.checksumFailureCount++;
    if (link_damaged < UINT16_MAX) link_damaged++;
    if (g_work.irc.work.checksumFailureCount < PW_IR_CHECKSUM_FAIL_LIMIT) {
      return;
    }
    g_state.irResult = IR_RESULT_CONNECTION_ERROR;
    IrFinish();
    return;
  }

  /* Keep the received token before constructing a reply in the same buffer. */
  receivedToken = pw_read_be32(g_work.irc.packet + 4);

  argument = header->argument;
  command = header->command;
  payloadLength = receivedBytes - IR_HEADER_BYTES;

  payload = g_work.irc.packet + IR_HEADER_BYTES;
  if (command < IR_ACK && (receivedToken != g_work.irc.work.sessionToken ||
                           g_work.irc.work.handshakePhase < IR_PHASE_INITIATOR)) {
    return;
  }
  if (!packet_payload_valid(command, argument, payload, payloadLength)) {
    return;
  }
  link_last_command = command;
  if (link_packets < UINT16_MAX) link_packets++;
  pw_host_infrared_detected();
  ir_turn = IR_LISTEN;
  switch (command) {
  case IR_CMD_PEER_STATUS_REQUEST:
  case IR_CMD_PEER_STATUS_REPLY:
  case IR_CMD_WALK_START_REQUEST:
  case IR_CMD_WALK_END_REQUEST:
  case IR_CMD_WALK_UPDATE_REQUEST:
  case IR_CMD_GIFT_REQUEST:
  case IR_CMD_EVENT_COURSE_CHECK:
  case IR_CMD_EVENT_ITEM_CHECK:
  case IR_CMD_EVENT_POKEMON_CHECK:
  case IR_CMD_EVENT_MAP_CHECK:
  case IR_CMD_EVENT_STAMP0_CHECK:
  case IR_CMD_EVENT_STAMP1_CHECK:
  case IR_CMD_EVENT_STAMP2_CHECK:
  case IR_CMD_EVENT_STAMP3_CHECK:
    pw_status_swap((DeviceStatus *)payload);
    break;
  case IR_CMD_PEER_INFO:
    pw_peer_swap((PeerInfo *)payload);
    break;
  default:
    break;
  }
  switch (command) {
  case IR_RESPONSE:
    if ((argument == IR_CONSOLE_PARAMETER) ||
        (argument == IR_WALKER_PARAMETER)) {
      switch (g_work.irc.work.handshakePhase) {
      case IR_PHASE_PROBING:
        g_work.irc.work.handshakePhase = IR_PHASE_INITIATOR;
        SendPacket(0, IR_ACK, IR_WALKER_PARAMETER);
        g_work.irc.work.sessionToken =
            receivedToken ^ g_work.irc.work.localHandshakeToken;
        break;
      case IR_PHASE_RESPONDER:
      case IR_PHASE_INITIATOR:
      case IR_PHASE_REPLY_SENT:
        ir_schedule_probe(pw_host_now_us());
        break;
      }
    } else {
      g_state.irResult = IR_RESULT_REJECTED;
      IrFinish();
    }
    break;
  case IR_SHUTDOWN:
    IrFinish();
    break;
  case IR_ACK:
    /* A Walker acknowledgement starts the peer-status exchange. */
    if (argument != IR_WALKER_PARAMETER) {
      g_state.irResult = IR_RESULT_REJECTED;
      IrFinish();
      break;
    }
    if (g_work.irc.work.handshakePhase < IR_PHASE_INITIATOR) {
      g_work.irc.work.sessionToken =
          receivedToken ^ g_work.irc.work.localHandshakeToken;
      g_work.irc.work.handshakePhase = IR_PHASE_RESPONDER;
      EepromMirrorRead(EEPROM_STATUS_PRIMARY, EEPROM_STATUS_BACKUP, IrPayload(),
                       sizeof(DeviceStatus));
      SendPacket(sizeof(DeviceStatus), IR_CMD_PEER_STATUS_REQUEST,
                 IR_WALKER_PARAMETER);
      g_ui.view.ir.role.bits.peerStatusResponder = 0;
    }
    break;
  case IR_CMD_PEER_STATUS_REQUEST:
    /* Each failed eligibility check sends a status reply and ends the session.
     * A previously recorded peer gets a dedicated reply. */
    g_ui.view.ir.role.bits.peerStatusResponder = 1;
    payloadStatus = (DeviceStatus *)IrPayload();
    /* Save peer status before loading local status into the payload buffer. */
    g_work.irc.statusA.status = *payloadStatus;
    EepromMirrorRead(EEPROM_STATUS_PRIMARY, EEPROM_STATUS_BACKUP,
                     (u8 *)payloadStatus, sizeof(DeviceStatus));
    if (payloadStatus->registered == 0) {
      SendPacket(sizeof(DeviceStatus), IR_CMD_PEER_STATUS_REPLY,
                 IR_WALKER_PARAMETER);
      g_state.irResult = IR_RESULT_REJECTED;
      IrFinish();
      break;
    }
    if (g_work.irc.statusA.status.registered == 0) {
      SendPacket(sizeof(DeviceStatus), IR_CMD_PEER_STATUS_REPLY,
                 IR_WALKER_PARAMETER);
      g_state.irResult = IR_RESULT_REJECTED;
      IrFinish();
      break;
    }
    if (g_work.irc.statusA.status.hasPokemon == 0) {
      SendPacket(sizeof(DeviceStatus), IR_CMD_PEER_STATUS_REPLY,
                 IR_WALKER_PARAMETER);
      g_state.irResult = IR_RESULT_REJECTED;
      IrFinish();
      break;
    }
    if (payloadStatus->peerProtocol != g_work.irc.statusA.status.peerProtocol) {
      SendPacket(sizeof(DeviceStatus), IR_CMD_PEER_STATUS_REPLY,
                 IR_WALKER_PARAMETER);
      g_state.irResult = IR_RESULT_REJECTED;
      IrFinish();
      break;
    }
    if (g_work.irc.statusA.status.firmwareCompatibility != 0) {
      SendPacket(sizeof(DeviceStatus), IR_CMD_PEER_STATUS_REPLY,
                 IR_WALKER_PARAMETER);
      g_state.irResult = IR_RESULT_REJECTED;
      IrFinish();
      break;
    }
    if (payloadStatus->hasPokemon == 0) {
      SendPacket(sizeof(DeviceStatus), IR_CMD_PEER_STATUS_REPLY,
                 IR_WALKER_PARAMETER);
      g_state.irResult = IR_RESULT_NO_POKEMON;
      IrFinish();
      break;
    }
    if (SeenPeer(g_work.irc.statusA.status.deviceId) != 0) {
      SendPacket(0, IR_CMD_PEER_ALREADY_RECORDED, IR_WALKER_PARAMETER);
      g_state.irResult = IR_RESULT_PEER_ALREADY_SEEN;
      IrFinish();
      break;
    }
    SendPacket(sizeof(DeviceStatus), IR_CMD_PEER_STATUS_REPLY,
               IR_WALKER_PARAMETER);
    break;
  case IR_CMD_PEER_STATUS_REPLY:
    /* Check status locally and end the session on failure. A previously
     * recorded peer gets a dedicated reply; a valid peer starts icon transfer.
     */
    payloadStatus = (DeviceStatus *)IrPayload();
    /* Save peer status before loading local status into the payload buffer. */
    g_work.irc.statusA.status = *payloadStatus;
    EepromMirrorRead(EEPROM_STATUS_PRIMARY, EEPROM_STATUS_BACKUP,
                     (u8 *)payloadStatus, sizeof(DeviceStatus));
    if (payloadStatus->registered == 0) {
      g_state.irResult = IR_RESULT_REJECTED;
      IrFinish();
      break;
    }
    if (g_work.irc.statusA.status.registered == 0) {
      g_state.irResult = IR_RESULT_REJECTED;
      IrFinish();
      break;
    }
    if (g_work.irc.statusA.status.hasPokemon == 0) {
      g_state.irResult = IR_RESULT_REJECTED;
      IrFinish();
      break;
    }
    if (payloadStatus->peerProtocol != g_work.irc.statusA.status.peerProtocol) {
      g_state.irResult = IR_RESULT_REJECTED;
      IrFinish();
      break;
    }
    if (g_work.irc.statusA.status.firmwareCompatibility != 0) {
      g_state.irResult = IR_RESULT_REJECTED;
      IrFinish();
      break;
    }
    if (payloadStatus->hasPokemon == 0) {
      g_state.irResult = IR_RESULT_NO_POKEMON;
      IrFinish();
      break;
    }
    if (SeenPeer(g_work.irc.statusA.status.deviceId) != 0) {
      SendPacket(0, IR_CMD_PEER_ALREADY_RECORDED, IR_WALKER_PARAMETER);
      g_state.irResult = IR_RESULT_PEER_ALREADY_SEEN;
      IrFinish();
      break;
    }
    BeginBulkPhase(
        PEER_BULK_SEND_ICON,
        PW_EEPROM_MEMBER_ADDRESS(EEPROM_COURSE, CourseResources, pokemonImage),
        EEPROM_PEER_IMAGE, sizeof(((CourseResources *)0)->pokemonImage));
    {
      u16 chunk;
      u16 dest;

      chunk = g_work.irc.work.bulkBytesRemaining;
      if (chunk > PW_IR_BULK_MAXIMUM_CHUNK_LENGTH) {
        chunk = PW_IR_BULK_MAXIMUM_CHUNK_LENGTH;
      }
      pw_eeprom_read_raw(g_work.irc.work.bulkSourceEepromAddress, IrPayload(),
                         chunk);
      dest = g_work.irc.work.bulkDestinationEepromAddress;
      SendPacket(chunk, ((dest & 0x80) | IR_CMD_PAGE_WRITE_A), (dest >> 8));
      g_work.irc.work.bulkSourceEepromAddress =
          (g_work.irc.work.bulkSourceEepromAddress + chunk);
      g_work.irc.work.bulkDestinationEepromAddress =
          (g_work.irc.work.bulkDestinationEepromAddress + chunk);
      g_work.irc.work.bulkBytesRemaining =
          (g_work.irc.work.bulkBytesRemaining - chunk);
      g_work.irc.work.bulkChunksCompleted++;
    }
    break;
  case IR_CMD_PEER_INFO:
    if (IsPeerStatusResponder() != 0) {

      EepromWrite(EEPROM_PEER_INFO, IrPayload(), sizeof(PeerInfo));
      BuildPeerInfo();
      SendPacket(sizeof(PeerInfo), IR_CMD_PEER_INFO, IR_WALKER_PARAMETER);
    } else {
      EepromWrite(EEPROM_PEER_INFO, IrPayload(), sizeof(PeerInfo));
      SendPacket(0, IR_CMD_PEER_START, IR_WALKER_PARAMETER);
    }
    break;
  case IR_CMD_PEER_START:
    g_work.irc.work.completionAction = IR_CMD_PEER_START;
    if (IsPeerStatusResponder() != 0) {
      SendPacket(0, IR_CMD_PEER_START, IR_WALKER_PARAMETER);
    }
    g_work.irc.work.completionAction = IR_CMD_PEER_START;
    IrFinish();
    break;
  case IR_CMD_PEER_ALREADY_RECORDED:
    g_state.irResult = IR_RESULT_PEER_ALREADY_SEEN;
    IrFinish();
    break;
  case IR_CMD_STATUS_REQUEST:
    payloadStatus = (DeviceStatus *)IrPayload();
    EepromMirrorRead(EEPROM_STATUS_PRIMARY, EEPROM_STATUS_BACKUP,
                     (u8 *)IrPayload(), sizeof(DeviceStatus));
    payloadStatus->totalSteps = g_state.save.totalSteps;
    EepromMirrorWrite(EEPROM_SAVE_PRIMARY, EEPROM_SAVE_BACKUP,
                      (u8 *)&g_state.save, sizeof(SaveData));
    EepromWrite(PW_EEPROM_MEMBER_ADDRESS(EEPROM_WALK, WalkData, watts),
                &g_state.save.watts, sizeof(g_state.save.watts));
    g_work.irc.statusB.status = *payloadStatus;
    SendPacket(sizeof(DeviceStatus), IR_CMD_STATUS_REPLY, IR_WALKER_PARAMETER);
    break;
  case IR_CMD_WALK_START_REQUEST:
    StatusApplyTime();
    EepromMirrorWrite(EEPROM_SAVE_PRIMARY, EEPROM_SAVE_BACKUP,
                      (u8 *)&g_state.save, sizeof(SaveData));
    SendPacket(0, IR_CMD_WALK_START_ACCEPTED, IR_WALKER_PARAMETER);
    break;
  case IR_CMD_WALK_START_REJECTED:
    g_state.irResult = IR_RESULT_REJECTED;
    IrFinish();
    break;
  case IR_CMD_WALK_START_COMMIT:
    g_work.irc.work.completionAction = IR_CMD_WALK_START_COMMIT;
    SendPacket(0, IR_CMD_WALK_START_COMMIT, IR_WALKER_PARAMETER);
    break;
  case IR_CMD_WALK_END_REQUEST:
    StatusApplyTime();
    EepromMirrorWrite(EEPROM_SAVE_PRIMARY, EEPROM_SAVE_BACKUP,
                      (u8 *)&g_state.save, sizeof(SaveData));
    SendPacket(0, IR_CMD_WALK_END_ACCEPTED, IR_WALKER_PARAMETER);
    break;
  case IR_CMD_WALK_END_REJECTED:
    g_state.irResult = IR_RESULT_REJECTED;
    IrFinish();
    break;
  case IR_CMD_WALK_END_COMMIT:
    g_work.irc.work.completionAction = IR_CMD_WALK_END_COMMIT;
    SendPacket(0, IR_CMD_WALK_END_ACK, IR_WALKER_PARAMETER);
    IrFinish();
    break;
  case IR_CMD_WALK_UPDATE_REQUEST:
    StatusApplyTime();
    EepromMirrorWrite(EEPROM_SAVE_PRIMARY, EEPROM_SAVE_BACKUP,
                      (u8 *)&g_state.save, sizeof(SaveData));
    SendPacket(0, IR_CMD_WALK_UPDATE_ACCEPTED, IR_WALKER_PARAMETER);
    break;
  case IR_CMD_WALK_UPDATE_COMMIT:
    g_work.irc.work.completionAction = IR_CMD_WALK_UPDATE_COMMIT;
    SendPacket(0, IR_CMD_WALK_UPDATE_COMMIT, IR_WALKER_PARAMETER);
    IrFinish();
    break;
  case IR_CMD_WALK_UPDATE_REJECTED:
    g_state.irResult = IR_RESULT_REJECTED;
    IrFinish();
    break;
  case IR_CMD_GIFT_REQUEST:
    StatusApplyTime();
    EepromMirrorRead(EEPROM_STATUS_PRIMARY, EEPROM_STATUS_BACKUP,
                     (u8 *)&g_work.irc.statusB.status, sizeof(DeviceStatus));
    SendPacket(0, IR_CMD_GIFT_ACCEPTED, IR_WALKER_PARAMETER);
    break;
  case IR_CMD_GIFT_COLLECTION_COMMIT:
    g_work.irc.work.completionAction = IR_CMD_GIFT_COLLECTION_COMMIT;
    SendPacket(0, IR_CMD_GIFT_COLLECTION_ACK, IR_WALKER_PARAMETER);
    break;
  case IR_CMD_GIFT_REJECTED:
    g_state.irResult = IR_RESULT_REJECTED;
    IrFinish();
    break;
  case IR_CMD_EVENT_MAP_DONE: {
    u8 flags;

    flags = EepromReadByte(EEPROM_EVENTS);
    flags |= EVENT_PRESENT_MAP;
    EepromWriteByte(EEPROM_EVENTS, flags);
    StatusSetReceived(&g_work.irc.statusB.status,
                      g_work.irc.statusA.status.receiptIndex);
    g_work.irc.work.completionAction = IR_CMD_EVENT_MAP_DONE;
    SendPacket(0, IR_CMD_EVENT_MAP_DONE, IR_WALKER_PARAMETER);
    IrFinish();
    break;
  }
  case IR_CMD_EVENT_STAMP_MAP_DONE: {
    u8 flags;

    flags = EepromReadByte(EEPROM_EVENTS);
    flags |= (EVENT_PRESENT_MAP | EVENT_PRESENT_STAMPS);
    EepromWriteByte(EEPROM_EVENTS, flags);
    StatusSetReceived(&g_work.irc.statusB.status,
                      g_work.irc.statusA.status.receiptIndex);
    g_work.irc.work.completionAction = IR_CMD_EVENT_MAP_DONE;
    SendPacket(0, IR_CMD_EVENT_MAP_DONE, IR_WALKER_PARAMETER);
    IrFinish();
    break;
  }
  case IR_CMD_EVENT_POKEMON_DONE: {
    u8 flags;

    flags = EepromReadByte(EEPROM_EVENTS);
    flags |= EVENT_PRESENT_POKEMON;
    EepromWriteByte(EEPROM_EVENTS, flags);
    StatusSetReceived(&g_work.irc.statusB.status,
                      g_work.irc.statusA.status.receiptIndex);
    g_work.irc.work.completionAction = IR_CMD_EVENT_POKEMON_DONE;
    SendPacket(0, IR_CMD_EVENT_POKEMON_DONE, IR_WALKER_PARAMETER);
    IrFinish();
    break;
  }
  case IR_CMD_EVENT_STAMP_POKEMON_DONE: {
    u8 flags;

    flags = EepromReadByte(EEPROM_EVENTS);
    flags |= (EVENT_PRESENT_POKEMON | EVENT_PRESENT_STAMPS);
    EepromWriteByte(EEPROM_EVENTS, flags);
    StatusSetReceived(&g_work.irc.statusB.status,
                      g_work.irc.statusA.status.receiptIndex);
    g_work.irc.work.completionAction = IR_CMD_EVENT_POKEMON_DONE;
    SendPacket(0, IR_CMD_EVENT_POKEMON_DONE, IR_WALKER_PARAMETER);
    IrFinish();
    break;
  }
  case IR_CMD_EVENT_ITEM_DONE: {
    u8 flags;

    flags = EepromReadByte(EEPROM_EVENTS);
    flags |= EVENT_PRESENT_ITEM;
    EepromWriteByte(EEPROM_EVENTS, flags);
    StatusSetReceived(&g_work.irc.statusB.status,
                      g_work.irc.statusA.status.receiptIndex);
    g_work.irc.work.completionAction = IR_CMD_EVENT_ITEM_DONE;
    SendPacket(0, IR_CMD_EVENT_ITEM_DONE, IR_WALKER_PARAMETER);
    IrFinish();
    break;
  }
  case IR_CMD_EVENT_STAMP_ITEM_DONE: {
    u8 flags;

    flags = EepromReadByte(EEPROM_EVENTS);
    flags |= (EVENT_PRESENT_ITEM | EVENT_PRESENT_STAMPS);
    EepromWriteByte(EEPROM_EVENTS, flags);
    StatusSetReceived(&g_work.irc.statusB.status,
                      g_work.irc.statusA.status.receiptIndex);
    g_work.irc.work.completionAction = IR_CMD_EVENT_ITEM_DONE;
    SendPacket(0, IR_CMD_EVENT_ITEM_DONE, IR_WALKER_PARAMETER);
    IrFinish();
    break;
  }
  case IR_CMD_EVENT_COURSE_DONE: {
    u8 flags;

    flags = EepromReadByte(EEPROM_EVENTS);
    flags |= EVENT_PRESENT_COURSE;
    EepromWriteByte(EEPROM_EVENTS, flags);
    StatusSetReceived(&g_work.irc.statusB.status,
                      g_work.irc.statusA.status.receiptIndex);
    g_state.save.bonusCourse = 1;
    EepromMirrorWrite(EEPROM_SAVE_PRIMARY, EEPROM_SAVE_BACKUP,
                      (u8 *)&g_state.save, sizeof(SaveData));
    g_work.irc.work.completionAction = IR_CMD_EVENT_COURSE_DONE;
    SendPacket(0, IR_CMD_EVENT_COURSE_DONE, IR_WALKER_PARAMETER);
    IrFinish();
    break;
  }
  case IR_CMD_EVENT_STAMP_COURSE_DONE: {
    u8 flags;

    flags = EepromReadByte(EEPROM_EVENTS);
    flags |= (EVENT_PRESENT_COURSE | EVENT_PRESENT_STAMPS);
    EepromWriteByte(EEPROM_EVENTS, flags);
    StatusSetReceived(&g_work.irc.statusB.status,
                      g_work.irc.statusA.status.receiptIndex);
    g_state.save.bonusCourse = 1;
    EepromMirrorWrite(EEPROM_SAVE_PRIMARY, EEPROM_SAVE_BACKUP,
                      (u8 *)&g_state.save, sizeof(SaveData));
    g_work.irc.work.completionAction = IR_CMD_EVENT_COURSE_DONE;
    SendPacket(0, IR_CMD_EVENT_COURSE_DONE, IR_WALKER_PARAMETER);
    IrFinish();
    break;
  }
  case IR_CMD_EVENT_REJECTED:
    g_state.irResult = IR_RESULT_REJECTED;
    IrFinish();
    break;
  case IR_CMD_PING_REQUEST:
    SendPacket(0, IR_CMD_PING_REPLY, IR_WALKER_PARAMETER);
    break;
  case IR_CMD_PAGE_COMPRESSED_B:
  case IR_CMD_PAGE_COMPRESSED_A: {
    u16 address;

    address = (((u16)argument << 8) + command);
    if (payloadLength == PW_IR_BULK_MAXIMUM_CHUNK_LENGTH) {
      pw_eeprom_write_raw(address, payload, EEPROM_PAGE_BYTES);
    } else {
      if (!decode_page(payload, payloadLength, g_work.irc.eepromScratch)) {
        goto packetDone;
      }
      pw_eeprom_write_raw(address, g_work.irc.eepromScratch, EEPROM_PAGE_BYTES);
    }
    SendPacket(0, IR_CMD_PAGE_REPLY, argument);
    break;
  }
  case IR_CMD_PAGE_WRITE_B:
  case IR_CMD_PAGE_WRITE_A: {
    u16 address;

    address = (((u16)argument << 8) + (command & 0x80));
    pw_eeprom_write_raw(address, payload, payloadLength);
    SendPacket(0, IR_CMD_PAGE_REPLY, argument);
    break;
  }
  case IR_CMD_PAGE_REPLY: {
    u16 chunk;
    u16 dest;

    if (g_work.irc.work.bulkBytesRemaining == 0) {
      switch (g_work.irc.work.bulkPhase) {
      case PEER_BULK_SEND_ICON:
        BeginBulkPhase(PEER_BULK_SEND_NAME,
                       PW_EEPROM_MEMBER_ADDRESS(EEPROM_COURSE, CourseResources,
                                                pokemonName),
                       EEPROM_PEER_NAME,
                       sizeof(((CourseResources *)0)->pokemonName));
        chunk = g_work.irc.work.bulkBytesRemaining;
        if (chunk > PW_IR_BULK_MAXIMUM_CHUNK_LENGTH) {
          chunk = PW_IR_BULK_MAXIMUM_CHUNK_LENGTH;
        }
        pw_eeprom_read_raw(g_work.irc.work.bulkSourceEepromAddress, IrPayload(),
                           chunk);
        dest = g_work.irc.work.bulkDestinationEepromAddress;
        SendPacket(chunk, ((dest & 0x80) | IR_CMD_PAGE_WRITE_A), (dest >> 8));
        g_work.irc.work.bulkSourceEepromAddress =
            (g_work.irc.work.bulkSourceEepromAddress + chunk);
        g_work.irc.work.bulkDestinationEepromAddress =
            (g_work.irc.work.bulkDestinationEepromAddress + chunk);
        g_work.irc.work.bulkBytesRemaining =
            (g_work.irc.work.bulkBytesRemaining - chunk);
        g_work.irc.work.bulkChunksCompleted++;
        break;
      case PEER_BULK_SEND_NAME:
        BeginBulkPhase(PEER_BULK_SEND_RECORDS, EEPROM_OWN_RECORDS,
                       EEPROM_PEER_RECORDS, sizeof(PeerRecords));
        chunk = g_work.irc.work.bulkBytesRemaining;
        if (chunk > PW_IR_BULK_MAXIMUM_CHUNK_LENGTH) {
          chunk = PW_IR_BULK_MAXIMUM_CHUNK_LENGTH;
        }
        pw_eeprom_read_raw(g_work.irc.work.bulkSourceEepromAddress, IrPayload(),
                           chunk);
        dest = g_work.irc.work.bulkDestinationEepromAddress;
        SendPacket(chunk, ((dest & 0x80) | IR_CMD_PAGE_WRITE_A), (dest >> 8));
        g_work.irc.work.bulkSourceEepromAddress =
            (g_work.irc.work.bulkSourceEepromAddress + chunk);
        g_work.irc.work.bulkDestinationEepromAddress =
            (g_work.irc.work.bulkDestinationEepromAddress + chunk);
        g_work.irc.work.bulkBytesRemaining =
            (g_work.irc.work.bulkBytesRemaining - chunk);
        g_work.irc.work.bulkChunksCompleted++;
        break;
      case PEER_BULK_SEND_RECORDS:
        BeginBulkPhase(PEER_BULK_RECEIVE_ICON,
                       PW_EEPROM_MEMBER_ADDRESS(EEPROM_COURSE, CourseResources,
                                                pokemonImage),
                       EEPROM_PEER_IMAGE,
                       sizeof(((CourseResources *)0)->pokemonImage));
        RequestBulkChunk();
        break;
      }
    } else {
      chunk = g_work.irc.work.bulkBytesRemaining;
      if (chunk > PW_IR_BULK_MAXIMUM_CHUNK_LENGTH) {
        chunk = PW_IR_BULK_MAXIMUM_CHUNK_LENGTH;
      }
      pw_eeprom_read_raw(g_work.irc.work.bulkSourceEepromAddress, IrPayload(),
                         chunk);
      dest = g_work.irc.work.bulkDestinationEepromAddress;
      SendPacket(chunk, ((dest & 0x80) | IR_CMD_PAGE_WRITE_A), (dest >> 8));
      g_work.irc.work.bulkSourceEepromAddress =
          (g_work.irc.work.bulkSourceEepromAddress + chunk);
      g_work.irc.work.bulkDestinationEepromAddress =
          (g_work.irc.work.bulkDestinationEepromAddress + chunk);
      g_work.irc.work.bulkBytesRemaining =
          (g_work.irc.work.bulkBytesRemaining - chunk);
      g_work.irc.work.bulkChunksCompleted++;
    }
    break;
  }
  case IR_CMD_EVENT_COURSE_CHECK:
  case IR_CMD_EVENT_ITEM_CHECK:
  case IR_CMD_EVENT_POKEMON_CHECK:
  case IR_CMD_EVENT_MAP_CHECK: {
    u8 i;
    u8 *out;

    StatusApplyTime();
    out = IrPayload();
    i = 0;
    do {
      *out++ = g_work.irc.statusB.status.receivedEvents[i];
      i++;
    } while (i < sizeof(g_work.irc.statusB.status.receivedEvents));
    *out = g_work.irc.statusA.status.receiptIndex;
    if (StatusLoadReceived(&g_work.irc.statusB.status,
                           g_work.irc.statusA.status.receiptIndex) != 0) {
      SendPacket((sizeof(g_work.irc.statusB.status.receivedEvents) + 1),
                 IR_CMD_EVENT_ALREADY_RECEIVED, IR_WALKER_PARAMETER);
      g_state.irResult = IR_RESULT_EVENT_ALREADY_RECEIVED;
      IrFinish();
      break;
    }
    SendPacket((sizeof(g_work.irc.statusB.status.receivedEvents) + 1), command,
               IR_WALKER_PARAMETER);
    break;
  }
  case IR_CMD_EVENT_STAMP3_CHECK:
  case IR_CMD_EVENT_STAMP2_CHECK:
  case IR_CMD_EVENT_STAMP1_CHECK:
  case IR_CMD_EVENT_STAMP0_CHECK: {
    u8 i;
    u8 *out;

    StatusApplyTime();
    out = IrPayload();
    i = 0;
    do {
      *out++ = g_work.irc.statusB.status.receivedEvents[i];
      i++;
    } while (i < sizeof(g_work.irc.statusB.status.receivedEvents));
    *out = g_work.irc.statusA.status.receiptIndex;
    if (StatusLoadReceived(&g_work.irc.statusB.status,
                           g_work.irc.statusA.status.receiptIndex) != 0) {
      SendPacket((sizeof(g_work.irc.statusB.status.receivedEvents) + 1),
                 IR_CMD_EVENT_ALREADY_RECEIVED, IR_WALKER_PARAMETER);
      g_state.irResult = IR_RESULT_EVENT_ALREADY_RECEIVED;
      IrFinish();
      break;
    }
    SendPacket((sizeof(g_work.irc.statusB.status.receivedEvents) + 1), command,
               IR_WALKER_PARAMETER);
    break;
  }
  case IR_CMD_EVENT_STAMP3:
  case IR_CMD_EVENT_STAMP2:
  case IR_CMD_EVENT_STAMP1:
  case IR_CMD_EVENT_STAMP0: {
    u8 flags;

    flags = EepromReadByte(EEPROM_EVENTS);
    switch (command) {
    case IR_CMD_EVENT_STAMP0:
      flags |= EVENT_PRESENT_STAMP0;
      g_work.irc.work.completionAction = IR_CMD_EVENT_STAMP0;
      break;
    case IR_CMD_EVENT_STAMP1:
      flags |= EVENT_PRESENT_STAMP1;
      g_work.irc.work.completionAction = IR_CMD_EVENT_STAMP1;
      break;
    case IR_CMD_EVENT_STAMP2:
      flags |= EVENT_PRESENT_STAMP2;
      g_work.irc.work.completionAction = IR_CMD_EVENT_STAMP2;
      break;
    case IR_CMD_EVENT_STAMP3:
      flags |= EVENT_PRESENT_STAMP3;
      g_work.irc.work.completionAction = IR_CMD_EVENT_STAMP3;
      break;
    }
    EepromWriteByte(EEPROM_EVENTS, flags);
    SendPacket(0, (command + 0x10), IR_WALKER_PARAMETER);
    IrFinish();
    break;
  }
  case IR_CMD_EVENT_ALREADY_RECEIVED:
    SendPacket(0, IR_CMD_EVENT_ALREADY_RECEIVED, IR_WALKER_PARAMETER);
    g_state.irResult = IR_RESULT_EVENT_NOT_RECEIVED;
    IrFinish();
    break;
  case IR_CMD_EVENT_BUFFER_FULL:
    SendPacket(0, IR_CMD_EVENT_BUFFER_FULL, IR_WALKER_PARAMETER);
    g_state.irResult = IR_RESULT_EVENT_NOT_RECEIVED;
    IrFinish();
    break;
  case IR_CMD_FACTORY_SETUP:
  case IR_CMD_MOTION_TEST_SETUP:
    g_state.irResult = IR_RESULT_REJECTED;
    IrFinish();
    break;
  case IR_CMD_RESET_ALL:
    EepromMirrorRead(EEPROM_ID_PRIMARY, EEPROM_ID_BACKUP, IrPayload(),
                     DEVICE_ID_BYTES);
    g_work.irc.work.completionAction = IR_CMD_RESET_ALL;
    SendPacket(DEVICE_ID_BYTES, IR_CMD_RESET_ALL, IR_WALKER_PARAMETER);
    break;
  case IR_CMD_RESET_KEEP_STEPS:
    EepromMirrorRead(EEPROM_ID_PRIMARY, EEPROM_ID_BACKUP, IrPayload(),
                     DEVICE_ID_BYTES);
    g_work.irc.work.completionAction = IR_CMD_RESET_KEEP_STEPS;
    SendPacket(DEVICE_ID_BYTES, IR_CMD_RESET_ALL, IR_WALKER_PARAMETER);
    break;
  case IR_CMD_EEPROM_READ: {
    u16 address;
    u8 length;

    address = ((payload[0] << 8) + payload[1]);
    length = payload[2];
    pw_eeprom_read_raw(address, IrPayload(), length);
    SendPacket(length, IR_CMD_EEPROM_REPLY, IR_WALKER_PARAMETER);
    break;
  }
  case IR_CMD_EEPROM_REPLY: {
    pw_eeprom_write_raw(g_work.irc.work.bulkDestinationEepromAddress,
                        IrPayload(), payloadLength);

    g_work.irc.work.bulkSourceEepromAddress += payloadLength;
    g_work.irc.work.bulkDestinationEepromAddress += payloadLength;
    g_work.irc.work.bulkBytesRemaining -= payloadLength;
    g_work.irc.work.bulkChunksCompleted++;
    if (g_work.irc.work.bulkBytesRemaining == 0) {
      switch (g_work.irc.work.bulkPhase) {
      case PEER_BULK_RECEIVE_ICON:
        BeginBulkPhase(PEER_BULK_RECEIVE_NAME,
                       PW_EEPROM_MEMBER_ADDRESS(EEPROM_COURSE, CourseResources,
                                                pokemonName),
                       EEPROM_PEER_NAME,
                       sizeof(((CourseResources *)0)->pokemonName));
        RequestBulkChunk();
        break;
      case PEER_BULK_RECEIVE_NAME:
        BeginBulkPhase(PEER_BULK_RECEIVE_RECORDS, EEPROM_OWN_RECORDS,
                       EEPROM_PEER_RECORDS, sizeof(PeerRecords));
        RequestBulkChunk();
        break;
      case PEER_BULK_RECEIVE_RECORDS:
        BuildPeerInfo();
        SendPacket(sizeof(PeerInfo), IR_CMD_PEER_INFO, IR_WALKER_PARAMETER);
        goto packetDone;
      default:
        goto packetDone;
      }
    } else {
      RequestBulkChunk();
    }
    break;
  }
  case IR_CMD_EEPROM_WRITE: {
    u16 address;

    address = (((u16)argument << 8) + payload[0]);
    pw_eeprom_write_raw(address, payload + 1, payloadLength - 1);
    SendPacket(0, IR_CMD_PAGE_REPLY, argument);
    break;
  }
  case IR_CMD_RAM_WRITE:
    g_state.irResult = IR_RESULT_REJECTED;
    IrFinish();
    break;
  default:
    break;
  }

packetDone:
  timerSample = ir_ticks_at(pw_host_now_us());
  /* Timer W bit 14 animates the two prepared connection frames. */
  displayBank = ((timerSample >> 14) & 1);
  DisplaySelectBank(displayBank);
  ir_begin_turn();
}

/* Stop the IR peripherals, then dispatch the session's completion action or
 * result. */
void IrFinish(void)
{
  pw_host_infrared_close();
  pw_host_link_report(g_state.irResult, g_work.irc.work.handshakePhase, link_last_command, link_packets, link_damaged);
  if (!ir_completion_sent) g_work.irc.work.completionAction = IR_ACTION_NONE;
  if (g_work.irc.work.completionAction != IR_ACTION_NONE) {
    IrComplete();
    if (!pw_save()) {
      pw_host_communication(false);
      pw_host_fault("The completed transfer could not be saved");
      return;
    }
    pw_host_communication(false);
  } else {
    pw_host_communication(false);
    pw_eeprom_mark_clean();
    pw_restore_runtime();
    StatusRestoreFlags();
    IrComplete();
  }
}

void pw_cancel_connection(void)
{
  if (!pw_communicating()) {
    return;
  }
  ir_failed = true;
  IrProtocolTick();
}
