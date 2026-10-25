/*
 * AuxInput.h
 * 
 * I2S auxiliary input handler for ESP32
 * Supports I2S microphone or external I2S source
 * Uses AudioTools library for I2S implementation
 */

#ifndef AUX_INPUT_H
#define AUX_INPUT_H

#include <Arduino.h>
#include "AudioTools.h"

class AuxInput {
public:
  AuxInput(uint32_t sampleRate = 44100, uint8_t channels = 2, uint8_t bitsPerSample = 16);
  
  // Initialize the I2S input
  bool begin(int bclkPin, int lrclkPin, int dataPin, int i2sPort = 1);
  
  // Deinitialize the I2S input
  void end();
  
  // Read audio data from I2S input
  size_t read(int16_t* buffer, size_t frames);
  
  // Check if input is active
  bool isActive() const { return _active; }
  
  // Set input gain (0.0 to 10.0)
  void setGain(float gain);
  
  // Get current gain
  float getGain() const { return _gain; }

private:
  // Configuration
  uint32_t _sampleRate;
  uint8_t _channels;
  uint8_t _bitsPerSample;
  int _i2sPort;
  
  // State
  bool _active;
  float _gain;
  
  // AudioTools I2S stream
  I2SStream _i2sStream;
  
  // Audio info
  AudioInfo _audioInfo;
  
  // Buffer for reading
  uint8_t* _buffer;
  size_t _bufferSize;
};

#endif // AUX_INPUT_H