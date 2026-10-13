#ifndef PW_STICK_DISPLAY_PANEL_H
#define PW_STICK_DISPLAY_PANEL_H

#include <stdint.h>
#include "display_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize after board setup. Present from the application task between
 * optical receive intervals. */
int StickDisplayPanelInit(void);
void StickDisplayPanelSetOrientation(unsigned right_side_down);
void StickDisplayPanelSetBacklight(unsigned enabled);
void StickDisplayPanelSetContrastDelta(unsigned delta);
int StickDisplayPresent(const StickDisplaySnapshot *snapshot);
int StickDisplayPresentationPending(void);
void StickDisplayPowerService(void);
void StickDisplayPrepareWake(void);
/* Cancel pending wake preparation when the user releases M early. */
void StickDisplayCancelPreparedWake(void);
int StickDisplayPanelIsReady(void);
uint64_t StickDisplayNextDeadline(void);
/* Call after an overlay or external drawing replaces the physical image. */
void StickDisplayInvalidate(void);
/* Appearance changes pixel colors; native contrast still controls brightness. */
unsigned StickDisplayIsDark(void);
int StickDisplaySetDark(unsigned dark);
uint16_t StickDisplayBackground(void);
uint16_t StickDisplayForeground(void);

#ifdef __cplusplus
}
#endif

#endif
