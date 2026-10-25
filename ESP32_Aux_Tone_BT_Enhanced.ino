/*
 * ESP32 Bluetooth Audio Receiver
 *
 * Transparent I2S output by default (enhancement optional).
 * Features: BT A2DP, AUX input, tone generator, source switching, deep sleep.
 */

#include "AudioTools.h"
#include "BluetoothA2DPSink.h"
#include <driver/i2s.h>
#include "esp_err.h"
#include <Preferences.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_wifi.h>
#include <nvs_flash.h>

#include "ToneGenerator.h"
#include "AuxInput.h"
#include "AudioSwitch.h"
#include "AudioPipeline.h"
#include "AudioTask.h"

// Pin definitions
#define LED_PIN 2
#define MODE_SELECT_PIN 4
#define VOLUME_UP_PIN 32
#define VOLUME_DOWN_PIN 33

// Source selection pins
#define AUX_SELECT_PIN 12      // Ground this pin to force AUX input
#define BT_SELECT_PIN 13       // Ground this pin to force Bluetooth input

// I2S pins for Bluetooth output
#define I2S_BCLK_PIN 26
#define I2S_LRCLK_PIN 25
#define I2S_DOUT_PIN 22

// I2S pins for Auxiliary input
#define I2S_AUX_BCLK_PIN 18
#define I2S_AUX_LRCLK_PIN 19
#define I2S_AUX_DIN_PIN 23

// UART communication
#define UART_RX_PIN 17
#define UART_TX_PIN 16
#define UART_BAUD_RATE 115200

// Deep sleep wake button (parallel with Audio Processor encoder button)
#define WAKE_BUTTON_PIN 27  // RTC GPIO, connected in parallel to Audio Processor GPIO7

// MAX98357A amplifier control for ultra-low power deep sleep
#define MAX98357A_SD_MODE 14  // GPIO 14 controls shutdown mode (HIGH=active, LOW=shutdown ~2µA)
#define SIGNAL_PIN_21 21      // GPIO 21 signal (HIGH when active, LOW in sleep)

// Audio configuration
#define SAMPLE_RATE 44100
#define CHANNELS 2
#define BITS_PER_SAMPLE 16

AudioInfo info(SAMPLE_RATE, CHANNELS, BITS_PER_SAMPLE);
I2SStream out;
BluetoothA2DPSink a2dp_sink(out);

#define AUDIO_BUFFER_SIZE 1024

// Preferences for persistent storage
Preferences preferences;

// State management
bool bluetoothConnected = false;
bool standaloneMode = false;
bool wokenFromSleep = false;
bool hardwareSourceOverride = false; // Flag to indicate hardware pin is overriding automatic rules
String currentTrack = "";
String currentArtist = "";
float btA2DPVolume = 1.0; // BT A2DP volume (0.0-1.0); default full — iPhone controls level via AVRCP
float processorVolume = 0.5; // Audio Processor master volume (0.0-1.0) - received via UART
bool enhanceAudioEnabled = false; // Off by default for best fidelity

// Deferred source routing (never reconfigure audio from BT stack callback — avoids crashes)
static volatile bool pendingSourceRouting = false;
static volatile bool pendingAutoSourceSelection = false;

// Volume handshake state (for dual mode)
bool volumeHandshakeComplete = false;
bool volumeRequested = false;
unsigned long volumeRequestTime = 0;
#define VOLUME_REQUEST_TIMEOUT 10000  // 10 seconds timeout (Audio Processor needs time to boot)
#define VOLUME_REQUEST_RETRY_INTERVAL 1000  // Retry every 1 second
#define VOLUME_REQUEST_INITIAL_DELAY 2000  // Wait 2 seconds before first request

// Button debounce
unsigned long lastVolumeUpPress = 0;
unsigned long lastVolumeDownPress = 0;
unsigned long wakeButtonDownTime = 0;
bool wakeButtonWasDown = false;
#define DEBOUNCE_TIME 300
#define LONG_PRESS_TIME 4000 // 4 seconds for deep sleep

ToneGenerator toneGenerator(SAMPLE_RATE, CHANNELS);
AuxInput auxInput(SAMPLE_RATE, CHANNELS, BITS_PER_SAMPLE);
AudioSwitch audioSwitch;
AudioPipeline audioPipeline;

// Function prototypes
void sendCommandToProcessor(const String &command);
void setVolume(float volume, bool saveToPrefs = true);
void volumeUp();
void volumeDown();
void showStatus();
void showHelp();
void blinkLED();
void checkButtons();
void setEnhanceAudio(bool enabled, bool saveToPrefs = true);
void loadSettings();
void saveSettings();
void processSerialCommand(const String &command);
void updateAudioSource();
void applySourceRouting();
void logCurrentAudioSource();
void requestSourceRouting();
void requestAutoSourceSelection();
void applyAutomaticSourceSelection();
void checkSourceSelectionPins();
void forceSourceSelection();
void requestVolumeFromProcessor();
void handleVolumeHandshake();
void resetVolumeHandshake();

static bool isAuxPinForced() {
  return digitalRead(AUX_SELECT_PIN) == LOW;
}

static bool isBtPinForced() {
  return digitalRead(BT_SELECT_PIN) == LOW;
}

void requestSourceRouting() {
  pendingSourceRouting = true;
}

void requestAutoSourceSelection() {
  pendingAutoSourceSelection = true;
  pendingSourceRouting = true;
}

// Start/stop AUX I2S and disconnect BT when needed — single writer to output I2S
void applySourceRouting() {
  AudioInputSource src = audioSwitch.getSource();

  switch (src) {
    case SOURCE_BLUETOOTH:
      if (auxInput.isActive()) {
        auxInput.end();
        Serial.println("AUX I2S stopped (Bluetooth active)");
      }
      break;

    case SOURCE_AUX_INPUT:
      if (a2dp_sink.is_connected()) {
        Serial.println("Disconnecting BT (AUX source active)");
        a2dp_sink.disconnect();
      }
      if (audioSwitch.isAuxInputEnabled() && !auxInput.isActive()) {
        if (auxInput.begin(I2S_AUX_BCLK_PIN, I2S_AUX_LRCLK_PIN, I2S_AUX_DIN_PIN, 1)) {
          Serial.println("AUX I2S OK");
        } else {
          Serial.println("AUX I2S FAIL");
          audioSwitch.enableAuxInput(false);
        }
      }
      break;

    case SOURCE_TONE_GENERATOR:
      if (auxInput.isActive()) {
        auxInput.end();
      }
      if (a2dp_sink.is_connected()) {
        Serial.println("Disconnecting BT (tone generator active)");
        a2dp_sink.disconnect();
      }
      break;

    case SOURCE_NONE:
    default:
      if (auxInput.isActive()) {
        auxInput.end();
      }
      if (a2dp_sink.is_connected()) {
        a2dp_sink.disconnect();
      }
      break;
  }
}

void logCurrentAudioSource() {
  Serial.print("Current audio source: ");
  switch (audioSwitch.getSource()) {
    case SOURCE_BLUETOOTH:
      Serial.println("Bluetooth");
      break;
    case SOURCE_TONE_GENERATOR:
      Serial.println("Tone Generator");
      break;
    case SOURCE_AUX_INPUT:
      Serial.println("Auxiliary Input");
      break;
    case SOURCE_NONE:
      Serial.println("None");
      break;
    default:
      Serial.println("Unknown");
      break;
  }
}

// Bluetooth connection state callback
void connection_state_changed(esp_a2d_connection_state_t state, void *ptr) {
  if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
    Serial.println("BT+");
    bluetoothConnected = true;
    digitalWrite(LED_PIN, HIGH);

    if (!standaloneMode) {
      sendCommandToProcessor("bt:connected");
    }

    audioSwitch.updateBluetoothConnected(true);

    // Do not switch to BT when GPIO12 forces AUX (pins are mutually exclusive)
    if (!isAuxPinForced()) {
      if (!hardwareSourceOverride || isBtPinForced()) {
        audioSwitch.setSource(SOURCE_BLUETOOTH);
      }
    }

    a2dp_sink.set_volume(127);
    requestSourceRouting();
  } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
    Serial.println("BT-");
    bluetoothConnected = false;
    digitalWrite(LED_PIN, LOW);
    
    if (!standaloneMode) {
      sendCommandToProcessor("bt:disconnected");
    }
    
    audioSwitch.updateBluetoothConnected(false);

    if (!hardwareSourceOverride) {
      requestAutoSourceSelection();
    } else {
      requestSourceRouting();
    }
  }
}

// Bluetooth metadata callback
void metadata_callback(uint8_t id, const uint8_t *text) {
  switch (id) {
    case ESP_AVRC_MD_ATTR_TITLE:
      currentTrack = (const char *)text;
      Serial.println("Track: " + currentTrack);
      if (!standaloneMode) {
        sendCommandToProcessor("track:" + currentTrack);
      }
      break;
    case ESP_AVRC_MD_ATTR_ARTIST:
      currentArtist = (const char *)text;
      Serial.println("Artist: " + currentArtist);
      if (!standaloneMode) {
        sendCommandToProcessor("artist:" + currentArtist);
      }
      // Display combined info when we have both track and artist
      if (currentTrack.length() > 0 && currentArtist.length() > 0) {
        Serial.println("Now Playing: " + currentTrack + " - " + currentArtist);
      }
      break;
  }
}

// Load settings from persistent storage
void loadSettings() {
  preferences.begin("btaudio", false); // Open preferences in read/write mode
  
  // Load BT A2DP volume only in standalone mode (default full scale for iOS compatibility)
  // In dual mode, Audio Processor will control BT A2DP volume
  if (standaloneMode) {
    btA2DPVolume = preferences.getFloat("volume", 1.0);
  } else {
    btA2DPVolume = 1.0;
  }
  
  // Processor volume starts at 0.5 and will be updated via UART in dual mode
  processorVolume = 0.5;
  
  // Load audio enhancement setting (default off for best fidelity)
  enhanceAudioEnabled = preferences.getBool("enhance", false);
  audioPipeline.setEnhancementEnabled(enhanceAudioEnabled);
  
  // Load audio source settings
  bool toneEnabled = preferences.getBool("tone_en", false);
  bool auxEnabled = preferences.getBool("aux_en", false);

  // Load last active source (default to SOURCE_BLUETOOTH if not found)
  int lastSource = preferences.getInt("last_source", SOURCE_BLUETOOTH);
  
  preferences.end();
  
  Serial.println("Settings loaded from storage:");
  Serial.print("  BT A2DP Volume: ");
  Serial.println(btA2DPVolume);
  Serial.print("  Audio Enhancement: ");
  Serial.println(enhanceAudioEnabled ? "ON" : "OFF");
  Serial.print("  Tone Generator Enabled: ");
  Serial.println(toneEnabled ? "YES" : "NO");
  Serial.print("  Aux Input Enabled: ");
  Serial.println(auxEnabled ? "YES" : "NO");
  Serial.print("  Last Source: ");
  Serial.println(lastSource);
  
  // Apply loaded settings to audio switch
  audioSwitch.enableToneGenerator(toneEnabled);
  audioSwitch.enableAuxInput(auxEnabled);

  // Restore the last active source directly
  audioSwitch.setSourceDirect((AudioInputSource)lastSource);
  
  Serial.print("Source restored to: ");
  Serial.println((int)audioSwitch.getSource());
}

// Save settings to persistent storage
void saveSettings() {
  preferences.begin("btaudio", false); // Open preferences in read/write mode
  
  // Save volume only in standalone mode
  // In dual mode, Audio Processor handles volume persistence
  if (standaloneMode) {
    preferences.putFloat("volume", btA2DPVolume);
  }
  
  // Save audio enhancement setting
  preferences.putBool("enhance", enhanceAudioEnabled);
  
  // Save audio source settings
  preferences.putBool("tone_en", audioSwitch.isToneGeneratorEnabled());
  preferences.putBool("aux_en", audioSwitch.isAuxInputEnabled());
  
  // Save current active source
  preferences.putInt("last_source", (int)audioSwitch.getSource());
  
  preferences.end();
  
  Serial.println("Settings saved to storage");
  Serial.print("Current source saved: ");
  Serial.println((int)audioSwitch.getSource());
}

void setup() {
  // 🔥 MAIN PRIORITY: Set critical output pins HIGH immediately
  pinMode(SIGNAL_PIN_21, OUTPUT);
  digitalWrite(SIGNAL_PIN_21, HIGH);
  
  // Disable RTC hold and prepare amplifier pin (Keep LOW initially to avoid pops)
  rtc_gpio_hold_dis((gpio_num_t)MAX98357A_SD_MODE);
  pinMode(MAX98357A_SD_MODE, OUTPUT);
  digitalWrite(MAX98357A_SD_MODE, LOW); 
  
  // Initialize NVS (needed for Bluetooth bonding persistence)
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // Initialize serial for debugging
  Serial.begin(115200);
  Serial.setTimeout(50);
  delay(500); // Give serial time to initialize
  
  // Disable RTC hold on pins (in case we woke from sleep)
  rtc_gpio_hold_dis((gpio_num_t)WAKE_BUTTON_PIN);

  // Check if we woke from deep sleep
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("BT Module: Woke from deep sleep via button press (GPIO27)");
    wokenFromSleep = true;
    
    // CRITICAL: Wait for button release to prevent immediate loop if button is still held
    // This also helps with Bluetooth reconnection stability
    pinMode(WAKE_BUTTON_PIN, INPUT_PULLUP);
    int releaseCount = 0;
    unsigned long releaseStart = millis();
    while (releaseCount < 10) { // 100ms stable HIGH
      if (digitalRead(WAKE_BUTTON_PIN) == HIGH) {
        releaseCount++;
      } else {
        releaseCount = 0;
      }
      delay(10);
      if (millis() - releaseStart > 3000) { // 3s safety timeout
        Serial.println("Warning: GPIO27 still LOW after 3s");
        break;
      }
    }
    Serial.println("GPIO27 released, proceeding with initialization");
    delay(500); // Additional stabilization delay
  } else {
    Serial.println("BT INIT (Normal Boot)");
  }
  
  delay(50);  // Small stabilization delay for logic pins
  
  // Initialize LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  
  // Initialize pins
  pinMode(MODE_SELECT_PIN, INPUT_PULLUP);
  pinMode(VOLUME_UP_PIN, INPUT_PULLUP);
  pinMode(VOLUME_DOWN_PIN, INPUT_PULLUP);
  
  // Initialize source selection pins
  pinMode(AUX_SELECT_PIN, INPUT_PULLUP);
  pinMode(BT_SELECT_PIN, INPUT_PULLUP);
  
  // Set operating mode based on mode select pin
  standaloneMode = digitalRead(MODE_SELECT_PIN) == HIGH;
  Serial.println(standaloneMode ? "MODE:SOLO" : "MODE:DUAL");
  
  // Load settings from persistent storage
  loadSettings();

  // Check source selection pins before audio/BT init
  bool auxPinInit = digitalRead(AUX_SELECT_PIN);
  bool btPinInit = digitalRead(BT_SELECT_PIN);
  if (auxPinInit == LOW) {
    hardwareSourceOverride = true;
    audioSwitch.enableAuxInput(true);
    audioSwitch.setSourceDirect(SOURCE_AUX_INPUT);
    Serial.println("Initial source: Forced AUX");
  } else if (btPinInit == LOW) {
    hardwareSourceOverride = true;
    audioSwitch.enableBluetooth(true);
    audioSwitch.setSourceDirect(SOURCE_BLUETOOTH);
    Serial.println("Initial source: Forced BT");
  } else {
    hardwareSourceOverride = false;
    Serial.println("Initial source: Auto (Rules based)");
    applyAutomaticSourceSelection();
  }

  toneGenerator.begin();

  // Start AUX I2S only when it is the active source (avoids BT connect conflicts)
  if (audioSwitch.getSource() == SOURCE_AUX_INPUT && audioSwitch.isAuxInputEnabled()) {
    if (auxInput.begin(I2S_AUX_BCLK_PIN, I2S_AUX_LRCLK_PIN, I2S_AUX_DIN_PIN, 1)) {
      Serial.println("AUX I2S OK");
    } else {
      Serial.println("AUX I2S FAIL");
      audioSwitch.enableAuxInput(false);
    }
  }

  audioSwitch.begin();

  // I2S must be running BEFORE Bluetooth starts (critical for iPhone streaming)
  auto cfg = out.defaultConfig(TX_MODE);
  cfg.copyFrom(info);
  cfg.pin_bck = I2S_BCLK_PIN;
  cfg.pin_ws = I2S_LRCLK_PIN;
  cfg.pin_data = I2S_DOUT_PIN;
  cfg.buffer_count = 8;
  cfg.buffer_size = 1024;

  if (out.begin(cfg)) {
    Serial.println("I2S OUT OK");
  } else {
    Serial.println("I2S OUT FAIL");
  }

  audioPipeline.attachOutput(&out);

  // Bluetooth — library I2S output (no custom stream reader)
  a2dp_sink.set_on_connection_state_changed(connection_state_changed);
  a2dp_sink.set_avrc_metadata_callback(metadata_callback);
  a2dp_sink.set_auto_reconnect(false);

  Serial.println("Starting Bluetooth...");
  a2dp_sink.start("VieilleRadio");
  Serial.println("BT START");

  applySourceRouting();
  logCurrentAudioSource();

  audioTaskBegin(SAMPLE_RATE, AUDIO_BUFFER_SIZE);

  if (!standaloneMode) {
    Serial2.end();
    delay(100);
    Serial2.begin(UART_BAUD_RATE, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    Serial2.setRxBufferSize(512);
    Serial2.setTimeout(50);

    while (Serial2.available()) {
      Serial2.read();
    }
  }

  // Print audio enhancement status
  Serial.print("AUDIO ENHANCE: ");
  Serial.println(enhanceAudioEnabled ? "ON" : "OFF");
  
  // If we woke from sleep, wait and then reactivate Bluetooth
  if (wokenFromSleep) {
    delay(1000); 
    Serial.println("Reactivating Bluetooth stack after deep sleep...");
    // Force a slightly longer wait for the radio stack to be ready
    delay(500);
    // Use reconnect() instead of activate() as activate() is not supported in this library version.
    // reconnect() will automatically check if there is a stored device address in NVS.
    Serial.println("Attempting to reconnect to last Bluetooth device...");
    a2dp_sink.reconnect();
    delay(500);
  }
  
  // Set initial volume only in standalone mode
  // In dual mode, wait for volume handshake with Audio Processor
  if (standaloneMode) {
    setVolume(btA2DPVolume, false);
  } else {
    Serial.println("Dual mode: waiting for volume from Audio Processor");
  }
  
  // 🔥 FINAL STEP: Wait 5 seconds before enabling amplifiers to avoid pops
  Serial.println("Waiting 5s for I2S stabilization before enabling amplifiers...");
  delay(5000);
  digitalWrite(MAX98357A_SD_MODE, HIGH);
  Serial.println("Amplifiers enabled (SD_MODE HIGH)");
  
  // Print available commands - DISABLED at startup to prevent serial buffer overflow
  // Use 'h' command in serial monitor to show help manually
  // showHelp();
}

void loop() {
  // Apply deferred routing outside BT stack callbacks (prevents vQueueDelete crashes)
  if (pendingSourceRouting) {
    pendingSourceRouting = false;
    if (pendingAutoSourceSelection) {
      pendingAutoSourceSelection = false;
      applyAutomaticSourceSelection();
    } else {
      applySourceRouting();
      logCurrentAudioSource();
    }
  }

  handleVolumeHandshake();
  
  // Check for physical button presses
  checkButtons();
  
  // Check source selection pins
  checkSourceSelectionPins();
  
  // Variables for non-blocking reading
  static String uartBuffer = "";
  static bool commandReady = false;
  static unsigned long lastUartActivity = 0;
  
  static String serialBuffer = "";
  static bool serialCommandReady = false;
  static unsigned long lastSerialActivity = 0;
  
  // Handle any commands from the processor - non-blocking approach
  // OPTIMIZED: Read all available characters to prevent buffer overflow
  while (!standaloneMode && Serial2.available() && !commandReady) {
    char c = Serial2.read();
    lastUartActivity = millis();
    
    if (c == '\n' || c == '\r') {
      if (uartBuffer.length() > 0) {
        commandReady = true;
      }
    } else if (c >= 32 && c <= 126) { // Printable characters only
      uartBuffer += c;
      
      // Prevent buffer overflow
      if (uartBuffer.length() > 100) {
        uartBuffer = "";
      }
    }
  }
  
  // Read serial input for commands - non-blocking approach
  while (Serial.available() && !serialCommandReady) {
    char c = Serial.read();
    lastSerialActivity = millis();
    
    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() > 0) {
        serialCommandReady = true;
      }
    } else if (c >= 32 && c <= 126) { // Printable characters only
      serialBuffer += c;
      
      // Prevent buffer overflow
      if (serialBuffer.length() > 100) {
        serialBuffer = "";
      }
    }
  }
  
  // Process command if ready
  if (commandReady && uartBuffer.length() > 0) {
    uartBuffer.trim();
    String currentCmd = uartBuffer;
    
    // CRITICAL: Clear flags BEFORE processing to prevent infinite loops if command handler returns
    uartBuffer = "";
    commandReady = false;
    
    // Debug: show complete command received
    Serial.print("[UART CMD] Processing: ");
    Serial.println(currentCmd);
    
    // Process the command
    if (currentCmd.startsWith("vol:")) {
      // BT A2DP volume command from processor (handshake)
      float vol = currentCmd.substring(4).toFloat();
      
      // Ensure volume is in valid range
      if (vol < 0.0) vol = 0.0;
      if (vol > 1.0) vol = 1.0;
      
      // Set BT A2DP volume directly WITHOUT sending back to processor
      btA2DPVolume = vol;
      
      // Apply to A2DP sink
      uint8_t espVol = vol * 127;
      a2dp_sink.set_volume(espVol);
      
      // Mark volume handshake as complete
      volumeHandshakeComplete = true;
      volumeRequested = false;
      Serial.print("BT A2DP volume handshake complete - volume set to: ");
      Serial.println(vol, 2);
      
      // Send confirmation back to processor
      sendCommandToProcessor("volume_ready");
    }
    else if (currentCmd == "enhance:on") {
      setEnhanceAudio(true);
    }
    else if (currentCmd == "enhance:off") {
      setEnhanceAudio(false);
    }
    else if (currentCmd.startsWith("tone:")) {
      // Process tone generator command
      String toneCmd = currentCmd.substring(5);
      if (toneCmd == "on") {
        audioSwitch.enableToneGenerator(true);
        audioSwitch.setSource(SOURCE_TONE_GENERATOR);
        updateAudioSource();
        saveSettings();
      } else if (toneCmd == "off") {
        audioSwitch.enableToneGenerator(false);
        toneGenerator.stop();
        applyAutomaticSourceSelection();
        saveSettings();
      } else {
        // Pass command to tone generator
        if (toneGenerator.parseCommand(toneCmd)) {
          // If tone command was valid, enable tone generator and switch to it
          audioSwitch.enableToneGenerator(true);
          audioSwitch.setSource(SOURCE_TONE_GENERATOR);
          updateAudioSource();
        }
      }
    }
    else if (currentCmd.startsWith("aux:")) {
      // Process aux input command
      String auxCmd = currentCmd.substring(4);
      if (auxCmd == "on") {
        audioSwitch.enableAuxInput(true);
        audioSwitch.setSource(SOURCE_AUX_INPUT);
        updateAudioSource();
        saveSettings();
      } else if (auxCmd == "off") {
        audioSwitch.enableAuxInput(false);
        applyAutomaticSourceSelection();
        saveSettings();
      }
    }
    else if (currentCmd.startsWith("source:")) {
      String sourceCmd = currentCmd.substring(7);
      if (sourceCmd == "bt") {
        audioSwitch.setSource(SOURCE_BLUETOOTH);
      } else if (sourceCmd == "tone") {
        audioSwitch.setSource(SOURCE_TONE_GENERATOR);
      } else if (sourceCmd == "aux") {
        audioSwitch.setSource(SOURCE_AUX_INPUT);
      } else if (sourceCmd == "none") {
        audioSwitch.setSource(SOURCE_NONE);
      }
      updateAudioSource();
    }
    else if (currentCmd == "reset_uart_request") {
      // Reset UART if requested by the processor
      Serial2.end();
      delay(50);
      Serial2.begin(UART_BAUD_RATE, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
      Serial2.setRxBufferSize(512);
      Serial2.setTimeout(50);
    }
    else if (currentCmd == "processor:ready") {
      // Audio processor is ready
      Serial.println("Audio processor is ready");
    }
    else if (currentCmd == "get_status") {
      // Audio processor is requesting status - respond silently
      sendCommandToProcessor("bt:" + String(bluetoothConnected ? "connected" : "disconnected"));
    }
    else {
      // Pass any unhandled commands to processSerialCommand()
      processSerialCommand(currentCmd);
    }
  }
  
  // Process serial command if ready
  if (serialCommandReady && serialBuffer.length() > 0) {
    serialBuffer.trim();
    String currentCmd = serialBuffer;
    
    // CRITICAL: Clear flags BEFORE processing
    serialBuffer = "";
    serialCommandReady = false;
    
    processSerialCommand(currentCmd);
  }
}

// Process serial commands
void processSerialCommand(const String &command) {
  Serial.print("Executing command: ");
  Serial.println(command);
  
  // Check for single-character commands first
  if (command.length() == 1) {
    char cmd = command.charAt(0);
    
    switch (cmd) {
      case 'h': // Help
        showHelp();
        blinkLED();
        return;
        
      case 's': // Status
        showStatus();
        blinkLED();
        return;
        
      case '+': // Volume up
        volumeUp();
        blinkLED();
        return;
        
      case '-': // Volume down
        volumeDown();
        blinkLED();
        return;
        
      case '0': // Set volume to 0%
      case '1': // Set volume to 10%
      case '2': // Set volume to 20%
      case '3': // Set volume to 30%
      case '4': // Set volume to 40%
      case '5': // Set volume to 50%
      case '6': // Set volume to 60%
      case '7': // Set volume to 70%
      case '8': // Set volume to 80%
      case '9': // Set volume to 90%
        setVolume((cmd - '0') * 0.1);
        blinkLED();
        return;
        
      case 'f': // Set volume to 100% (full)
        setVolume(1.0);
        blinkLED();
        return;
        
      case 'e': // Toggle audio enhancement
        setEnhanceAudio(!enhanceAudioEnabled);
        blinkLED();
        return;
        
      case 'v': // Request volume from Audio Processor (dual mode only)
        if (!standaloneMode) {
          resetVolumeHandshake();
          Serial.println("Manual volume request initiated");
        } else {
          Serial.println("Volume request only available in dual mode");
        }
        return;
    }
  }
  
  // Process multi-character commands
  
  // Tone generator commands
  if (command.startsWith("tone_")) {
    if (toneGenerator.parseCommand(command)) {
      // If tone command was valid, enable tone generator and switch to it
      audioSwitch.enableToneGenerator(true);
      audioSwitch.setSource(SOURCE_TONE_GENERATOR);
      updateAudioSource();
      Serial.println("Tone generator enabled: " + command);
    } else {
      Serial.println("Invalid tone command: " + command);
    }
    return;
  }
  
  // ToneGen on/off command
  if (command == "ToneGen on") {
    audioSwitch.enableToneGenerator(true);
    audioSwitch.setSource(SOURCE_TONE_GENERATOR);
    updateAudioSource();
    saveSettings();
    Serial.println("Tone generator enabled");
    return;
  }
  else if (command == "ToneGen off") {
    audioSwitch.enableToneGenerator(false);
    toneGenerator.stop();
    // Apply automatic source selection when disabling
    applyAutomaticSourceSelection();
    saveSettings();
    Serial.println("Tone generator disabled");
    return;
  }
  
  // Aux on/off command
  if (command == "Aux on") {
    audioSwitch.enableAuxInput(true);
    audioSwitch.setSource(SOURCE_AUX_INPUT);
    updateAudioSource();
    saveSettings();
    Serial.println("Auxiliary input enabled");
    return;
  }
  else if (command == "Aux off") {
    audioSwitch.enableAuxInput(false);
    applyAutomaticSourceSelection();
    saveSettings();
    Serial.println("Auxiliary input disabled");
    return;
  }
  
  // Source selection commands
  if (command == "source bt") {
    audioSwitch.setSource(SOURCE_BLUETOOTH);
    updateAudioSource();
    Serial.println("Source set to Bluetooth");
    return;
  }
  else if (command == "reconnect") {
    Serial.println("Manual reconnection requested...");
    a2dp_sink.reconnect();
    return;
  }
  else if (command == "source tone") {
    audioSwitch.setSource(SOURCE_TONE_GENERATOR);
    updateAudioSource();
    Serial.println("Source set to Tone Generator");
    return;
  }
  else if (command == "source aux") {
    audioSwitch.setSource(SOURCE_AUX_INPUT);
    updateAudioSource();
    Serial.println("Source set to Auxiliary Input");
    return;
  }

  // BT A2DP volume command from processor
  if (command.startsWith("set_bt_volume:")) {
    String volStr = command.substring(14); // Skip "set_bt_volume:"
    float btVol = volStr.toFloat();
    if (btVol < 0.0) btVol = 0.0;
    if (btVol > 1.0) btVol = 1.0;

    // Set BT A2DP volume directly (0-127 range)
    uint8_t espVol = btVol * 127;
    a2dp_sink.set_volume(espVol);

    // Store BT A2DP volume for display/status
    btA2DPVolume = btVol;

    Serial.println("BT A2DP volume set to: " + String(btVol));
    
    // In dual mode, notify Audio Processor so it can persist the change
    if (!standaloneMode) {
      sendCommandToProcessor("bt_volume:" + String(btVol, 2));
    }
    
    return;
  }
  
  // Audio Processor master volume update (for status display)
  if (command.startsWith("proc_volume:")) {
    Serial.println("[DEBUG] Processing proc_volume command: " + command);
    String volStr = command.substring(12); // Skip "proc_volume:"
    float procVol = volStr.toFloat();
    if (procVol < 0.0) procVol = 0.0;
    if (procVol > 1.0) procVol = 1.0;

    // Store Audio Processor master volume for display/status
    processorVolume = procVol;

    Serial.println("Audio Processor volume received: " + String(procVol, 2));
    return;
  }
  
  // Deep sleep command from Audio Processor
  if (command == "sleep") {
    Serial.println("*** SLEEP command received ***");
    
    // 0. 🔥 POWER OPTIMIZATION: Shutdown MAX98357A amplifiers IMMEDIATELY
    Serial.println("Shutting down amplifiers and signal pins...");
    
    // Set SIGNAL_PIN_21 LOW
    digitalWrite(SIGNAL_PIN_21, LOW);
    
    // Configure GPIO14 (SD_MODE) to stay LOW during sleep
    rtc_gpio_init((gpio_num_t)MAX98357A_SD_MODE);
    rtc_gpio_set_direction((gpio_num_t)MAX98357A_SD_MODE, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level((gpio_num_t)MAX98357A_SD_MODE, 0);
    rtc_gpio_hold_en((gpio_num_t)MAX98357A_SD_MODE);
    
    // Redundant hold
    gpio_hold_en((gpio_num_t)MAX98357A_SD_MODE);
    
    // 1. Acknowledge sleep command immediately
    if (!standaloneMode) {
      Serial2.println("ack:sleep");
      Serial2.flush();
    }
    
    // 2. Stop all audio processing and Bluetooth
    Serial.println("Stopping audio components...");
    audioTaskEnd();
    toneGenerator.stop();
    if (auxInput.isActive()) {
      auxInput.end();
    }
    
    Serial.println("Shutting down A2DP sink...");
    // 1. Disconnect current peer if connected
    // This triggers a clean disconnect or link-loss on the phone
    if (a2dp_sink.is_connected()) {
      Serial.println("Disconnecting from BT peer...");
      a2dp_sink.disconnect();
      
      // Wait for disconnection (max 2 seconds)
      unsigned long startDis = millis();
      while (a2dp_sink.is_connected() && (millis() - startDis < 2000)) {
        delay(50);
      }
    }
    
    // NOTE: We DON'T call a2dp_sink.end() because it wipes NVS connection data.
    // Instead, we manually disable the stack components.
    
    Serial.println("Stopping I2S output...");
    out.end();
    
    Serial.println("Uninstalling I2S drivers...");
    // Explicitly uninstall I2S drivers to stop clocks
    i2s_driver_uninstall(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_1);
    
    // 2.5 🔥 DEEP RADIO SHUTDOWN: Ensure BT and WiFi are fully off
    Serial.println("Disabling Bluetooth stack...");
    // CRITICAL: Order matters. Disable bluedroid THEN controller.
    // We removed manual deinit calls as they might cause hangs when called out of sync.
    esp_bluedroid_disable();
    
    Serial.println("Disabling BT controller...");
    esp_bt_controller_disable();
    
    Serial.println("Stopping WiFi...");
    esp_wifi_stop();
    
    Serial.println("Radio shutdown complete.");
    delay(100);
    
    // 4. Detach UART (needed for clean sleep)
    Serial2.end();
    
    // 5. Configure wake button (GPIO27) as INPUT with pullup
    pinMode(WAKE_BUTTON_PIN, INPUT_PULLUP);
    
    // 6. Wait for button to be released (optional but recommended for clean wakeup)
    Serial.println("Waiting for GPIO27 release (HIGH) before sleeping...");
    
    int stableCount = 0;
    unsigned long startWait = millis();
    unsigned long lastLog = 0;
    
    while (stableCount < 5) { // 50ms of stable HIGH
      bool pinState = digitalRead(WAKE_BUTTON_PIN);
      
      if (pinState == HIGH) {
        stableCount++;
      } else {
        stableCount = 0; 
      }
      
      if (millis() - lastLog > 500) {
        Serial.print("GPIO27 state: ");
        Serial.println(pinState == HIGH ? "HIGH" : "LOW");
        lastLog = millis();
      }
      
      delay(10);
      
      // Safety timeout (2 seconds)
      if (millis() - startWait > 2000) {
        Serial.println("GPIO27 still LOW, entering sleep anyway...");
        break;
      }
    }
    
    Serial.println("Configuring wakeup and entering deep sleep...");
    
    // 7. Configure RTC wakeup
    rtc_gpio_init((gpio_num_t)WAKE_BUTTON_PIN);
    rtc_gpio_set_direction((gpio_num_t)WAKE_BUTTON_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis((gpio_num_t)WAKE_BUTTON_PIN);
    rtc_gpio_pullup_en((gpio_num_t)WAKE_BUTTON_PIN);
    
    esp_sleep_enable_ext0_wakeup((gpio_num_t)WAKE_BUTTON_PIN, 0); // Wake on LOW
    
    Serial.println("Goodnight!");
    Serial.flush();
    delay(100);
    
    // 8. Enter deep sleep
    esp_deep_sleep_start();
    
    // Failure recovery
    Serial.println("ERROR: Deep sleep failed!");
    digitalWrite(SIGNAL_PIN_21, HIGH);
    rtc_gpio_hold_dis((gpio_num_t)MAX98357A_SD_MODE);
    pinMode(MAX98357A_SD_MODE, OUTPUT);
    digitalWrite(MAX98357A_SD_MODE, HIGH);
    Serial2.begin(UART_BAUD_RATE, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    Serial2.setRxBufferSize(512);
    Serial2.setTimeout(50);
    return;
  }
  
  // Wake command from Audio Processor
  if (command == "wake") {
    Serial.println("*** WAKE command received from Audio Processor ***");
    Serial.println("BT Module is now awake and operational");
    
    // Acknowledge wake command
    if (!standaloneMode) {
      Serial2.println("ack:wake");
    }
    
    // Send current Bluetooth status to Audio Processor
    if (!standaloneMode) {
      sendCommandToProcessor("bt:" + String(bluetoothConnected ? "connected" : "disconnected"));
    }
    
    return;
  }


  // If we get here, command was not recognized
  Serial.println("Unknown command: " + command);
}

// Update the active audio source based on the current state
void updateAudioSource() {
  applySourceRouting();
  logCurrentAudioSource();
}

// Apply automatic source selection based on rules
void applyAutomaticSourceSelection() {
  AudioInputSource newSource = audioSwitch.applyRules();
  audioSwitch.setSource(newSource);

  Serial.print("Automatic source selection: ");
  switch (newSource) {
    case SOURCE_BLUETOOTH:
      Serial.println("Bluetooth");
      break;
    case SOURCE_TONE_GENERATOR:
      Serial.println("Tone Generator");
      break;
    case SOURCE_AUX_INPUT:
      Serial.println("Auxiliary Input");
      break;
    case SOURCE_NONE:
      Serial.println("None");
      break;
    default:
      Serial.println("Unknown");
      break;
  }

  updateAudioSource();
}

// Set audio enhancement state
void setEnhanceAudio(bool enabled, bool saveToPrefs) {
  enhanceAudioEnabled = enabled;
  audioPipeline.setEnhancementEnabled(enabled);
  Serial.print("AUDIO ENHANCE: ");
  Serial.println(enhanceAudioEnabled ? "ON" : "OFF");
  
  // Send to processor if in dual mode
  if (!standaloneMode) {
    sendCommandToProcessor("enhance:" + String(enhanceAudioEnabled ? "on" : "off"));
  }
  
  // Save to preferences if requested
  if (saveToPrefs) {
    saveSettings();
  }
}

// Check physical buttons
void checkButtons() {
  // Check volume up button
  if (digitalRead(VOLUME_UP_PIN) == LOW) {
    unsigned long currentTime = millis();
    if (currentTime - lastVolumeUpPress > DEBOUNCE_TIME) {
      lastVolumeUpPress = currentTime;
      volumeUp();
    }
  }
  
  // Check volume down button
  if (digitalRead(VOLUME_DOWN_PIN) == LOW) {
    unsigned long currentTime = millis();
    if (currentTime - lastVolumeDownPress > DEBOUNCE_TIME) {
      lastVolumeDownPress = currentTime;
      volumeDown();
    }
  }

  // Check wake/sleep button (GPIO27)
  bool wakeButtonDown = (digitalRead(WAKE_BUTTON_PIN) == LOW);
  unsigned long currentTime = millis();

  if (wakeButtonDown && !wakeButtonWasDown) {
    // Button just pressed
    Serial.println("GPIO27 button pressed");
    wakeButtonDownTime = currentTime;
    wakeButtonWasDown = true;
  } 
  else if (wakeButtonDown && wakeButtonWasDown) {
    // Button still held
    if (currentTime - wakeButtonDownTime > LONG_PRESS_TIME) {
      Serial.println("GPIO27 long press detected (2s) - entering sleep...");
      processSerialCommand("sleep");
      wakeButtonWasDown = false; // Reset to avoid multiple triggers
    }
  }
  else if (!wakeButtonDown && wakeButtonWasDown) {
    // Button released
    Serial.println("GPIO27 button released");
    wakeButtonWasDown = false;
  }
}

// Volume up function
void volumeUp() {
  // Increase volume by 0.05 (5%)
  btA2DPVolume += 0.05;
  if (btA2DPVolume > 1.0) btA2DPVolume = 1.0;
  
  setVolume(btA2DPVolume);
  Serial.print("VOL+: ");
  Serial.println(btA2DPVolume, 2);
}

// Volume down function
void volumeDown() {
  // Decrease volume by 0.05 (5%)
  btA2DPVolume -= 0.05;
  if (btA2DPVolume < 0.0) btA2DPVolume = 0.0;
  
  setVolume(btA2DPVolume);
  Serial.print("VOL-: ");
  Serial.println(btA2DPVolume, 2);
}

// Set volume to a specific level (0.0 to 1.0)
void setVolume(float volume, bool saveToPrefs) {
  // Ensure volume is in valid range
  if (volume < 0.0) volume = 0.0;
  if (volume > 1.0) volume = 1.0;
  
  // Store the BT A2DP volume
  btA2DPVolume = volume;
  
  if (standaloneMode) {
    // Convert 0-1 range to ESP32 BT volume range (0-127)
    uint8_t espVol = volume * 127;
    a2dp_sink.set_volume(espVol);
    Serial.print("VOL: ");
    Serial.println(volume, 2);
  } else {
    // Send volume to processor
    char volStr[10];
    sprintf(volStr, "%.2f", volume);
    sendCommandToProcessor("set_volume:" + String(volStr));
    Serial.print("VOL: ");
    Serial.println(volume, 2);
  }
  
  // Save to preferences if requested and in standalone mode
  // In dual mode, let Audio Processor handle volume persistence
  if (saveToPrefs && standaloneMode) {
    saveSettings();
  }
}

// Show current status
void showStatus() {
  Serial.println("STATUS:");
  Serial.println("Mode: " + String(standaloneMode ? "Standalone" : "Dual"));
  Serial.println("Bluetooth: " + String(bluetoothConnected ? "Connected" : "Disconnected"));
  
  // Volume display
  Serial.println("BT A2DP Volume: " + String(btA2DPVolume, 2) + " (" + String(int(btA2DPVolume * 100)) + "%)");
  if (!standaloneMode) {
    // In dual mode, show all three volume levels
    Serial.println("Audio Processor Volume: " + String(processorVolume, 2) + " (" + String(int(processorVolume * 100)) + "%)");
    float globalVolume = btA2DPVolume * processorVolume;
    Serial.println("Global Volume (BT × Processor): " + String(globalVolume, 2) + " (" + String(int(globalVolume * 100)) + "%)");
    Serial.println("Volume Handshake: " + String(volumeHandshakeComplete ? "Complete" : "Pending"));
  }
  
  Serial.println("Audio Enhancement: " + String(enhanceAudioEnabled ? "ON" : "OFF"));
  
  // Show audio source status
  Serial.print("Active Source: ");
  switch (audioSwitch.getSource()) {
    case SOURCE_BLUETOOTH:
      Serial.println("Bluetooth");
      break;
    case SOURCE_TONE_GENERATOR:
      Serial.println("Tone Generator");
      break;
    case SOURCE_AUX_INPUT:
      Serial.println("Auxiliary Input");
      break;
    case SOURCE_NONE:
      Serial.println("None");
      break;
    default:
      Serial.println("Unknown");
      break;
  }
  
  Serial.println("Tone Generator: " + String(audioSwitch.isToneGeneratorEnabled() ? "Enabled" : "Disabled"));
  Serial.println("Auxiliary Input: " + String(audioSwitch.isAuxInputEnabled() ? "Enabled" : "Disabled"));
  
  if (currentTrack.length() > 0) {
    Serial.println("Track: " + currentTrack);
  }
  
  if (currentArtist.length() > 0) {
    Serial.println("Artist: " + currentArtist);
  }
  
  Serial.print("Pin 14 (AMP SD): ");
  Serial.println(digitalRead(MAX98357A_SD_MODE) == HIGH ? "HIGH (Active)" : "LOW (Shutdown)");
}

// Show help information
void showHelp() {
  Serial.println("COMMANDS:");
  Serial.println("h - Show this help");
  Serial.println("s - Show status");
  Serial.println("+ - BT A2DP volume up");
  Serial.println("- - BT A2DP volume down");
  Serial.println("0-9 - Set BT A2DP volume (0% to 90%)");
  Serial.println("f - Set BT A2DP volume to 100%");
  Serial.println("set_bt_volume:X.X - Set BT A2DP volume (0.0-1.0, for dual mode)");
  Serial.println("e - Toggle audio enhancement");
  if (!standaloneMode) {
    Serial.println("v - Request volume from Audio Processor");
    Serial.println("NOTE: BT volume is multiplied with Processor volume for final output");
  }
  Serial.println("");
  Serial.println("Audio Source Commands:");
  Serial.println("ToneGen on/off - Enable/disable tone generator");
  Serial.println("Aux on/off - Enable/disable auxiliary input");
  Serial.println("source bt/tone/aux - Select audio source");
  Serial.println("");
  Serial.println("Hardware Source Selection:");
  Serial.println("Ground pin 12 - Force AUX input");
  Serial.println("Ground pin 13 - Force Bluetooth input");
  Serial.println("Both pins high - Automatic source selection");
  Serial.println("");
  Serial.println("Tone Generator Commands:");
  Serial.println("tone_1khz - Generate 1kHz sine wave at -18dBFS");
  Serial.println("tone_sweep - Generate 20Hz to 20kHz sweep over 10 seconds");
  Serial.println("tone_white - Generate white noise at -24dBFS");
  Serial.println("tone_pink - Generate pink noise at -24dBFS");
  Serial.println("tone_stop - Stop tone generation");
  Serial.println("");
  Serial.println("Custom Tone Commands:");
  Serial.println("tone_sine:freq:level - Generate sine wave with custom frequency (Hz) and level (dBFS)");
  Serial.println("  Example: tone_sine:440:-12 - Generate 440Hz at -12dBFS");
  Serial.println("tone_sweep:start:end:duration:level - Generate custom sweep");
  Serial.println("  Example: tone_sweep:100:1000:5:-18 - Sweep 100Hz to 1kHz over 5 seconds at -18dBFS");
  Serial.println("tone_noise:type:level - Generate noise (type: white or pink)");
  Serial.println("  Example: tone_noise:pink:-20 - Generate pink noise at -20dBFS");
}

// Blink LED to indicate command received
void blinkLED() {
  digitalWrite(LED_PIN, HIGH);
  delay(50);
  digitalWrite(LED_PIN, bluetoothConnected ? HIGH : LOW);
}

// Send command to processor ESP32
void sendCommandToProcessor(const String &command) {
  if (!standaloneMode && Serial2) {
    Serial2.println(command);
    Serial2.flush();
    // Debug output
    Serial.print("[UART TX] Sent: ");
    Serial.println(command);
  }
}

// Check source selection pins and force source if pins are grounded
void checkSourceSelectionPins() {
  static bool lastAuxPinState = HIGH;
  static bool lastBtPinState = HIGH;
  static unsigned long lastPinCheck = 0;
  
  // Check pins every 50ms to avoid excessive checking
  if (millis() - lastPinCheck < 50) {
    return;
  }
  lastPinCheck = millis();
  
  bool auxPinState = digitalRead(AUX_SELECT_PIN);
  bool btPinState = digitalRead(BT_SELECT_PIN);
  
  // Check for AUX pin grounding (HIGH to LOW transition or sustained LOW)
  if (auxPinState == LOW) {
    hardwareSourceOverride = true;
    if (audioSwitch.getSource() != SOURCE_AUX_INPUT) {
      Serial.println("AUX pin grounded - forcing AUX input");
      audioSwitch.enableAuxInput(true);
      audioSwitch.setSource(SOURCE_AUX_INPUT);
      updateAudioSource();
      saveSettings();
    }
  }
  
  // Check for Bluetooth pin grounding (HIGH to LOW transition or sustained LOW)
  else if (btPinState == LOW) {
    hardwareSourceOverride = true;
    if (audioSwitch.getSource() != SOURCE_BLUETOOTH) {
      Serial.println("BT pin grounded - forcing Bluetooth input");
      audioSwitch.enableBluetooth(true);
      audioSwitch.setSource(SOURCE_BLUETOOTH);
      updateAudioSource();
      saveSettings();
    }
  }
  
  // If neither pin is grounded, use automatic source selection
  else if (auxPinState == HIGH && btPinState == HIGH) {
    // Only switch to automatic mode if we were previously in a forced state
    if (hardwareSourceOverride || lastAuxPinState == LOW || lastBtPinState == LOW) {
      Serial.println("No pins grounded - using automatic source selection");
      hardwareSourceOverride = false;
      forceSourceSelection(); // Apply automatic rules
    }
  }
  
  lastAuxPinState = auxPinState;
  lastBtPinState = btPinState;
}

// Force source selection based on automatic rules when no pins are grounded
void forceSourceSelection() {
  applyAutomaticSourceSelection();
}

// Request volume from Audio Processor
void requestVolumeFromProcessor() {
  if (standaloneMode) {
    return; // No need for handshake in standalone mode
  }
  
  Serial.println("Requesting volume from Audio Processor...");
  sendCommandToProcessor("request_volume");
  volumeRequested = true;
  volumeRequestTime = millis();
}

// Handle volume handshake process
void handleVolumeHandshake() {
  if (standaloneMode || volumeHandshakeComplete) {
    return; // No handshake needed
  }

  static unsigned long requestStart = 0; // First request timestamp (does not reset on retries)
  static unsigned long bootTime = millis(); // Record boot time once
  unsigned long currentTime = millis();

  // Wait for initial delay before sending first volume request (give Audio Processor time to boot)
  if (!volumeRequested && (currentTime - bootTime < VOLUME_REQUEST_INITIAL_DELAY)) {
    return; // Still waiting for initial delay
  }

  // If we haven't requested volume yet, send initial request
  if (!volumeRequested) {
    Serial.println("Initial delay complete - starting volume handshake");
    requestVolumeFromProcessor();            // Sets volumeRequested = true and volumeRequestTime = millis()
    requestStart = currentTime;              // Mark start time once
    return;
  }

  // Timeout based on first request timestamp
  if (requestStart > 0 && (currentTime - requestStart >= VOLUME_REQUEST_TIMEOUT)) {
    Serial.println("Volume request timeout - using default volume");
    volumeHandshakeComplete = true;          // Give up and use default
    volumeRequested = false;
    requestStart = 0;                        // Reset for future handshakes
    return;
  }

  // Retry based on last retry time without resetting the overall timeout window
  if (currentTime - volumeRequestTime >= VOLUME_REQUEST_RETRY_INTERVAL) {
    Serial.println("Retrying volume request...");
    // Send request without printing the initial request message again
    sendCommandToProcessor("request_volume");
    volumeRequestTime = currentTime;         // Update last retry time
  }
}

// Reset volume handshake (for reconnection scenarios)
void resetVolumeHandshake() {
  volumeHandshakeComplete = false;
  volumeRequested = false;
  volumeRequestTime = 0;
  Serial.println("Volume handshake reset");
}
