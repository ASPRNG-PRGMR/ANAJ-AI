// ============================================================================
// !!! STALE MODEL -- FEATURE SET CHANGED (v1 -> v2) !!!
// The weights below were trained against the OLD feature meaning
// ([meanR, meanG, meanB, greenRatio] per cell). The ESP32 now sends v2
// features ([meanBrightness, greenRatio, edgeDensity, contrast] per cell)
// instead. The array is still 64 uint8_t values either way, so this will
// COMPILE FINE and RUN FINE -- it will just produce meaningless
// predictions, silently, with no error anywhere.
//
// Re-run training/train_tiny_model.py against your dataset and replace
// this entire file with its fresh output before trusting any result from
// the Uno's tinyModelVerdict() again.
// ============================================================================

#ifndef TINY_MODEL_H
#define TINY_MODEL_H

#include <Arduino.h>
#include <avr/pgmspace.h>

#define FEATURE_COUNT 64
#define MODEL_SCALE 256

// PROGMEM: stored in flash, not RAM. Without this, avr-gcc puts
// `static const` arrays in RAM anyway -- on a 2KB-RAM board that's ~128
// bytes wasted for nothing, since flash has plenty of room. Read access
// needs pgm_read_word() instead of plain indexing -- see
// tinyModelVerdict() in the main sketch.
static const int16_t MODEL_WEIGHTS[FEATURE_COUNT] PROGMEM = { -48, 6, 38, 5, 43, -40, -2, 76, 3, -2, -4, 5, -14, 7, 1, -21, -12, 22, -14, -7, 55, -25, -23, 15, 22, -12, -6, 6, -10, 12, -7, -17, 33, 0, -35, -42, -10, -27, 36, 9, 13, -48, 45, 35, 52, -18, -35, -5, 16, -49, 29, 77, 1, 3, -7, -21, 34, -17, -16, 17, -43, 20, 17, -74 };
static const int32_t MODEL_BIAS = 8;

#endif // TINY_MODEL_H
