/* Preloaded only for upstream files with port replacements.
 * Strong aliases in overrides/ supply these symbols; sources.cmake requires
 * each implementation so a missing source file fails the link. */
#include "overrides.h"

#pragma weak RadarChooseEncounter
#pragma weak DowsingAwardBonus
#pragma weak DowsingChooseReward
#pragma weak PeerAwardGift
#pragma weak InventoryMasks
#pragma weak MotionEstimateQ9

#include "startup/iodefine.h"
#pragma weak ScratchReset
#pragma weak ScratchAlloc
#pragma weak BulkDecode
#pragma weak MainTick
#pragma weak BeepTick
#pragma weak MotionSessionWake
#pragma weak MotionSessionStart
#pragma weak MotionSessionEnd
#pragma weak IRQ0Interrupt
#pragma weak PowerOnReset
#pragma weak CommitStagedWalk
#pragma weak RtcSetTime
#pragma weak RtcReadStable
