/*
 * ToneGenerator.h
 * 
 * High-quality audio tone generator for ESP32
 * Supports sine waves, frequency sweeps, white noise, and pink noise
 */

#ifndef TONE_GENERATOR_H
#define TONE_GENERATOR_H

#include <Arduino.h>
#include <math.h>

// Tone types
enum ToneType {
  TONE_NONE,
  TONE_SINE,
  TONE_SWEEP,
  TONE_WHITE_NOISE,
  TONE_PINK_NOISE
};

class ToneGenerator {
public:
  ToneGenerator(uint32_t sampleRate = 44100, uint8_t channels = 2);
  
  // Initialize the generator
  void begin();
  
  // Stop tone generation
  void stop();
  
  // Generate a sine wave at specified frequency and level
  void generateSine(float frequency, float levelDb);
  
  // Generate a frequency sweep
  void generateSweep(float startFreq, float endFreq, float durationSec, float levelDb);
  
  // Generate noise (white or pink)
  void generateNoise(bool isPink, float levelDb);
  
  // Preset tones
  void generate1kHz();       // 1kHz sine wave at -18dBFS
  void generateSweep();      // 20Hz to 20kHz sweep over 10 seconds
  void generateWhiteNoise(); // White noise at -24dBFS
  void generatePinkNoise();  // Pink noise at -24dBFS
  
  // Process audio samples - call this to fill the buffer with generated tone
  void process(int16_t* buffer, size_t frames);
  
  // Check if tone generator is active
  bool isActive() const { return _active; }
  
  // Get current tone type
  ToneType getToneType() const { return _toneType; }
  
  // Parse command string and set tone accordingly
  // Returns true if command was valid and processed
  bool parseCommand(const String& command);

private:
  // Configuration
  uint32_t _sampleRate;
  uint8_t _channels;
  
  // State
  bool _active;
  ToneType _toneType;
  
  // Sine wave parameters
  float _frequency;
  float _phase;
  float _phaseIncrement;
  
  // Sweep parameters
  float _startFreq;
  float _endFreq;
  float _sweepDuration;
  float _sweepTime;
  
  // Level control
  float _level;  // Linear amplitude (0.0 to 1.0)
  
  // Pink noise filter state (for each channel)
  float _pinkNoiseState[2][7];
  
  // Helper methods
  float dbToLinear(float db);
  float generatePinkNoiseValue(int channel);
  float generateWhiteNoiseValue();
  void updatePhaseIncrement(float frequency);
  float getCurrentSweepFrequency();
};

#endif // TONE_GENERATOR_H