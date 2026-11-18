#include "port_internal.h"
#include "project.h"
#include "types.h"
#include "eeprom_address.h"
#include "pokewalker_host.h"
#include <stdbool.h>
#include "application/pw_buzzer.h"
#include "application/pw_builtin.h"
#include "application/pw_eeprom_m95512.h"
#include "application/pw_main.h"

typedef struct {
  u16 offsetLe;
  u8 byteLength;
  u8 checksum;
} SoundEntry;

_Static_assert(sizeof(SoundEntry) == 4, "Sound directory entry layout");

enum { SOUND_DIRECTORY_ENTRIES = 16, SOUND_STORAGE_BYTES = 480 };

typedef struct {
  SoundEntry entries[SOUND_DIRECTORY_ENTRIES];
  u8 scores[SOUND_STORAGE_BYTES];
} SoundArchive;

typedef char SoundArchiveData[offsetof(SoundArchive, scores) ==
                                      EEPROM_SOUND_DATA - EEPROM_SOUND_DIRECTORY
                                  ? 1
                                  : -1];
typedef char SoundArchiveSize[sizeof(SoundArchive) == 544 ? 1 : -1];

u8 BeepHasScore(void)
{
  if (g_note == 0) {
    return 0;
  }
  return 1;
}

static u16 native_compare;
static bool native_pwm, native_beep_enabled;
static uint64_t native_next_period;
static const Note *native_score_start;
static bool native_first_note;
static uint64_t native_output_ready_us;

static uint64_t watch_ticks(uint64_t time_us)
{
  return time_us / 1000000 * 32768 + time_us % 1000000 * 32768 / 1000000;
}

static uint64_t watch_time_us(uint64_t ticks)
{
  return ticks / 32768 * 1000000 +
         (ticks % 32768 * 1000000 + 32767) / 32768;
}

static void restart_score(void)
{
  native_score_start = g_note;
  native_first_note = true;
  g_ui.periodsRemaining = 0;
  g_ui.separatorPeriodsRemaining = 0;
  if (native_beep_enabled || g_task == BeepTick) {
    native_beep_enabled = true;
    native_compare = 0;
    native_pwm = false;
    native_next_period = watch_ticks(pw_host_now_us()) + 1;
    pw_host_tone(0, g_ui.outputMode);
  }
}

void BeepInit(void)
{
  g_ui.durationDivisor = 120;
  g_ui.outputMode = 0;
  g_note = 0;
  native_compare = 0;
  native_pwm = false;
  native_beep_enabled = false;
  native_score_start = NULL;
  native_first_note = false;
}

#define BEEPER_SEQUENCE_RAM g_work.beeper.score
#define BEEPER_SEQUENCE_MAX_BYTES sizeof(g_work.beeper.score)
#define BEEPER_CONTROL_VALUE_MASK 0x7f
#define BEEPER_CONTROL_RESTART_MIN 0x7e

/* Load an EEPROM score into the motion sample RAM. Check the byte sum of
 * complete two-byte notes against the descriptor checksum and require the
 * last complete note's pitch to repeat or end. An oversized descriptor keeps
 * the previous score selected. */
void BeepLoadScore(u8 sequenceId)
{
  SoundEntry descriptor;
  u16 location;
  u8 id;
  u8 noteIndex;
  u8 checksum;
  u16 directoryEntry;

  if (g_ui.outputMode == 0) {
    return;
  }

  id = sequenceId;
  /* Calculate the selected descriptor's serial EEPROM address. */
  directoryEntry = EEPROM_SOUND_DIRECTORY;
  directoryEntry += (id) * sizeof(SoundEntry);
  EepromRead((u16)directoryEntry, &descriptor, sizeof(SoundEntry));
  /* Directory offsets and native integers are both little-endian. */
  location = descriptor.offsetLe;
  location = (location + PW_EEPROM_MEMBER_ADDRESS(EEPROM_SOUND_DIRECTORY,
                                                  SoundArchive, scores[0]));
  if (descriptor.byteLength >= 2 && !(descriptor.byteLength & 1) &&
      descriptor.byteLength <= BEEPER_SEQUENCE_MAX_BYTES) {
    g_note = BEEPER_SEQUENCE_RAM;
    EepromRead(location, BEEPER_SEQUENCE_RAM, descriptor.byteLength);
    noteIndex = 0;
    checksum = 0;
    while (noteIndex < ((u16)descriptor.byteLength >> 1)) {
      checksum = (checksum + g_note[noteIndex].duration);
      checksum = (checksum + g_note[noteIndex].pitch);
      noteIndex++;
    }
    if ((checksum != descriptor.checksum) ||
        ((((u8 *)g_note)[(((u16)descriptor.byteLength >> 1) << 1) - 1] &
          BEEPER_CONTROL_VALUE_MASK) < BEEPER_CONTROL_RESTART_MIN)) {
      g_note = 0;
    } else {
      restart_score();
    }
  }
}

void BeepSelectScore(const Note *score)
{
  g_note = score;
  restart_score();
}

/* The first score command runs after one 32,768 Hz tick. The speaker produces
 * the waveform continuously; foreground deadlines represent note boundaries. */
void BeepEnableTimer(void)
{
  native_beep_enabled = true;
  native_pwm = false;
  native_compare = 0;
  native_next_period = watch_ticks(pw_host_now_us()) + 1;
  pw_host_tone(0, g_ui.outputMode);
}

void BeepDisableTimer(void)
{
  native_beep_enabled = false;
  native_pwm = false;
  pw_host_tone(0, g_ui.outputMode);
}

void BeepSetOutputMode(u8 mode)
{
  g_ui.outputMode = mode;
  if (mode == 0) {
    g_note = NULL;
    BeepDisableTimer();
  } else if (native_pwm) {
    if (pw_host_tone(32768000u / (native_compare + 1), mode) == UINT64_MAX) {
      g_note = NULL;
      native_pwm = false;
    }
  }
}

#define BEEPER_SEPARATOR_COMPARE 0x140
/* At 32768 watch clocks/s, 60/24 converts score duration units and tempo
 * into a clock budget. */
#define BEEPER_DURATION_CLOCK_SCALE 0x14000ul

/* The original timer period lasts compare+1 watch clocks. The host generates
 * that pitch continuously and maps the two output modes to speaker gain. */
static bool set_period(u8 compareValue)
{
  native_compare = compareValue;
  native_output_ready_us = pw_host_tone(
      native_pwm && compareValue ? 32768000u / (compareValue + 1) : 0,
      g_ui.outputMode);
  if (native_output_ready_us == UINT64_MAX) {
    g_note = NULL;
    native_pwm = false;
    return false;
  }
  return true;
}

/* Called per compare-A interrupt. g_note points to the next record while the
 * preceding note's period count runs down. A non-legato note schedules one
 * separator period before the next record is interpreted. */
static void advance_score(void)
{
  const u8 *periodTable;

  if (g_note == 0) {
    return;
  }
  if (g_ui.periodsRemaining != 0) {
    g_ui.periodsRemaining--;
    if ((g_ui.periodsRemaining == 1) &&
        ((g_note->pitch & NOTE_PITCH_MASK) == NOTE_END)) {
      native_pwm = false;
      pw_host_tone(0, g_ui.outputMode);
    }
    if (g_ui.periodsRemaining != 0) {
      return;
    }
  }
  if (g_ui.separatorPeriodsRemaining != 0) {
    native_compare = BEEPER_SEPARATOR_COMPARE;
    native_pwm = false;
    pw_host_tone(0, g_ui.outputMode);

    if (g_ui.separatorPeriodsRemaining != 0) {
      g_ui.separatorPeriodsRemaining--;
    }
    return;
  }
  if ((g_note->pitch & NOTE_PITCH_MASK) == NOTE_END) {
    g_note = 0;
    return;
  }
  /* Consume at most one tempo command. A repeat selects the shared RAM score
   * regardless of the current score's storage. */
  if ((g_note->pitch & NOTE_PITCH_MASK) == NOTE_SET_TEMPO) {
    if (g_note->duration == 0) {
      g_note = 0;
      return;
    }
    g_ui.durationDivisor = g_note->duration;
    g_note++;
    if ((g_note->pitch & NOTE_PITCH_MASK) == NOTE_END) {
      g_note = 0;
      return;
    }
  }
  if ((g_note->pitch & NOTE_PITCH_MASK) == NOTE_REPEAT) {
    g_note = BEEPER_SEQUENCE_RAM;
    return;
  }
  /* Narrow the duration's clock budget to 16 bits, then divide by the raw
   * compare byte. Rest and other pitch indices read adjacent resident artwork
   * when they extend beyond the pitch table. */
  periodTable = g_residentResources.bytes;
  if ((g_note->pitch & NOTE_PITCH_MASK) == NOTE_REST) {
    g_ui.periodsRemaining =
        ((u16)((BEEPER_DURATION_CLOCK_SCALE * g_note->duration) /
               g_ui.durationDivisor) /
         periodTable[g_note->pitch & NOTE_PITCH_MASK]);
    if (!set_period(0)) {
      return;
    }
  } else {
    if ((g_note->pitch & NOTE_LEGATO) != 0) {
      g_ui.periodsRemaining =
          ((u16)((BEEPER_DURATION_CLOCK_SCALE * g_note->duration) /
                 g_ui.durationDivisor) /
           periodTable[g_note->pitch & NOTE_PITCH_MASK]);
      g_ui.separatorPeriodsRemaining = 0;
    } else {
      g_ui.periodsRemaining =
          ((u16)(((BEEPER_DURATION_CLOCK_SCALE * g_note->duration) /
                  g_ui.durationDivisor) -
                 BEEPER_SEPARATOR_COMPARE) /
           periodTable[g_note->pitch & NOTE_PITCH_MASK]);
      g_ui.separatorPeriodsRemaining = 1;
    }
    /* The preceding record's legato flag controls whether pitch is changed. */
    if (g_note == BEEPER_SEQUENCE_RAM || g_note == native_score_start ||
        (g_note[-1].pitch & NOTE_LEGATO) == 0) {
      native_pwm = true;

      if (!set_period(g_residentResources.bytes[g_note->pitch & NOTE_PITCH_MASK])) {
        return;
      }
    }
  }
  g_note++;
}

static unsigned periods_until_change(void)
{
  unsigned periods = g_ui.periodsRemaining;
  if (periods > 1 && (g_note->pitch & NOTE_PITCH_MASK) == NOTE_END) {
    return periods - 1;
  }
  return periods ? periods : 1;
}

uint64_t pw_audio_tick(uint64_t now_us)
{
  uint64_t now_ticks = watch_ticks(now_us);
  unsigned transitions = 0;
  while (native_beep_enabled && g_note && transitions++ < 64) {
    unsigned periods = periods_until_change();
    uint64_t boundary = native_next_period +
                        (uint64_t)(periods - 1) * (native_compare + 1);
    if (now_ticks < boundary) {
      return watch_time_us(boundary);
    }
    /* Skip timer interrupts whose only effect is decrementing the same note.
     * Keep the original final-period mute, separator, tempo and legato rules. */
    if (g_ui.periodsRemaining >= periods) {
      g_ui.periodsRemaining -= periods - 1;
    }
    advance_score();
    uint64_t now_after_output = pw_host_now_us();
    if (g_note && g_ui.periodsRemaining != 0 &&
        (native_first_note || native_output_ready_us > now_after_output)) {
      uint64_t started = watch_ticks(native_output_ready_us > now_after_output
                                        ? native_output_ready_us
                                        : now_after_output);
      if (started > boundary) {
        boundary = started;
      }
      native_first_note = false;
    }
    native_next_period = boundary + native_compare + 1;
  }
  if (!g_note) {
    BeepDisableTimer();
    return UINT64_MAX;
  }
  return native_beep_enabled ? watch_time_us(native_next_period) : UINT64_MAX;
}
