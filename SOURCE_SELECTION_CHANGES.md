# ESP32 Bluetooth Receiver - Source Selection Modifications

## Changes Made

### 1. Pin-Based Source Selection
- Added pins 12 and 13 for hardware source selection
- Pin 12 (AUX_SELECT_PIN): Ground this pin to force AUX input
- Pin 13 (BT_SELECT_PIN): Ground this pin to force Bluetooth input
- Both pins high: Uses automatic source selection

### 2. Persistent Source Memory
- Modified loadSettings(): Now saves and restores the last active source
- Modified saveSettings(): Saves current active source to preferences
- Added setSourceDirect(): Allows direct source setting for restoration
- Storage key: 'last_source' in preferences

### 3. Source Selection Logic
- Pin grounding forces source immediately
- Automatic fallback when no pins are grounded
- Persistent memory: Selected source is saved and restored after reboot

### 4. Usage Instructions

#### Hardware Setup:
- Connect switch/jumper between pin 12 and GND for AUX selection
- Connect switch/jumper between pin 13 and GND for Bluetooth selection
- Leave both pins floating (high) for automatic selection

#### Behavior:
- On startup: Restores the last selected source before power off
- Pin 12 grounded: Forces AUX input (saves this selection)
- Pin 13 grounded: Forces Bluetooth input (saves this selection)
- No pins grounded: Uses automatic selection based on priority rules
- Source changes are automatically saved

### 5. Default Behavior Change
- Before: Always started with tone generator as highest priority
- After: Starts with the last selected source from before power off
- Fallback: If no previous source saved, defaults to Bluetooth


## Compilation Fixes Applied

### Issues Found:
- AudioSwitch.h had formatting corruption with escape characters
- AudioSwitch.cpp had similar formatting issues
- Method declarations didn't match implementations

### Fixes Applied:
- ✅ Recreated AudioSwitch.h with clean formatting
- ✅ Recreated AudioSwitch.cpp with proper method implementations
- ✅ Added missing setSourceDirect() method
- ✅ Fixed all method signatures to match declarations

### Files Ready for Compilation:
- ESP32_BT_Well_Flac_Enhanced.ino (main code with pin selection)
- AudioSwitch.h (clean header file)
- AudioSwitch.cpp (clean implementation)
- ToneGenerator.h and ToneGenerator.cpp (unchanged)
- AuxInput.h and AuxInput.cpp (unchanged)

## Testing Instructions:
1. Compile and upload the code to ESP32
2. Test pin-based source selection:
   - Ground pin 12 → Should force AUX input
   - Ground pin 13 → Should force Bluetooth input
   - Both pins high → Should use automatic selection
3. Test persistence:
   - Change source via pins or serial commands
   - Power cycle ESP32
   - Verify last source is restored
4. Monitor Serial output for debug messages
