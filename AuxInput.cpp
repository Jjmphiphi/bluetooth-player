/*
 * AuxInput.cpp
 * 
 * Implementation of I2S auxiliary input handler for ESP32
 * Uses AudioTools library for I2S implementation
 */

#include "AuxInput.h"

// Constructor
AuxInput::AuxInput(uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample) :
  _sampleRate(sampleRate),
  _channels(channels),
  _bitsPerSample(bitsPerSample),
  _i2sPort(1),
  _active(false),
  _gain(1.0f),
  _buffer(nullptr),
  _bufferSize(0),
  _audioInfo(sampleRate, channels, bitsPerSample)
{
}

// Initialize the I2S input
bool AuxInput::begin(int bclkPin, int lrclkPin, int dataPin, int i2sPort) {
  // Store I2S port
  _i2sPort = i2sPort;
  
  // Configure I2S stream
  auto cfg = _i2sStream.defaultConfig(RX_MODE);
  cfg.copyFrom(_audioInfo);
  cfg.port_no = _i2sPort;  // Use port_no instead of i2s_port
  cfg.pin_bck = bclkPin;
  cfg.pin_ws = lrclkPin;
  cfg.pin_data = dataPin;
  cfg.is_master = false; // Set to slave mode to take clocks from AUX source
  cfg.buffer_count = 8;
  cfg.buffer_size = 1024;
  
  // Start I2S stream
  if (!_i2sStream.begin(cfg)) {
    Serial.println("Failed to start I2S stream");
    return false;
  }
  
  // Allocate buffer for reading
  _bufferSize = 4096;
  _buffer = (uint8_t*)malloc(_bufferSize);
  if (_buffer == nullptr) {
    Serial.println("Failed to allocate buffer");
    _i2sStream.end();
    return false;
  }
  
  _active = true;
  return true;
}

// Deinitialize the I2S input
void AuxInput::end() {
  if (_active) {
    _i2sStream.end();
    
    if (_buffer != nullptr) {
      free(_buffer);
      _buffer = nullptr;
    }
    
    _active = false;
  }
}

// Read audio data from I2S input
size_t AuxInput::read(int16_t* buffer, size_t frames) {
  if (!_active || _buffer == nullptr) {
    // If not active, fill buffer with zeros
    memset(buffer, 0, frames * _channels * sizeof(int16_t));
    return 0;
  }
  
  // Calculate bytes to read
  size_t bytesToRead = frames * _channels * (_bitsPerSample / 8);
  if (bytesToRead > _bufferSize) {
    bytesToRead = _bufferSize;
  }
  
  // Read from I2S stream
  size_t bytesRead = _i2sStream.readBytes(_buffer, bytesToRead);
  
  if (bytesRead == 0) {
    // If read failed, fill buffer with zeros
    memset(buffer, 0, frames * _channels * sizeof(int16_t));
    return 0;
  }
  
  // Calculate frames read
  size_t framesRead = bytesRead / (_channels * (_bitsPerSample / 8));
  
  // Convert to int16_t and apply gain
  int16_t* src = (int16_t*)_buffer;
  for (size_t i = 0; i < framesRead * _channels; i++) {
    // Apply gain
    float sample = src[i] * _gain;
    
    // Clip to int16_t range
    if (sample > 32767.0f) sample = 32767.0f;
    if (sample < -32768.0f) sample = -32768.0f;
    
    buffer[i] = (int16_t)sample;
  }
  
  return framesRead;
}

// Set input gain
void AuxInput::setGain(float gain) {
  _gain = constrain(gain, 0.0f, 10.0f); // Limit gain to reasonable range
}