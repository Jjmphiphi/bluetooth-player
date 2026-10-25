/*
 * AudioSwitch.cpp
 * 
 * Implementation of audio source switching and mixing for ESP32
 */

#include "AudioSwitch.h"

// Constructor
AudioSwitch::AudioSwitch() :
  _currentSource(SOURCE_NONE),
  _bluetoothEnabled(true),    // Bluetooth enabled by default
  _toneGeneratorEnabled(false), // Tone generator disabled by default
  _auxInputEnabled(false),    // Aux input disabled by default
  _bluetoothConnected(false)
{
}

// Initialize the audio switch
void AudioSwitch::begin() {
  // Don't automatically apply rules here - the source should be restored from settings
  // Only apply rules if no source has been set at all
  // This preserves the source restored from preferences
}

// Set the active audio source
void AudioSwitch::setSource(AudioInputSource source) {
  // Only set the source if it's enabled
  switch (source) {
    case SOURCE_BLUETOOTH:
      if (_bluetoothEnabled) {
        _currentSource = source;
      }
      break;
      
    case SOURCE_TONE_GENERATOR:
      if (_toneGeneratorEnabled) {
        _currentSource = source;
      }
      break;
      
    case SOURCE_AUX_INPUT:
      if (_auxInputEnabled) {
        _currentSource = source;
      }
      break;
      
    case SOURCE_NONE:
      _currentSource = SOURCE_NONE;
      break;
      
    default:
      // Unsupported source
      break;
  }
}

// Set the active audio source directly (for restoring from preferences)
void AudioSwitch::setSourceDirect(AudioInputSource source) {
  _currentSource = source;
}

// Enable/disable Bluetooth
void AudioSwitch::enableBluetooth(bool enable) {
  _bluetoothEnabled = enable;
  // Only update active source if the current source is being disabled
  if (!enable && _currentSource == SOURCE_BLUETOOTH) {
    updateActiveSource();
  }
}

// Enable/disable Tone Generator
void AudioSwitch::enableToneGenerator(bool enable) {
  _toneGeneratorEnabled = enable;
  // Only update active source if the current source is being disabled
  if (!enable && _currentSource == SOURCE_TONE_GENERATOR) {
    updateActiveSource();
  }
}

// Enable/disable Aux Input
void AudioSwitch::enableAuxInput(bool enable) {
  _auxInputEnabled = enable;
  // Only update active source if the current source is being disabled
  if (!enable && _currentSource == SOURCE_AUX_INPUT) {
    updateActiveSource();
  }
}

// Update Bluetooth connection status
void AudioSwitch::updateBluetoothConnected(bool connected) {
  _bluetoothConnected = connected;
}

// Apply switching rules based on boolean logic
AudioInputSource AudioSwitch::applyRules() {
  // Bluetooth when connected takes priority (required for reliable iOS A2DP + library I2S path)
  
  // Rule 1: If Bluetooth is connected, use it
  if (_bluetoothEnabled && _bluetoothConnected) {
    return SOURCE_BLUETOOTH;
  }
  
  // Rule 2: If tone generator is enabled, use it
  if (_toneGeneratorEnabled) {
    return SOURCE_TONE_GENERATOR;
  }
  
  // Rule 3: If aux input is enabled, use it
  if (_auxInputEnabled) {
    return SOURCE_AUX_INPUT;
  }
  
  // Default: No source
  return SOURCE_NONE;
}

// Parse command string and set source accordingly
bool AudioSwitch::parseCommand(const String& command) {
  if (command == "source:bt") {
    setSource(SOURCE_BLUETOOTH);
    return true;
  }
  else if (command == "source:tone") {
    setSource(SOURCE_TONE_GENERATOR);
    return true;
  }
  else if (command == "source:aux") {
    setSource(SOURCE_AUX_INPUT);
    return true;
  }
  else if (command == "source:none") {
    setSource(SOURCE_NONE);
    return true;
  }
  else if (command == "enable:bt") {
    enableBluetooth(true);
    return true;
  }
  else if (command == "disable:bt") {
    enableBluetooth(false);
    return true;
  }
  else if (command == "enable:tone") {
    enableToneGenerator(true);
    return true;
  }
  else if (command == "disable:tone") {
    enableToneGenerator(false);
    return true;
  }
  else if (command == "enable:aux") {
    enableAuxInput(true);
    return true;
  }
  else if (command == "disable:aux") {
    enableAuxInput(false);
    return true;
  }
  
  return false; // Command not recognized
}

// Helper method: Update active source based on enabled sources
void AudioSwitch::updateActiveSource() {
  // Apply rules to determine the new active source
  AudioInputSource newSource = applyRules();
  
  // Update the current source if it changed
  if (newSource != _currentSource) {
    _currentSource = newSource;
  }
}