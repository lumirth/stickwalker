#ifndef PW_NATIVE_H8_REGISTERS_H
#define PW_NATIVE_H8_REGISTERS_H

#include "../../../upstream/include/startup/iodefine.h"

/* No backing storage: any linked H8 MMIO access is a missing port adaptation. */
extern __typeof__(FLASH) pw_unported_FLASH;
#undef FLASH
#define FLASH pw_unported_FLASH
extern __typeof__(RTC) pw_unported_RTC;
#undef RTC
#define RTC pw_unported_RTC
extern __typeof__(IIC2) pw_unported_IIC2;
#undef IIC2
#define IIC2 pw_unported_IIC2
extern __typeof__(TB1) pw_unported_TB1;
#undef TB1
#define TB1 pw_unported_TB1
extern __typeof__(COMP) pw_unported_COMP;
#undef COMP
#define COMP pw_unported_COMP
extern __typeof__(SSU) pw_unported_SSU;
#undef SSU
#define SSU pw_unported_SSU
extern __typeof__(TW) pw_unported_TW;
#undef TW
#define TW pw_unported_TW
extern __typeof__(AEC) pw_unported_AEC;
#undef AEC
#define AEC pw_unported_AEC
extern __typeof__(SCI3) pw_unported_SCI3;
#undef SCI3
#define SCI3 pw_unported_SCI3
extern __typeof__(WDT) pw_unported_WDT;
#undef WDT
#define WDT pw_unported_WDT
extern __typeof__(AD) pw_unported_AD;
#undef AD
#define AD pw_unported_AD
extern __typeof__(IO) pw_unported_IO;
#undef IO
#define IO pw_unported_IO
extern __typeof__(PFCR) pw_unported_PFCR;
#undef PFCR
#define PFCR pw_unported_PFCR
extern __typeof__(SYSCR1) pw_unported_SYSCR1;
#undef SYSCR1
#define SYSCR1 pw_unported_SYSCR1
extern __typeof__(SYSCR2) pw_unported_SYSCR2;
#undef SYSCR2
#define SYSCR2 pw_unported_SYSCR2
extern __typeof__(IEGR) pw_unported_IEGR;
#undef IEGR
#define IEGR pw_unported_IEGR
extern __typeof__(IENR1) pw_unported_IENR1;
#undef IENR1
#define IENR1 pw_unported_IENR1
extern __typeof__(IENR2) pw_unported_IENR2;
#undef IENR2
#define IENR2 pw_unported_IENR2
extern __typeof__(OSCCR) pw_unported_OSCCR;
#undef OSCCR
#define OSCCR pw_unported_OSCCR
extern __typeof__(IRR1) pw_unported_IRR1;
#undef IRR1
#define IRR1 pw_unported_IRR1
extern __typeof__(IRR2) pw_unported_IRR2;
#undef IRR2
#define IRR2 pw_unported_IRR2
extern __typeof__(CKSTPR1) pw_unported_CKSTPR1;
#undef CKSTPR1
#define CKSTPR1 pw_unported_CKSTPR1
extern __typeof__(CKSTPR2) pw_unported_CKSTPR2;
#undef CKSTPR2
#define CKSTPR2 pw_unported_CKSTPR2

#endif
