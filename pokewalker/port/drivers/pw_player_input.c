#include "port_internal.h"
#include "flags.h"
#include "types.h"
#include "pokewalker_host.h"
#include "project.h"

#include "application/pw_player_input.h"
#include "application/pw_power.h"

#define CENTER_WAKE_HOLD_SCANS 8

/* Clear input state, then mask interrupts while configuring wake inputs. */
void InputInit(void)
{
  g_state.buttons = g_state.previousButtons = g_state.pressedButtons = 0;
  g_state.centerHoldScans = 0;
}

/* Merge the latched center press with sampled button levels, then replace this
 * scan's pressed edges. With buttonWake[0] set, holding center for eight scans
 * starts interactive mode. Only the 16 Hz scan advances this hold counter;
 * immediate edge updates never shorten the wake gesture. */
void InputScan(void)
{
  SystemEvents flags;
  u8 levels = pw_input_levels();

  g_state.buttons = 0;

  if ((levels & BUTTON_CENTER) != 0) {
    g_state.buttons |= BUTTON_CENTER;
    if (g_state.buttonWake[0] != 0 &&
        g_state.centerHoldScans < CENTER_WAKE_HOLD_SCANS) {
      ++g_state.centerHoldScans;
    }
  } else {
    g_state.centerHoldScans = 0;
    if ((g_state.flags.byte & SYSTEM_MODE_MASK) != SYSTEM_MODE_INTERACTIVE) {
      g_state.buttonWake[0] = 0;
    }
  }

  flags.byte = g_state.events.byte;
  if (flags.bits.centerPressed) {
    g_state.buttons |= BUTTON_CENTER;
    g_state.events.byte &= EVENT_CLEAR(EVENT_CENTER_PRESS);
  }

  if ((levels & BUTTON_LEFT)) {
    g_state.buttons |= BUTTON_LEFT;
  }
  if ((levels & BUTTON_RIGHT)) {
    g_state.buttons |= BUTTON_RIGHT;
  }

  g_state.pressedButtons = g_state.buttons & ~g_state.previousButtons;
  g_state.previousButtons = g_state.buttons;

  if (g_state.pressedButtons != 0) {
    g_state.idleSeconds[IDLE_DISPLAY] = INPUT_DISPLAY_SECONDS;
    g_state.sampleIndex = 0;
    if ((g_state.flags.byte & SYSTEM_MODE_MASK) != SYSTEM_MODE_INTERACTIVE) {
      g_state.pressedButtons = 0;
    }
  }

  if ((g_state.centerHoldScans >= CENTER_WAKE_HOLD_SCANS) &&
      ((g_state.flags.byte & SYSTEM_MODE_MASK) != SYSTEM_MODE_INTERACTIVE)) {
    MotionSessionStart();
    g_state.flags.byte |= SYSTEM_SOCIAL_OFFER;
  }
}

u8 InputPressed(u8 requestedMask)
{
  return (requestedMask & g_state.pressedButtons);
}
