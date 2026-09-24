#ifndef PW_STICK_EEPROM_BACKEND_H
#define PW_STICK_EEPROM_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

/* Mount checked image slots and replay the private EEPROM write journal.
 * Never format a nonblank filesystem. */
int StickEepromMount(void);
/* Defer flash writes during the source-faithful optical session. */
void StickEepromDefer(int enabled);
int StickEepromCommit(void);
/* Persist a source EEPROM mirror write as one atomic image update. */
void StickEepromBatchBegin(void);
int StickEepromBatchEnd(void);
#ifdef PW_STICK_BENCH_CONTROL
/* Read-only inspection of the committed image for independent verification. */
const unsigned char *StickEepromSnapshot(void);
int StickEepromBenchErase(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
