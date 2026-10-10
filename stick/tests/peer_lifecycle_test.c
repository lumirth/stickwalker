
#include "project.h"
#include "application/pw_eeprom_m95512.h"
#include "application/pw_storage.h"
#include "application/pw_friend.h"
#include "application/pw_rtc.h"
#include "startup/h8_resetprg.h"
#include "support/ir.h"
#include "support/lib_common.h"
#include "support/scratch.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "device_identity.h"
void WalkStartCommit(void);
void RtcDayRollover(void);
RuntimeState g_state;
PresentationState g_ui;
Workspace g_work;
static u8 image[65536];
static _Alignas(8) u8 scratch[4096];
static u16 used;
void EepromRead(u16 a, void *b, u16 n)
{
  assert((unsigned)a + n <= sizeof(image));
  memcpy(b, image + a, n);
}
void EepromWrite(u16 a, void *b, u16 n)
{
  assert((unsigned)a + n <= sizeof(image));
  memcpy(image + a, b, n);
}
u8 EepromReadByte(u16 a)
{
  return image[a];
}
u8 EepromWriteByte(u16 a, u8 b)
{
  image[a] = b;
  return 0;
}
void EepromFill(u16 a, u16 n, u8 b)
{
  assert((unsigned)a + n <= sizeof(image));
  memset(image + a, b, n);
}
void StickEepromBatchBegin(void)
{
}
int StickEepromBatchEnd(void)
{
  return 1;
}
void ScratchReset(void)
{
  used = 0;
}
void *ScratchAlloc(u16 n)
{
  void *p = scratch + used;
  assert(used + n <= sizeof(scratch));
  used += n;
  return p;
}

void BeepSetOutputMode(u8 n)
{
  (void)n;
}
void DisplaySetContrast(u8 n)
{
  (void)n;
}
void DiaryAppend(Course *a, DiaryEntry *b, u8 c, u8 d, u16 e, u16 f)
{
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  (void)e;
  (void)f;
}

static u8 last_sent_command;
static unsigned finished;
void WatchdogService(void)
{
}
void LowClockDelay(void)
{
}
u32 RandomNext(void)
{
  return 0;
}
void DisplaySelectBank(u8 b)
{
  (void)b;
}
void StickIrInitPins(void)
{
}
void StickIrConfigure(void)
{
}
void StickIrStart(void)
{
}
void StickIrStop(void)
{
}
void StickIrDelayTicks(u16 n)
{
  (void)n;
}
u16 StickIrTicks(void)
{
  return 1000;
}
void StickIrSendByte(u8 b)
{
  (void)b;
}
void StickIrSendFrame(const u8 *b, u8 n)
{
  (void)n;
  last_sent_command = b[0];
}
int StickIrTakeBurst(u8 *b, u8 n, u8 *l, u16 *t)
{
  (void)b;
  (void)n;
  (void)l;
  (void)t;
  return 0;
}
int StickIrFailed(void)
{
  return 0;
}
void StickEepromDefer(int b)
{
  (void)b;
}
void IrComplete(void)
{
  finished++;
}
void EepromWritePage(u16 a, u8 *b)
{
  EepromWrite(a, b, 128);
}
void BulkDecode(u8 *a, u8 *b)
{
  (void)a;
  (void)b;
  assert(0);
}
u8 *CoursePokemonBuffer(void)
{
  assert(0);
  return 0;
}
u8 StatusLoadReceived(DeviceStatus *s, u8 n)
{
  (void)s;
  (void)n;
  assert(0);
  return 0;
}
void StatusSetReceived(DeviceStatus *s, u8 n)
{
  (void)s;
  (void)n;
  assert(0);
}

uint PacketChecksum(u8 *b, u32 n);
void PeerStoreHistory(void);
void WalkEndClear(void);
static const u8 factoryMac[6] = {2, 1, 2, 3, 4, 5};

static void snapshot(DeviceStatus *s)
{
  EepromMirrorRead(EEPROM_STATUS_PRIMARY, EEPROM_STATUS_BACKUP, (u8 *)s,
                   sizeof(*s));
}

static void boot(void)
{
  StickIdentityRepair(image, factoryMac);
  memset(&g_state, 0, sizeof(g_state));
  memset(&g_ui, 0, sizeof(g_ui));
  memset(&g_work, 0, sizeof(g_work));
  BootRestore();
}

static void stage_walk(u8 protocol)
{
  g_work.irc.statusA.status.peerProtocol = protocol;
  memset(image + EEPROM_COURSE_TEMP, 0, 82 * EEPROM_PAGE_BYTES);
  memset(image + EEPROM_RECORD_TEMP, 0xff, 5 * EEPROM_PAGE_BYTES);
}

static void fresh_walk(u8 protocol)
{
  memset(image, 0xff, sizeof(image));
  boot();
  stage_walk(protocol);
  WalkStartCommit();
}

static void assert_identity(const DeviceStatus *expected)
{
  DeviceStatus actual;
  snapshot(&actual);
  assert(memcmp(expected->deviceId, actual.deviceId, DEVICE_ID_BYTES) == 0);
  assert(memcmp(expected->deviceId, image + EEPROM_ID_PRIMARY,
                DEVICE_ID_BYTES) == 0);
  assert(memcmp(expected->deviceId, image + EEPROM_ID_BACKUP,
                DEVICE_ID_BYTES) == 0);
  assert(memcmp(expected->deviceId,
                image + EEPROM_OWN_RECORDS + offsetof(PeerRecords, deviceId),
                DEVICE_ID_BYTES) == 0);
}

/* Exercise both status roles with packet checksums and protocol dispatch.
 * EEPROM, optical I/O and the completion UI use the test substitutes above. */
static void status_packet(u8 command, const DeviceStatus *remote, u8 result)
{
  const u32 token = 0x12345678;
  memset(&g_work, 0, sizeof(g_work));
  memcpy(IrPayload(), remote, sizeof(*remote));
  u8 *header = g_work.irc.packet;
  header[0] = command;
  header[1] = IR_WALKER_PARAMETER;
  g_work.irc.work.sessionToken = token;
  g_work.irc.work.handshakePhase = IR_PHASE_INITIATOR;
  header[4] = token >> 24;
  header[5] = token >> 16;
  header[6] = token >> 8;
  header[7] = (u8)token;
  uint sum = PacketChecksum(header, sizeof(*remote) + IR_HEADER_BYTES);
  header[2] = sum;
  header[3] = sum >> 8;
  g_state.irReceivedBytes = sizeof(*remote) + IR_HEADER_BYTES;
  g_state.irTimerReference = 900;
  g_state.irResult = IR_RESULT_NONE;
  finished = 0;
  last_sent_command = 0;
  IrProtocolTick();
  assert(g_state.irResult == result);
  assert(finished == (result != IR_RESULT_NONE));
  if (result == IR_RESULT_PEER_ALREADY_SEEN)
    assert(last_sent_command == IR_CMD_PEER_ALREADY_RECORDED);
  else if (result == IR_RESULT_NONE)
    assert(last_sent_command != IR_CMD_PEER_ALREADY_RECORDED);
}

static void both_roles(const DeviceStatus *peer, u8 result)
{
  status_packet(IR_CMD_PEER_STATUS_REQUEST, peer, result);
  status_packet(IR_CMD_PEER_STATUS_REPLY, peer, result);
}

static void remember_peer(const DeviceStatus *peer)
{
  PeerRecords record = {0};
  memcpy(record.deviceId, peer->deviceId, DEVICE_ID_BYTES);
  EepromWrite(EEPROM_PEER_RECORDS, &record, sizeof(record));
  PeerStoreHistory();
}

static void lifecycle(u8 protocol)
{
  DeviceStatus self, peer, other;
  fresh_walk(protocol);
  snapshot(&self);
  assert(self.registered && self.hasPokemon);
  assert(memcmp(self.deviceId, "Stickwalker v1", 13) == 0);
  assert_identity(&self);
  peer = self;
  peer.deviceId[DEVICE_ID_BYTES - 1]++;
  /* Different encoded names do not affect peer eligibility. */
  memset(peer.trainerNameData, 0xa1, sizeof(peer.trainerNameData));
  other = peer;
  other.deviceId[DEVICE_ID_BYTES - 1]++;
  assert(!SeenPeer(peer.deviceId));
  both_roles(&peer, IR_RESULT_NONE);
  remember_peer(&peer);
  assert(SeenPeer(peer.deviceId));
  both_roles(&peer, IR_RESULT_PEER_ALREADY_SEEN);
  both_roles(&other, IR_RESULT_NONE);
  boot();
  assert_identity(&self);
  both_roles(&peer, IR_RESULT_PEER_ALREADY_SEEN);
  RtcDayRollover();
  assert(!SeenPeer(peer.deviceId));
  both_roles(&peer, IR_RESULT_NONE);
  remember_peer(&peer);
  both_roles(&peer, IR_RESULT_PEER_ALREADY_SEEN);
  other.peerProtocol ^= 2;
  both_roles(&other, IR_RESULT_REJECTED);

  WalkEndClear();
  assert_identity(&self);
  stage_walk(protocol);
  WalkStartCommit();
  assert_identity(&self);
  both_roles(&peer, IR_RESULT_NONE);

  /* Replay an old firmware's pending transfer containing an erased ID. */
  stage_walk(protocol);
  u8 marker = WALK_COMMIT_PENDING;
  EepromMirrorWrite(EEPROM_COMMIT_PRIMARY, EEPROM_COMMIT_BACKUP, &marker, 1);
  boot();
  assert_identity(&self);
  EepromMirrorRead(EEPROM_COMMIT_PRIMARY, EEPROM_COMMIT_BACKUP, &marker, 1);
  assert(marker == 0);

  PersistentReset(RESET_CLEAR_EVENTS, RESET_CLEAR_LIFETIME);
  assert_identity(&self);
  boot();
  assert_identity(&self);
}

int main(void)
{
  lifecycle(0);
  lifecycle(2);
  puts("peer first/repeat/other/day rollover/reboot/walk return/reset/pending "
       "transfer: PASS");
  return 0;
}
