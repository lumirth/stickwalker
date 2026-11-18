#include "project.h"
#include "pokewalker_host.h"
#include "application/pw_eeprom_m95512.h"
#include "application/pw_selftest.h"

static const u8 eeprom_signature[EEPROM_SIGNATURE_BYTES] = "nintendo";

void BootSignatureWrite(void)
{
  for (u8 index = 0; index < EEPROM_SIGNATURE_BYTES; index++) {
    EepromWriteByte(index, eeprom_signature[index]);
  }
}

u8 BootSignatureValid(void)
{
  for (u8 index = 0; index < EEPROM_SIGNATURE_BYTES; index++) {
    if (EepromReadByte(index) != eeprom_signature[index]) {
      return 0;
    }
  }
  return 1;
}

/* H8 factory fixture screens have no host peripheral implementation. */
void DiagnosticsInit(void) { pw_host_fault("H8 factory diagnostics are unavailable"); }
void DiagnosticsUpdate(void) { pw_host_fault("H8 factory diagnostics are unavailable"); }
void DiagnosticsRender(void) { pw_host_fault("H8 factory diagnostics are unavailable"); }
void ThresholdInit(void) { pw_host_fault("H8 factory diagnostics are unavailable"); }
void ThresholdUpdate(void) { pw_host_fault("H8 factory diagnostics are unavailable"); }
void ThresholdRender(void) { pw_host_fault("H8 factory diagnostics are unavailable"); }
void ThresholdFailureRender(void) { pw_host_fault("H8 factory diagnostics are unavailable"); }
