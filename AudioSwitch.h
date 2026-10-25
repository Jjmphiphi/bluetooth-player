/*
 * AudioSwitch.h
 * 
 * Audio source switching and mixing for ESP32
 * Handles switching between Bluetooth, tone generator, and auxiliary input
 */

#ifndef AUDIO_SWITCH_H
#define AUDIO_SWITCH_H

#include <Arduino.h>

// Audio input sources
enum AudioInputSource {
  SOURCE_NONE,
  SOURCE_BLUETOOTH,
  SOURCE_TONE_GENERATOR,
  SOURCE_AUX_INPUT
};

class AudioSwitch {
public:
  AudioSwitch();
  
  // Initialize the audio switch
  void begin();
  
  // Set the active audio source
  void setSource(AudioInputSource source);
  
  // Set the active audio source directly (for restoring from preferences)
  void setSourceDirect(AudioInputSource source);
  
  // Get the current active source
  AudioInputSource getSource() const { return _currentSource; }
  
  // Enable/disable sources
  void enableBluetooth(bool enable);
  void enableToneGenerator(bool enable);
  void enableAuxInput(bool enable);
  
  // Check if sources are enabled
  bool isBluetoothEnabled() const { return _bluetoothEnabled; }
  bool isToneGeneratorEnabled() const { return _toneGeneratorEnabled; }
  bool isAuxInputEnabled() const { return _auxInputEnabled; }
  
  // Update Bluetooth connection status
  void updateBluetoothConnected(bool connected);
  
  // Check if Bluetooth is connected
  bool isBluetoothConnected() const { return _bluetoothConnected; }
  
  // Parse command string and set source accordingly
  // Returns true if command was valid and processed
  bool parseCommand(const String& command);
  
  // Apply switching rules based on boolean logic
  AudioInputSource applyRules();

private:
  // Current active source
  AudioInputSource _currentSource;
  
  // Source enable flags
  bool _bluetoothEnabled;
  bool _toneGeneratorEnabled;
  bool _auxInputEnabled;
  
  // Connection status
  bool _bluetoothConnected;
  
  // Helper methods
  void updateActiveSource();
};

#endif // AUDIO_SWITCH_H