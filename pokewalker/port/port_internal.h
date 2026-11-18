#ifndef POKEWALKER_PORT_INTERNAL_H
#define POKEWALKER_PORT_INTERNAL_H

#include "types.h"
#include <stdbool.h>
#include <stdint.h>

void BeepTick(void);
void StatusRestoreFlags(void);

u8 pw_input_levels(void);
bool pw_acceleration(s8 xyz[3]);
void pw_clock_set_anchor(void);
void pw_checkpoint(void);
void pw_restore_runtime(void);
uint64_t pw_audio_tick(uint64_t now_us);
bool pw_eeprom_dirty(void);
void pw_eeprom_mark_clean(void);

#endif
