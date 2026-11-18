#include "overrides.h"
#include "project.h"

extern const u8 g_motionCandidateOrder[10];
s16 MotionCentroidQ9(u8 classIndex, u16 *candidateBins);

#define MOTION_MIN_MAGNITUDE 0x200
#define MOTION_NO_CLASS 0xff

/* From upstream/src/application/pw_pedometer.c.
 * Products and shifts retain the H8 unsigned 16-bit intermediate results. */
s32 pw_port_MotionEstimateQ9(u16 *spectrum)
{
  u16 spectrumMaximum;
  u16 candidateMagnitude;
  u16 *cursor;
  u16 *bins;
  u16 restartDivisor;
  const u8 *candidate;
  struct {
    u16 magnitude;
    u8 classIndex;
    u8 candidateIndex;
  } walk;

  spectrumMaximum = 0;
  bins = spectrum;
  bins++;
  walk.candidateIndex = 0;
  cursor = bins;
  do {
    if (spectrumMaximum < *cursor) {
      spectrumMaximum = *cursor;
    }
    walk.candidateIndex++;
    cursor++;
  } while (walk.candidateIndex < 0x1d);

  candidateMagnitude = 0;
  walk.classIndex = MOTION_NO_CLASS;
  bins = spectrum;
  bins += 5;
  walk.candidateIndex = 0;
  do {
    walk.magnitude =
        bins[*(candidate = g_motionCandidateOrder + walk.candidateIndex)];
    if (walk.magnitude >= MOTION_MIN_MAGNITUDE) {
      if ((u16)(candidateMagnitude * 3u) < (u16)(walk.magnitude * 2u)) {
        walk.classIndex = *candidate;
        candidateMagnitude = walk.magnitude;
      }
    }
    walk.candidateIndex++;
  } while (walk.candidateIndex < 10);

  if (g_work.motion.batch.lastRejected != 0) {
    restartDivisor = 3;
    candidateMagnitude = ((u16)(candidateMagnitude << 2) / restartDivisor);
    if (spectrumMaximum > candidateMagnitude) {
      walk.classIndex = MOTION_NO_CLASS;
    }
  } else {
    if (spectrumMaximum > (u16)(candidateMagnitude << 1)) {
      walk.classIndex = MOTION_NO_CLASS;
    }
  }

  if (walk.classIndex == MOTION_NO_CLASS) {
    g_work.motion.batch.resetOnlyByte = 0;
    g_work.motion.batch.pendingStepsQ9 = 0;
    g_work.motion.batch.lastRejected = 1;
    return 0;
  }
  g_work.motion.batch.lastRejected = 0;
  return MotionCentroidQ9(walk.classIndex, spectrum + 5);
}

extern __typeof__(pw_port_MotionEstimateQ9) MotionEstimateQ9
    __attribute__((alias("pw_port_MotionEstimateQ9")));
