#include "types.h"
#include "project.h"
#include "application/pw_buzzer.h"
#include "application/pw_nt7508.h"
#include "application/pw_power.h"

/* Host scheduling replaces H8 clock gates and interrupt acknowledgement. */
void pw_port_MotionSessionWake(void)
{
  g_state.flags.byte =
      ((g_state.flags.byte & SYSTEM_MODE_CLEAR) | SYSTEM_MODE_MOTION);

  g_state.idleSeconds[IDLE_MOTION] = ACTIVITY_MOTION_SECONDS;
  g_state.events.byte &= EVENT_CLEAR(EVENT_MOTION);
}
extern __typeof__(pw_port_MotionSessionWake) MotionSessionWake
  __attribute__((alias("pw_port_MotionSessionWake")));

void pw_port_MotionSessionStart(void)
{
  g_state.idleSeconds[IDLE_DISPLAY] = INTERACTIVE_DISPLAY_SECONDS;
  g_state.idleSeconds[IDLE_MOTION] = INTERACTIVE_MOTION_SECONDS;
  if ((g_state.flags.byte & SYSTEM_MODE_MASK) != SYSTEM_MODE_INTERACTIVE) {
    if ((g_state.flags.byte & SYSTEM_MODE_MASK) == SYSTEM_MODE_INACTIVE) {
      g_state.sampleIndex = 0;
    }
    g_state.flags.byte =
        ((g_state.flags.byte & SYSTEM_MODE_CLEAR) | SYSTEM_MODE_INTERACTIVE);

    DisplayExitPowerSave();
  }
}
extern __typeof__(pw_port_MotionSessionStart) MotionSessionStart
  __attribute__((alias("pw_port_MotionSessionStart")));

void pw_port_MotionSessionEnd(void)
{
  BeepDisableTimer();

  g_state.flags.byte &= SYSTEM_MODE_CLEAR;
}
extern __typeof__(pw_port_MotionSessionEnd) MotionSessionEnd
  __attribute__((alias("pw_port_MotionSessionEnd")));

void pw_port_IRQ0Interrupt(void)
{
  g_state.events.byte |= EVENT_CENTER_PRESS;
  g_state.buttonWake[0] = 1;
}
extern __typeof__(pw_port_IRQ0Interrupt) IRQ0Interrupt
  __attribute__((alias("pw_port_IRQ0Interrupt")));
