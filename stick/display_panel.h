#ifndef PW_STICK_DISPLAY_PANEL_H
#define PW_STICK_DISPLAY_PANEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call after board init. Present only from the application task, outside the
 * optically quiet receive interval; drawing does not advance game state. */
int StickDisplayPanelInit(void);
void StickDisplayPanelSetOrientation(unsigned right_side_down);
void StickDisplayPanelSetBacklight(unsigned enabled);
void StickDisplayPanelSetContrastDelta(unsigned delta);
/* False until asynchronous physical wake is ready; keep the image pending. */
int StickDisplayPresent(void);
void StickDisplayPowerService(void);
void StickDisplayPrepareWake(void);
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
