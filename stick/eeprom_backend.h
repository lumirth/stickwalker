#ifndef PW_STICK_EEPROM_BACKEND_H
#define PW_STICK_EEPROM_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

int StickEepromMount(void);
void StickEepromDefer(int enabled);
int StickEepromCommit(void);
int StickEepromSessionBegin(void);
int StickEepromSessionEnd(void);
int StickEepromHealthy(void);
void StickEepromBatchBegin(void);
int StickEepromBatchEnd(void);
#ifdef PW_STICK_BENCH_CONTROL
/* Inspect the saved image for bench readback checks. */
const unsigned char *StickEepromSnapshot(void);
int StickEepromBenchErase(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
