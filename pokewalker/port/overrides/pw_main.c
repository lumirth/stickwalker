#include "port_internal.h"
#include "project.h"
#include "pokewalker_host.h"
#include "application/pw_battery.h"
#include "application/pw_buzzer.h"
#include "application/pw_friend.h"
#include "application/pw_follower_prompts.h"
#include "application/pw_home.h"
#include "application/pw_main.h"
#include "application/pw_nt7508.h"
#include "application/pw_pedometer.h"
#include "application/pw_player_input.h"
#include "application/pw_power.h"
#include "application/pw_rtc.h"
#include "support/ir.h"
#include "support/lib_common.h"

void ViewUpdate(void);
void ViewRender(void);

#define PW_ACCEL_RING_MASK 0x3f
#define PW_ACCEL_RING_LAST 0x3f
#define PW_ABS(value) ((value) >= 0 ? (value) : -(value))

static bool CaptureSample(void)
{
  u8 previous;

  s8 xyz[3];
  if (!pw_acceleration(xyz)) {
    g_state.sampleIndex = 0;
    return false;
  }
  g_work.motion.x[g_state.sampleIndex] = xyz[0];
  g_work.motion.y[g_state.sampleIndex] = xyz[1];
  g_work.motion.z[g_state.sampleIndex] = xyz[2];
  if (g_state.view == VIEW_THRESHOLD_TEST) {
    previous = ((g_state.sampleIndex + 0x3f) & 0x3f);
    if (g_state.sampleIndex == 0) {
      g_ui.view.accel.xActivity = 0;
      g_ui.view.accel.yActivity = 0;
      g_ui.view.accel.zActivity = 0;
    }
    g_ui.view.accel.xActivity += PW_ABS(g_work.motion.x[g_state.sampleIndex] -
                                        g_work.motion.x[previous]);
    g_ui.view.accel.yActivity += PW_ABS(g_work.motion.y[g_state.sampleIndex] -
                                        g_work.motion.y[previous]);
    g_ui.view.accel.zActivity += PW_ABS(g_work.motion.z[g_state.sampleIndex] -
                                        g_work.motion.z[previous]);
  }
  return true;
}

void pw_port_MainTick(void)
{
  bool sample_available = CaptureSample();
  InputScan();
  if ((g_state.flags.byte & SYSTEM_MODE_MASK) == SYSTEM_MODE_INACTIVE) {
    RtcDispatch();
    if ((sample_available && MotionActivityCheck() != 0) ||
        (g_state.events.bits.centerPressed != 0)) {
      MotionSessionWake();
    }
  } else {
    if ((g_state.flags.byte & SYSTEM_MODE_MASK) == SYSTEM_MODE_INTERACTIVE) {
      SocialOfferCheck();
      ViewUpdate();
      /* After the view starts IR, advance sampleIndex and return. IR owns the
       * motion and scratch storage from this point. */
      if (g_task == IrProtocolTick) {
        goto tickTail;
      }
    }
    if (sample_available && (g_state.flags.bits.registered != 0) &&
        (g_state.sampleIndex == PW_ACCEL_RING_LAST)) {
      MotionProcess();
    } else {
      if (g_state.events.bits.uiRefreshPending != 0) {
        if ((g_state.flags.byte & SYSTEM_MODE_MASK) ==
            SYSTEM_MODE_INTERACTIVE) {
          DisplayClear(0x40);
          ViewRender();
          DisplayToggleBank();
          g_state.uiFrame++;
        }
        g_state.events.bits.uiRefreshPending = 0;
      } else {
        RtcDispatch();
        if ((g_state.flags.byte & SYSTEM_MODE_MASK) ==
            SYSTEM_MODE_INTERACTIVE) {
          if (g_state.idleSeconds[IDLE_DISPLAY] == 0) {
            DisplayEnterPowerSave();
            g_state.flags.byte =
                ((g_state.flags.byte & SYSTEM_MODE_CLEAR) | SYSTEM_MODE_MOTION);
            g_state.buttonWake[0] = 0;
            g_state.centerHoldScans = 0;
          }
        } else {
          BatteryUpdate();
          MotionSessionIdleCheck();
        }
      }
    }
    StepPacingTick();
    if (BeepHasScore() != 0) {
      InstallTask(BeepTick);
      BeepEnableTimer();
    }
  }
tickTail:
  if (sample_available) {
    g_state.sampleIndex = ((g_state.sampleIndex + 1) & PW_ACCEL_RING_MASK);
  }
}
extern __typeof__(pw_port_MainTick) MainTick
  __attribute__((alias("pw_port_MainTick")));

void pw_port_BeepTick(void)
{
  g_state.events.bits.lowPowerClock = 1;
  InputScan();
  ViewUpdate();
  if (g_state.events.bits.uiRefreshPending != 0) {
    if ((g_state.flags.byte & SYSTEM_MODE_MASK) == SYSTEM_MODE_INTERACTIVE) {
      DisplayClear(0x40);
      ViewRender();
      DisplayToggleBank();
      g_state.uiFrame++;
    }
    g_state.events.bits.uiRefreshPending = 0;
  }
  if (BeepHasScore() == 0) {
    BeepDisableTimer();
    InstallTask(MainTick);
    g_state.sampleIndex = 0;
  }
}
extern __typeof__(pw_port_BeepTick) BeepTick
  __attribute__((alias("pw_port_BeepTick")));
