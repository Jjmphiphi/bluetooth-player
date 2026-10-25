/*
 * AudioTask.h
 *
 * FreeRTOS task for non-Bluetooth sources (tone, AUX, silence).
 * Bluetooth PCM stays in the A2DP callback for lowest latency.
 */

#ifndef AUDIO_TASK_H
#define AUDIO_TASK_H

#include <Arduino.h>

void audioTaskBegin(uint32_t sampleRate, size_t bufferFrames);
void audioTaskEnd();

#endif // AUDIO_TASK_H
