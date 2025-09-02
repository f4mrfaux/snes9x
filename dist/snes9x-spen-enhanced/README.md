# Snes9x S-Pen Enhanced Core for Android RetroArch

This is a modified version of the Snes9x libretro core with comprehensive Samsung S-Pen support for Android devices.

## Features

### S-Pen Enhancements
- **Full RetroArch RETRO_DEVICE_POINTER compliance** - Works through standard RetroArch pointer interface
- **Configurable tap and barrel button actions** - Map stylus tip and side button to various mouse/lightgun actions
- **Hover support** - Detect stylus proximity for cursor movement and lightgun tracking
- **Coordinate system options** - Choose between absolute (direct positioning) and relative (mouse-like) modes
- **Auto-detection** - Automatically detect S-Pen usage vs traditional input methods
- **Legacy compatibility** - Preserves existing multi-touch functionality

### Core Options Added
- `snes9x_spen_tap_action`: Configure S-Pen tap behavior
  - Options: left_click, right_click, middle_click, trigger, reload, disabled
- `snes9x_spen_barrel_action`: Configure S-Pen barrel button behavior  
  - Options: left_click, right_click, middle_click, trigger, reload, disabled
- `snes9x_spen_hover_behavior`: Configure hover behavior
  - Options: cursor, active_cursor, lightgun_tracking, disabled  
- `snes9x_spen_coordinate_mode`: Choose coordinate system
  - Options: absolute, relative
- `snes9x_spen_input_mode`: Select input detection mode
  - Options: auto, mouse, lightgun

## Installation

### Method 1: Manual Installation
1. Copy `snes9x_libretro_android.so` to your RetroArch cores directory:
   - Usually `/Android/data/com.retroarch/files/cores/`
2. Copy `snes9x_libretro_android.info` to your RetroArch info directory:
   - Usually `/Android/data/com.retroarch/files/info/`
3. Restart RetroArch
4. The core will appear as "Nintendo - SNES / SFC (Snes9x) [S-Pen Enhanced]"

### Method 2: ADB Installation
```bash
adb push snes9x_libretro_android.so /sdcard/Android/data/com.retroarch/files/cores/
adb push snes9x_libretro_android.info /sdcard/Android/data/com.retroarch/files/info/
```

## Configuration

1. Load a SNES game with the S-Pen Enhanced core
2. Go to Quick Menu → Options  
3. Configure S-Pen settings according to your preference:
   - For lightgun games: Set input mode to "lightgun", hover to "lightgun_tracking"
   - For mouse games: Set input mode to "mouse", coordinate mode to "absolute"
   - For auto-detection: Leave input mode on "auto"

## Recommended Settings

### For Lightgun Games (Super Scope, etc.)
- Input Mode: `auto` or `lightgun`
- Coordinate Mode: `absolute`  
- Tap Action: `trigger`
- Barrel Action: `reload`
- Hover Behavior: `lightgun_tracking`

### For Mouse Games (Mario Paint, etc.)
- Input Mode: `auto` or `mouse`
- Coordinate Mode: `absolute`
- Tap Action: `left_click`
- Barrel Action: `right_click`  
- Hover Behavior: `cursor`

## Technical Details

This core uses RetroArch's RETRO_DEVICE_POINTER system with the following enhancements:

- **Virtual pointer system**: Barrel button is exposed as an additional pointer (pointer_count > 1)
- **Hover detection**: Stylus proximity is detected when pointer coordinates exist but pressed=false
- **Coordinate transformation**: Proper mapping from libretro pointer coordinates to SNES screen space
- **Dual compatibility**: Works with both S-Pen and traditional touch input simultaneously

## Compatibility

- **Requires**: RetroArch with S-Pen input support (included in unified features build)
- **Architecture**: ARM64 (aarch64) Android devices
- **Android Version**: API 21+ (Android 5.0+)
- **S-Pen Support**: Samsung Galaxy Note series and compatible stylus devices

## Build Information

- **Core Version**: Snes9x 1.62.3 with S-Pen enhancements
- **Build Date**: $(date +%Y-%m-%d)
- **Architecture**: ARM64-v8a
- **Compiler**: Android NDK r25c

## Known Issues

- S-Pen detection requires proper RetroArch input driver support
- Hover behavior may vary between different Samsung device models
- Some older SNES games may not support mouse/lightgun input

## Support

For issues related to S-Pen functionality, ensure you're using the compatible RetroArch build with S-Pen input driver support.

For general Snes9x core issues, refer to the upstream Snes9x documentation.