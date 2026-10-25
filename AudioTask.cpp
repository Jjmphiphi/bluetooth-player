/*
 * AudioTask.cpp
 *
 * Pinned high-priority task on core 1 — drives tone, AUX, and idle silence output.
 */

#include "AudioTask.h"
#include "AudioPipeline.h"
#include "AudioSwitch.h"
#include "ToneGenerator.h"
#include "AuxInput.h"

extern AudioSwitch audioSwitch;
extern ToneGenerator toneGenerator;
extern AuxInput auxInput;
extern AudioPipeline audioPipeline;

static TaskHandle_t s_taskHandle = nullptr;
static size_t s_bufferFrames = 1024;
static int16_t *s_buffer = nullptr;
static uint32_t s_sampleRate = 44100;

static void audioTaskEntry(void *param) {
  (void)param;

  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t minPeriod = 1;

  while (true) {
    AudioInputSource src = audioSwitch.getSource();

    if (src != SOURCE_BLUETOOTH) {
      size_t frames = s_bufferFrames;
      size_t bytes = frames * 2 * sizeof(int16_t);

      switch (src) {
        case SOURCE_TONE_GENERATOR:
          if (toneGenerator.isActive()) {
            toneGenerator.process(s_buffer, frames);
          } else {
            memset(s_buffer, 0, bytes);
          }
          break;

        case SOURCE_AUX_INPUT:
          if (auxInput.isActive()) {
            size_t framesRead = auxInput.read(s_buffer, frames);
            if (framesRead < frames) {
              memset(s_buffer + framesRead * 2, 0, (frames - framesRead) * 2 * sizeof(int16_t));
            }
          } else {
            memset(s_buffer, 0, bytes);
          }
          break;

        case SOURCE_NONE:
        default:
          memset(s_buffer, 0, bytes);
          break;
      }

      audioPipeline.writePCM(reinterpret_cast<uint8_t *>(s_buffer), bytes);
    }

    TickType_t period = pdMS_TO_TICKS((s_bufferFrames * 1000UL) / s_sampleRate);
    if (period < minPeriod) {
      period = minPeriod;
    }
    vTaskDelayUntil(&lastWake, period);
  }
}

void audioTaskBegin(uint32_t sampleRate, size_t bufferFrames) {
  if (s_taskHandle != nullptr) {
    return;
  }

  s_sampleRate = sampleRate;
  s_bufferFrames = bufferFrames;

  s_buffer = static_cast<int16_t *>(malloc(bufferFrames * 2 * sizeof(int16_t)));
  if (s_buffer == nullptr) {
    Serial.println("Audio task: buffer alloc failed");
    return;
  }

  BaseType_t ok = xTaskCreatePinnedToCore(
    audioTaskEntry,
    "audioOut",
    4096,
    nullptr,
    configMAX_PRIORITIES - 2,
    &s_taskHandle,
    1);

  if (ok != pdPASS) {
    Serial.println("Audio task: create failed");
    free(s_buffer);
    s_buffer = nullptr;
    s_taskHandle = nullptr;
  } else {
    Serial.println("Audio task started (core 1)");
  }
}

void audioTaskEnd() {
  if (s_taskHandle != nullptr) {
    vTaskDelete(s_taskHandle);
    s_taskHandle = nullptr;
  }
  if (s_buffer != nullptr) {
    free(s_buffer);
    s_buffer = nullptr;
  }
}
