#ifndef PW_STICK_EEPROM_BACKEND_H
#define PW_STICK_EEPROM_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

/* Mount a private 64 KiB M95512 image. Never format a nonblank filesystem. */
int StickEepromMount(void);
/* Defer flash writes during the source-faithful optical session. */
void StickEepromDefer(int enabled);
int StickEepromCommit(void);

#ifdef __cplusplus
}
#endif

#endif
