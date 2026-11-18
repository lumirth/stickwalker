#include <string.h>
#include "port_internal.h"
#include "project.h"
#include "pokewalker_host.h"
#include "record_io.h"
#include "application/pw_buzzer.h"
#include "application/pw_eeprom_m95512.h"
#include "application/pw_home.h"
#include "application/pw_main.h"
#include "application/pw_nt7508.h"
#include "application/pw_pedometer.h"
#include "application/pw_player_input.h"
#include "application/pw_power.h"
#include "application/pw_rtc.h"
#include "application/pw_selftest.h"
#include "application/pw_storage.h"
#include "startup/h8_resetprg.h"
#include "startup/h8_rominfo.h"
#include "support/lib_common.h"

#define PW_PRNG_SEED_CROSS_RECORD 0x153
void pw_port_PowerOnReset(void)
{
  u8 id[40];
  u8 seed_bytes[4];
  memset(&g_state, 0, sizeof g_state);
  memset(&g_ui, 0, sizeof g_ui);
  memset(&g_work, 0, sizeof g_work);
  g_task = MainTick;
  g_state.flags.byte = SYSTEM_MODE_INTERACTIVE;
  g_state.events.byte = EVENT_UI_REFRESH;
  g_state.idleSeconds[IDLE_DISPLAY] = INTERACTIVE_DISPLAY_SECONDS;
  g_state.idleSeconds[IDLE_MOTION] = INTERACTIVE_MOTION_SECONDS;
  g_state.socialElapsedSeconds = 3600;
  MotionReset();
  if (!BootSignatureValid()) {
    /* Production walkers receive an identity during manufacture. Native board
     * identity replaces that fixture step; game resources still arrive from DS.
     */
    DeviceStatus initial;
    memset(&initial, 0, sizeof initial);
    pw_host_device_id(id);
    EepromMirrorWrite(EEPROM_ID_PRIMARY, EEPROM_ID_BACKUP, id, sizeof id);
    memcpy(initial.deviceId, id, sizeof id);
    initial.firmwareRevision = FIRMWARE_REVISION;
    EepromMirrorWrite(EEPROM_STATUS_PRIMARY, EEPROM_STATUS_BACKUP,
                      (u8 *)&initial, sizeof initial);
  }
  BootRestore();
  BeepInit();
  BeepSetOutputMode(g_state.save.volume);
  DisplayInit();
  RtcSetTime(g_state.save.rtcSeconds);
  pw_eeprom_read_raw(PW_PRNG_SEED_CROSS_RECORD, seed_bytes, 4);
  RandomSeed(pw_read_be32(seed_bytes));
  InputInit();
  InstallTask(MainTick);
  HomeInit();
  g_state.view = VIEW_HOME;
}
extern __typeof__(pw_port_PowerOnReset) PowerOnReset
  __attribute__((alias("pw_port_PowerOnReset")));

void pw_port_CommitStagedWalk(void)
{
  u8 page[EEPROM_PAGE_BYTES];
  for (unsigned i = 0; i < 82; ++i) {
    pw_eeprom_read_raw(EEPROM_COURSE_TEMP + i * sizeof(page), page, sizeof(page));
    pw_eeprom_write_raw(EEPROM_COURSE + i * sizeof(page), page, sizeof(page));
  }
  for (unsigned i = 0; i < 5; ++i) {
    pw_eeprom_read_raw(EEPROM_RECORD_TEMP + i * sizeof(page), page, sizeof(page));
    pw_eeprom_write_raw(EEPROM_OWN_RECORDS + i * sizeof(page), page, sizeof(page));
  }
}
extern __typeof__(pw_port_CommitStagedWalk) CommitStagedWalk
  __attribute__((alias("pw_port_CommitStagedWalk")));
