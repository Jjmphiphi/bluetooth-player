/*
 * AudioPipeline.h
 *
 * Unified I2S output path with optional enhancement (off by default for best fidelity).
 */

#ifndef AUDIO_PIPELINE_H
#define AUDIO_PIPELINE_H

#include <Arduino.h>
#include "AudioTools.h"

class AudioPipeline {
public:
  AudioPipeline();

  void attachOutput(I2SStream *output);
  void writePCM(const uint8_t *data, size_t byteCount);

  void setEnhancementEnabled(bool enabled);
  bool isEnhancementEnabled() const { return _enhancementEnabled; }
  void resetEnhancementState();

private:
  I2SStream *_output;
  bool _enhancementEnabled;

  int16_t _prevLeftSample;
  int16_t _prevRightSample;
  int32_t _bassLeftAccum;
  int32_t _bassRightAccum;

  void applyEnhancement(int16_t *samples, int sampleCount);
};

#endif // AUDIO_PIPELINE_H
