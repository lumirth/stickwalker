#ifndef PW_STICK_DISPLAY_PANEL_H
#define PW_STICK_DISPLAY_PANEL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Call after board init. Present only from the application task, outside the
 * optically quiet receive interval; drawing does not advance game state. */
int StickDisplayPanelInit(void);
void StickDisplayPanelSetOrientation(unsigned right_side_down);
void StickDisplayPanelSetBacklight(unsigned enabled);
void StickDisplayPanelSetContrastDelta(unsigned delta);
void StickDisplayPresent(void);

#ifdef __cplusplus
}
#endif

#endif
