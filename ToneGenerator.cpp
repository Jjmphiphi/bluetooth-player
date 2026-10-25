/*
 * ToneGenerator.cpp
 * 
 * Implementation of high-quality audio tone generator for ESP32
 */

#include "ToneGenerator.h"

// Constructor
ToneGenerator::ToneGenerator(uint32_t sampleRate, uint8_t channels) :
  _sampleRate(sampleRate),
  _channels(channels),
  _active(false),
  _toneType(TONE_NONE),
  _frequency(1000.0f),
  _phase(0.0f),
  _phaseIncrement(0.0f),
  _startFreq(20.0f),
  _endFreq(20000.0f),
  _sweepDuration(10.0f),
  _sweepTime(0.0f),
  _level(0.0f)
{
  // Initialize pink noise filter states
  for (int c = 0; c < 2; c++) {
    for (int i = 0; i < 7; i++) {
      _pinkNoiseState[c][i] = 0.0f;
    }
  }
}

// Initialize the generator
void ToneGenerator::begin() {
  _active = false;
  _toneType = TONE_NONE;
  _phase = 0.0f;
  _sweepTime = 0.0f;
}

// Stop tone generation
void ToneGenerator::stop() {
  _active = false;
  _toneType = TONE_NONE;
}

// Generate a sine wave at specified frequency and level
void ToneGenerator::generateSine(float frequency, float levelDb) {
  _toneType = TONE_SINE;
  _frequency = constrain(frequency, 10.0f, 20000.0f); // Limit to audible range
  _level = dbToLinear(levelDb);
  updatePhaseIncrement(_frequency);
  _active = true;
}

// Generate a frequency sweep
void ToneGenerator::generateSweep(float startFreq, float endFreq, float durationSec, float levelDb) {
  _toneType = TONE_SWEEP;
  _startFreq = constrain(startFreq, 10.0f, 20000.0f);
  _endFreq = constrain(endFreq, 10.0f, 20000.0f);
  _sweepDuration = max(0.1f, durationSec);
  _level = dbToLinear(levelDb);
  _sweepTime = 0.0f;
  _active = true;
}

// Generate noise (white or pink)
void ToneGenerator::generateNoise(bool isPink, float levelDb) {
  _toneType = isPink ? TONE_PINK_NOISE : TONE_WHITE_NOISE;
  _level = dbToLinear(levelDb);
  _active = true;
}

// Preset: 1kHz sine wave at -18dBFS
void ToneGenerator::generate1kHz() {
  generateSine(1000.0f, -18.0f);
}

// Preset: 20Hz to 20kHz sweep over 10 seconds
void ToneGenerator::generateSweep() {
  generateSweep(20.0f, 20000.0f, 10.0f, -18.0f);
}

// Preset: White noise at -24dBFS
void ToneGenerator::generateWhiteNoise() {
  generateNoise(false, -24.0f);
}

// Preset: Pink noise at -24dBFS
void ToneGenerator::generatePinkNoise() {
  generateNoise(true, -24.0f);
}

// Process audio samples - fill buffer with generated tone
void ToneGenerator::process(int16_t* buffer, size_t frames) {
  if (!_active || _toneType == TONE_NONE) {
    // If not active, fill buffer with zeros
    memset(buffer, 0, frames * _channels * sizeof(int16_t));
    return;
  }

  // Process each frame
  for (size_t i = 0; i < frames; i++) {
    float leftSample = 0.0f;
    float rightSample = 0.0f;
    
    // Handle each tone type separately to avoid variable initialization issues in switch
    if (_toneType == TONE_SINE) {
      // Generate sine wave
      leftSample = sin(_phase) * _level;
      rightSample = leftSample; // Same for both channels
      
      // Update phase
      _phase += _phaseIncrement;
      if (_phase >= 2.0f * PI) {
        _phase -= 2.0f * PI;
      }
    }
    else if (_toneType == TONE_SWEEP) {
      // Calculate current frequency based on sweep time
      float currentFreq = getCurrentSweepFrequency();
      updatePhaseIncrement(currentFreq);
      
      // Generate sine wave at current frequency
      leftSample = sin(_phase) * _level;
      rightSample = leftSample; // Same for both channels
      
      // Update phase
      _phase += _phaseIncrement;
      if (_phase >= 2.0f * PI) {
        _phase -= 2.0f * PI;
      }
      
      // Update sweep time
      _sweepTime += 1.0f / _sampleRate;
      if (_sweepTime >= _sweepDuration) {
        // Loop the sweep
        _sweepTime = 0.0f;
      }
    }
    else if (_toneType == TONE_WHITE_NOISE) {
      // Generate white noise
      leftSample = generateWhiteNoiseValue() * _level;
      rightSample = generateWhiteNoiseValue() * _level;
    }
    else if (_toneType == TONE_PINK_NOISE) {
      // Generate pink noise
      leftSample = generatePinkNoiseValue(0) * _level;
      rightSample = generatePinkNoiseValue(1) * _level;
    }
    else {
      // Default case
      leftSample = 0.0f;
      rightSample = 0.0f;
    }
    
    // Convert to int16_t and store in buffer
    buffer[i * _channels] = (int16_t)(leftSample * 32767.0f);
    if (_channels > 1) {
      buffer[i * _channels + 1] = (int16_t)(rightSample * 32767.0f);
    }
  }
}

// Parse command string and set tone accordingly
bool ToneGenerator::parseCommand(const String& command) {
  // Check for preset commands
  if (command == "tone_1khz") {
    generate1kHz();
    return true;
  }
  else if (command == "tone_sweep") {
    generateSweep();
    return true;
  }
  else if (command == "tone_white") {
    generateWhiteNoise();
    return true;
  }
  else if (command == "tone_pink") {
    generatePinkNoise();
    return true;
  }
  else if (command == "tone_stop") {
    stop();
    return true;
  }
  
  // Check for custom parameter commands
  if (command.startsWith("tone_sine:")) {
    // Parse sine wave parameters: tone_sine:freq:level
    int firstColon = command.indexOf(':', 10);
    if (firstColon > 0) {
      String freqStr = command.substring(10, firstColon);
      String levelStr = command.substring(firstColon + 1);
      
      float freq = freqStr.toFloat();
      float level = levelStr.toFloat();
      
      generateSine(freq, level);
      return true;
    }
  }
  else if (command.startsWith("tone_sweep:")) {
    // Parse sweep parameters: tone_sweep:start:end:duration:level
    int firstColon = command.indexOf(':', 11);
    int secondColon = command.indexOf(':', firstColon + 1);
    int thirdColon = command.indexOf(':', secondColon + 1);
    
    if (firstColon > 0 && secondColon > 0 && thirdColon > 0) {
      String startStr = command.substring(11, firstColon);
      String endStr = command.substring(firstColon + 1, secondColon);
      String durationStr = command.substring(secondColon + 1, thirdColon);
      String levelStr = command.substring(thirdColon + 1);
      
      float start = startStr.toFloat();
      float end = endStr.toFloat();
      float duration = durationStr.toFloat();
      float level = levelStr.toFloat();
      
      generateSweep(start, end, duration, level);
      return true;
    }
  }
  else if (command.startsWith("tone_noise:")) {
    // Parse noise parameters: tone_noise:type:level
    int firstColon = command.indexOf(':', 11);
    if (firstColon > 0) {
      String typeStr = command.substring(11, firstColon);
      String levelStr = command.substring(firstColon + 1);
      
      float level = levelStr.toFloat();
      bool isPink = (typeStr == "pink");
      
      generateNoise(isPink, level);
      return true;
    }
  }
  
  return false; // Command not recognized
}

// Helper method: Convert dB to linear amplitude
float ToneGenerator::dbToLinear(float db) {
  return pow(10.0f, db / 20.0f);
}

// Helper method: Generate white noise value
float ToneGenerator::generateWhiteNoiseValue() {
  // Simple white noise generator
  return ((random(1000000) / 500000.0f) - 1.0f);
}

// Helper method: Generate pink noise value using Voss algorithm
float ToneGenerator::generatePinkNoiseValue(int channel) {
  // Pink noise using the Voss-McCartney algorithm (simplified)
  float white = generateWhiteNoiseValue();
  
  // Apply pink filter (first-order IIR filter)
  _pinkNoiseState[channel][0] = 0.99765f * _pinkNoiseState[channel][0] + white * 0.0990460f;
  _pinkNoiseState[channel][1] = 0.96300f * _pinkNoiseState[channel][1] + white * 0.2965164f;
  _pinkNoiseState[channel][2] = 0.57000f * _pinkNoiseState[channel][2] + white * 1.0526913f;
  
  // Mix the filtered noise sources
  return _pinkNoiseState[channel][0] + _pinkNoiseState[channel][1] + _pinkNoiseState[channel][2] + white * 0.1f;
}

// Helper method: Update phase increment based on frequency
void ToneGenerator::updatePhaseIncrement(float frequency) {
  _phaseIncrement = 2.0f * PI * frequency / _sampleRate;
}

// Helper method: Calculate current frequency for sweep
float ToneGenerator::getCurrentSweepFrequency() {
  // Use logarithmic sweep for more natural sound
  float t = _sweepTime / _sweepDuration;
  return _startFreq * pow(_endFreq / _startFreq, t);
}