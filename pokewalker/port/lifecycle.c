#include "port_internal.h"
#include "pokewalker.h"
#include "pokewalker_host.h"
#include "project.h"
#include "application/pw_eeprom_m95512.h"
#include "application/pw_player_input.h"
#include "application/pw_pedometer.h"
#include "application/pw_nt7508.h"
#include "application/pw_power.h"
#include "application/pw_main.h"
#include "application/pw_rtc.h"
#include "support/ir.h"
#include <string.h>

extern void PowerOnReset(void);
extern void IRQ0Interrupt(void);

static uint64_t next_sample;
static uint64_t next_frame;
static uint64_t last_second;
static uint32_t saved_steps;
static uint64_t save_retry_at;
static u8 previous_levels;
static u8 input_levels;
static u8 input_pending;
static PwMotionMode previous_motion;
static s8 acceleration[3];
static bool acceleration_available;

bool pw_acceleration(s8 xyz[3])
{
  memcpy(xyz, acceleration, sizeof(acceleration));
  return acceleration_available;
}

static bool pocket_sampling(void)
{
  return g_task == MainTick &&
      (g_state.flags.byte & SYSTEM_MODE_MASK) != SYSTEM_MODE_INTERACTIVE &&
      !g_state.buttonWake[0];
}

u8 pw_input_levels(void)
{
  u8 result = input_levels | input_pending;
  input_pending = 0;
  return result;
}

/* A time supplied by the game becomes authoritative when accepted. Do not add
 * the portion of the current IR exchange before that synchronization again. */
void pw_clock_set_anchor(void)
{
  last_second = pw_host_calendar_us();
  next_frame = pw_host_now_us() + 250000;
}

void pw_checkpoint(void)
{
  PwRuntimeState state = {
      .calendar_us = last_second,
      .daily_steps = g_state.dailySteps,
      .hour_steps = g_state.hourSteps,
      .social_elapsed_seconds = g_state.socialElapsedSeconds,
      .random_state = g_state.randomState,
      .rollover_hour_bcd = g_state.rolloverHourBcd,
      .present = 1,
  };
  _Static_assert(sizeof(state.save) == sizeof(g_state.save), "Retained save size");
  memcpy(state.save, &g_state.save, sizeof(g_state.save));
  pw_host_runtime_checkpoint(&state);
}

bool pw_save(void)
{
  EepromMirrorWrite(EEPROM_SAVE_PRIMARY, EEPROM_SAVE_BACKUP,
                    (u8 *)&g_state.save, sizeof(g_state.save));
  pw_checkpoint();
  if (!pw_host_commit()) {
    return false;
  }
  pw_eeprom_mark_clean();
  saved_steps = g_state.save.totalSteps;
  return true;
}

bool pw_save_changes(void)
{
  pw_checkpoint();
  if (!pw_eeprom_dirty()) {
    return true;
  }
  return pw_save();
}

bool pw_sound_playing(void)
{
  return g_task == BeepTick;
}

bool pw_sound_enabled(void)
{
  return g_ui.outputMode != 0;
}

bool pw_connection_screen(void)
{
  return pw_communicating() || g_state.view == VIEW_IR_RESULT ||
         g_state.view == VIEW_WALK_START || g_state.view == VIEW_WALK_END ||
         g_state.view == VIEW_PEER || g_state.view == VIEW_EVENT_REWARD;
}

bool pw_communicating(void)
{
  return g_task == IrProtocolTick;
}

void pw_wake_screen(void)
{
  if ((g_state.flags.byte & SYSTEM_MODE_MASK) == SYSTEM_MODE_INACTIVE) {
    next_sample = pw_host_now_us();
  }
  MotionSessionStart();
  g_state.flags.byte |= SYSTEM_SOCIAL_OFFER;
}

void pw_keep_awake(void)
{
  MotionSessionStart();
}

void pw_sleep_screen(void)
{
  DisplayEnterPowerSave();
  if ((g_state.flags.byte & SYSTEM_MODE_MASK) == SYSTEM_MODE_INTERACTIVE) {
    g_state.flags.byte = (g_state.flags.byte & SYSTEM_MODE_CLEAR) | SYSTEM_MODE_MOTION;
  }
  g_state.idleSeconds[IDLE_DISPLAY] = 0;
  g_state.buttonWake[0] = 0;
  g_state.centerHoldScans = 0;
}

static bool apply_elapsed_time(uint64_t calendar_us)
{
  if (calendar_us < last_second || calendar_us - last_second < 1000000) {
    return false;
  }
  uint64_t elapsed = (calendar_us - last_second) / 1000000;
  if (elapsed > UINT32_MAX) {
    elapsed = UINT32_MAX;
  }
  last_second += elapsed * 1000000;
  pw_elapsed((u32)elapsed);
  return true;
}

void pw_restore_runtime(void)
{
  PwRuntimeState state;
  if (pw_host_runtime_load(&state)) {
    g_state.dailySteps = state.daily_steps;
    g_state.hourSteps = state.hour_steps;
    g_state.socialElapsedSeconds = state.social_elapsed_seconds;
    g_state.randomState = state.random_state;
    g_state.rolloverHourBcd = state.rollover_hour_bcd;
    memcpy(&g_state.save, state.save, sizeof(g_state.save));
    RtcSetTime(g_state.save.rtcSeconds);
    last_second = state.calendar_us;
  }
}

void pw_init(void)
{
  pw_eeprom_mark_clean();
  PowerOnReset();
  pw_restore_runtime();
  saved_steps = g_state.save.totalSteps;
  uint64_t calendar_us = pw_host_calendar_us();
  if (last_second > calendar_us) {
    last_second = calendar_us;
  }
  apply_elapsed_time(calendar_us);

  g_state.flags.byte =
      (g_state.flags.byte & SYSTEM_MODE_CLEAR) | SYSTEM_MODE_INTERACTIVE;
  g_state.idleSeconds[IDLE_DISPLAY] = INTERACTIVE_DISPLAY_SECONDS;
  g_state.idleSeconds[IDLE_MOTION] = INTERACTIVE_MOTION_SECONDS;
  uint64_t now = pw_host_now_us();
  next_sample = now;
  next_frame = now;
  previous_motion = PW_MOTION_ACTIVE;
  previous_levels = input_levels = input_pending = 0;
  if (pw_eeprom_dirty() && !pw_save()) {
    pw_host_fault("State persistence failed");
  }
}

uint64_t pw_tick(void)
{
  uint64_t now = pw_host_now_us();
  if (pw_communicating()) {
    IrProtocolTick();
    return now;
  }
  uint64_t calendar_now = pw_host_calendar_us();
  bool replay = pocket_sampling() &&
      (g_state.flags.byte & SYSTEM_MODE_MASK) != SYSTEM_MODE_INACTIVE;
  if (replay) {
    uint64_t available = pw_host_acceleration_until();
    if (available < now) {
      calendar_now -= now - available;
      now = available;
    }
  }

  u8 levels = pw_host_buttons();
  u8 edges = levels & ~previous_levels;
  input_pending |= edges;
  input_levels = levels;
  if (edges & BUTTON_CENTER) {
    IRQ0Interrupt();
    /* Inactive wake changes the scan rate from 1 Hz to 16 Hz. Interactive
     * input leaves the motion sample deadline alone. */
    if ((g_state.flags.byte & SYSTEM_MODE_MASK) == SYSTEM_MODE_INACTIVE &&
        g_task == MainTick && next_sample > now) {
      next_sample = now;
    }
  }
  previous_levels = levels;
  // Live input coalesces late scans. Pocket FIFO samples retain their times;
  // replay them through MainTick with calendar events in the same order.
  if (!replay && now >= next_sample && now - next_sample > 125000) {
    g_state.sampleIndex = 0;
    next_sample = now;
  }
  unsigned ticks = 0;
  while (now >= next_sample && ticks++ < 96) {
    uint64_t age = now - next_sample;
    apply_elapsed_time(calendar_now > age ? calendar_now - age : 0);
    if (next_sample >= next_frame) {
      if ((g_state.flags.byte & SYSTEM_MODE_MASK) != SYSTEM_MODE_INACTIVE ||
          g_state.buttonWake[0]) {
        g_state.events.byte |= EVENT_UI_REFRESH;
      }
      next_frame += ((next_sample - next_frame) / 250000 + 1) * 250000;
    }
    if (g_task == BeepTick) {
      BeepTick();
    } else {
      acceleration_available = pw_host_acceleration(next_sample, acceleration);
      MainTick();
    }
    bool inactive =
        (g_state.flags.byte & SYSTEM_MODE_MASK) == SYSTEM_MODE_INACTIVE &&
        !g_state.buttonWake[0];
    uint64_t period = inactive ? 1000000 : 62500;
    next_sample += period;
    if (pw_communicating()) return pw_host_now_us();
    if (!replay || !pocket_sampling() || !acceleration_available) {
      if (next_sample <= now) next_sample += ((now - next_sample) / period + 1) * period;
      break;
    }
  }
  apply_elapsed_time(calendar_now);
  uint64_t next_note = g_task == BeepTick
                           ? pw_audio_tick(pw_host_now_us())
                           : UINT64_MAX;
  if (pw_communicating()) {
    return pw_host_now_us();
  }

  /* Flash suspends the mixer and this foreground score clock. Defer ordinary
   * writes until playback ends, including a mute or failed output; keep the
   * BeepTick workspace handoff on its original sample clock. Explicit shutdown
   * and protocol saves still call pw_save() synchronously. */
  PwMotionMode motion = pw_motion_mode();
  bool walked = g_state.save.totalSteps != saved_steps;
  bool stopped = walked && motion == PW_MOTION_IDLE && previous_motion != PW_MOTION_IDLE;
  if (next_note == UINT64_MAX && now >= save_retry_at &&
      (pw_eeprom_dirty() || stopped || g_state.save.totalSteps - saved_steps >= 256)) {
    save_retry_at = pw_save() ? 0 : pw_host_now_us() + 30000000;
  }
  previous_motion = motion;
  pw_checkpoint();
  uint64_t deadline = next_sample;
  if (pocket_sampling()) {
    bool inactive = (g_state.flags.byte & SYSTEM_MODE_MASK) == SYSTEM_MODE_INACTIVE;
    if (!inactive) deadline = next_sample + (63u - g_state.sampleIndex) * 62500ULL;
  } else if (next_frame < deadline) {
    deadline = next_frame;
  }
  return next_note < deadline ? next_note : deadline;
}

PwMotionMode pw_motion_mode(void)
{
  if (pw_communicating() || g_task == BeepTick) {
    return PW_MOTION_PAUSED;
  }
  if ((g_state.flags.byte & SYSTEM_MODE_MASK) == SYSTEM_MODE_INACTIVE &&
      !g_state.buttonWake[0]) {
    return PW_MOTION_IDLE;
  }
  return PW_MOTION_ACTIVE;
}

bool pw_clock_time(uint8_t *hour, uint8_t *minute)
{
  if (!g_state.flags.bits.registered) {
    return false;
  }
  *hour = (g_state.time.hourBcd24h >> 4) * 10 +
          (g_state.time.hourBcd24h & 0x0f);
  *minute = (g_state.time.minuteBcd >> 4) * 10 +
            (g_state.time.minuteBcd & 0x0f);
  return true;
}
