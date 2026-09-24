#ifndef PW_STICK_DISPLAY_BUS_H
#define PW_STICK_DISPLAY_BUS_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Only the H8 register fields the original NT7508 drawing module uses. The
 * assignments preserve its command/data mode and ready polling; bytes are
 * handed to the virtual NT7508 by StickDisplayWrite. */
typedef struct {
  struct { u8 B0, B1; } BIT;
} StickPdr1;
typedef struct { StickPdr1 PDR1; } StickDisplayIo;
typedef struct {
  struct { u8 BYTE; } SSER;
  struct { struct { u8 TDRE, TEND; } BIT; } SSSR;
} StickDisplaySsu;

extern StickDisplayIo IO;
extern StickDisplaySsu SSU;
void StickDisplayWrite(u8 value);
void StickDisplayBusInit(void);
int StickDisplayIsPowered(void);
/* Read the panel's selected 96x64 view as four-shade palette indexes. */
void StickDisplayFrame(u8 *pixels, u16 byteCount);

#ifdef __cplusplus
}
#endif

#endif
