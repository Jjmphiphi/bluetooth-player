# Repository Guidelines

## Project Structure & Module Organization
- **ESP32_Aux_Tone_BT_Enhanced.ino**: Main entry point. Coordinates the Bluetooth A2DP sink, I2S output, and custom audio components.
- **AudioSwitch.cpp/h**: Manages audio source selection (Bluetooth, AUX, Tone Generator). Implements priority rules and persistent state storage using the `Preferences` library.
- **ToneGenerator.cpp/h**: Synthesis engine for high-quality audio tones (Sine, Frequency Sweep, White/Pink Noise).
- **AuxInput.cpp/h**: Handles auxiliary audio input via a secondary I2S interface.
- **AudioPipeline.cpp/h**: Unified I2S output for tone/AUX; optional enhancement (off by default).
- **AudioTask.cpp/h**: FreeRTOS task on core 1 for non-Bluetooth sources.
- **GPIO 12 / GPIO 13**: Mutually exclusive hardware source select (ground one pin only). AUX stops BT I2S via disconnect; BT stops AUX I2S. Routing runs in `loop()`, not in BT callbacks.

## Build, Test, and Development Commands
- **Environment**: Standard Arduino project. Best developed using **Arduino IDE** or **VS Code with Arduino extension**.
- **Serial Communication**: Use **115200 baud** for debug logs and UART commands.
- **Hardware Controls**:
  - **Pin 12 (AUX_SELECT_PIN)**: Ground to force AUX input.
  - **Pin 13 (BT_SELECT_PIN)**: Ground to force Bluetooth input.
  - **Pin 27 (WAKE_BUTTON_PIN)**: RTC GPIO used for waking the ESP32 from deep sleep.
- **Commands**: Supports UART commands like `enhance_on`, `enhance_off`, and `status`.

## Coding Style & Naming Conventions
- **Language**: C++ (Arduino flavor).
- **Naming**:
  - Functions and variables use `camelCase` (e.g., `loadSettings`, `bluetoothConnected`).
  - Classes use `PascalCase` (e.g., `AudioSwitch`, `ToneGenerator`).
  - Macros and enum constants use `UPPER_CASE` with underscores (e.g., `I2S_BCLK_PIN`, `SOURCE_BLUETOOTH`).
- **Encapsulation**: Private class members are prefixed with an underscore (e.g., `_currentSource`).

## Testing Guidelines
- **Manual Testing**: Verify audio enhancement states using UART commands.
- **Hardware Verification**: Test source switching logic by grounding selection pins (12/13).
- **Persistence**: Ensure settings and active sources are correctly restored after power cycles or deep sleep transitions.
