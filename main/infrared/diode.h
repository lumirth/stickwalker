#pragma once
#include <stdbool.h>

typedef enum { DIODE_HIDDEN, DIODE_MISSING, DIODE_REVERSED, DIODE_READY, DIODE_ACTIVE } DiodeIcon;
typedef struct { DiodeIcon icon; bool receive_safe; } DiodeProbe;

/* Owns G4/G5 only before optical capture starts; no background probing. */
DiodeProbe diode_probe(void);
