#ifndef PW_STICK_DISPLAY_PANEL_H
#define PW_STICK_DISPLAY_PANEL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Call after M5.begin. Present only from the application task, outside the
 * optically quiet receive interval; drawing does not advance game state. */
int StickDisplayPanelInit(void);
void StickDisplayPresent(void);

#ifdef __cplusplus
}
#endif

#endif
