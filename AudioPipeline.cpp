/*
 * AudioPipeline.cpp
 *
 * Single output path for all audio sources. Enhancement is optional and disabled
 * by default to preserve transparent Bluetooth playback quality.
 */

#include "AudioPipeline.h"

// Enhancement tuning (only applied when explicitly enabled)
static const int16_t NOISE_GATE_THRESHOLD = 10;
static const float BASS_BOOST_GAIN = 1.08f;
static const int32_t COMPRESSION_THRESHOLD = 14500;
static const float COMPRESSION_RATIO = 2.5f;
static const float STEREO_WIDTH_MULT = 1.25f;

AudioPipeline::AudioPipeline()
  : _output(nullptr),
    _enhancementEnabled(false),
    _prevLeftSample(0),
    _prevRightSample(0),
    _bassLeftAccum(0),
    _bassRightAccum(0) {}

void AudioPipeline::attachOutput(I2SStream *output) {
  _output = output;
}

void AudioPipeline::setEnhancementEnabled(bool enabled) {
  if (_enhancementEnabled != enabled) {
    _enhancementEnabled = enabled;
    resetEnhancementState();
  }
}

void AudioPipeline::resetEnhancementState() {
  _prevLeftSample = 0;
  _prevRightSample = 0;
  _bassLeftAccum = 0;
  _bassRightAccum = 0;
}

void AudioPipeline::writePCM(const uint8_t *data, size_t byteCount) {
  if (_output == nullptr || byteCount == 0) {
    return;
  }

  if (!_enhancementEnabled) {
    _output->write(data, byteCount);
    return;
  }

  static uint8_t workBuffer[4096];
  size_t offset = 0;

  while (offset < byteCount) {
    size_t chunkBytes = min(byteCount - offset, sizeof(workBuffer));
    memcpy(workBuffer, data + offset, chunkBytes);

    int16_t *samples = reinterpret_cast<int16_t *>(workBuffer);
    int sampleCount = static_cast<int>(chunkBytes / sizeof(int16_t));
    applyEnhancement(samples, sampleCount);

    _output->write(workBuffer, chunkBytes);
    offset += chunkBytes;
  }
}

void AudioPipeline::applyEnhancement(int16_t *samples, int sampleCount) {
  for (int i = 0; i < sampleCount; i += 2) {
    int32_t left = samples[i];
    int32_t right = samples[i + 1];

    if (abs(left) < NOISE_GATE_THRESHOLD) {
      left = (left * abs(left)) / NOISE_GATE_THRESHOLD;
    }
    if (abs(right) < NOISE_GATE_THRESHOLD) {
      right = (right * abs(right)) / NOISE_GATE_THRESHOLD;
    }

    _bassLeftAccum = ((_bassLeftAccum * 7) + left) / 8;
    _bassRightAccum = ((_bassRightAccum * 7) + right) / 8;

    left = left + static_cast<int32_t>(_bassLeftAccum * (BASS_BOOST_GAIN - 1.0f));
    right = right + static_cast<int32_t>(_bassRightAccum * (BASS_BOOST_GAIN - 1.0f));

    if (abs(left) > COMPRESSION_THRESHOLD) {
      int32_t excess = abs(left) - COMPRESSION_THRESHOLD;
      int32_t compressed = COMPRESSION_THRESHOLD + static_cast<int32_t>(excess / COMPRESSION_RATIO);
      left = (left > 0) ? compressed : -compressed;
    }
    if (abs(right) > COMPRESSION_THRESHOLD) {
      int32_t excess = abs(right) - COMPRESSION_THRESHOLD;
      int32_t compressed = COMPRESSION_THRESHOLD + static_cast<int32_t>(excess / COMPRESSION_RATIO);
      right = (right > 0) ? compressed : -compressed;
    }

    int32_t filteredLeft = (left * 3 + _prevLeftSample) / 4;
    int32_t filteredRight = (right * 3 + _prevRightSample) / 4;
    _prevLeftSample = static_cast<int16_t>(left);
    _prevRightSample = static_cast<int16_t>(right);

    int32_t mid = (filteredLeft + filteredRight) / 2;
    int32_t side = (filteredLeft - filteredRight) / 2;
    side = static_cast<int32_t>(side * STEREO_WIDTH_MULT);

    left = mid + side;
    right = mid - side;

    if (left > 30000) {
      left = 30000 + (left - 30000) / 3;
    } else if (left < -30000) {
      left = -30000 + (left + 30000) / 3;
    }
    if (right > 30000) {
      right = 30000 + (right - 30000) / 3;
    } else if (right < -30000) {
      right = -30000 + (right + 30000) / 3;
    }

    samples[i] = constrain(left, -32000, 32000);
    samples[i + 1] = constrain(right, -32000, 32000);
  }
}
