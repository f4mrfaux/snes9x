S-Pen Integration (RetroArch + SNES9x)

Purpose
- Define a clean contract between RetroArch and SNES9x for stylus (S‑Pen) input.
- Prevent configuration conflicts by keeping RetroArch responsible for pointer semantics and SNES9x responsible for mapping to game actions.

RetroArch Responsibilities
- Normalize stylus events into libretro `RETRO_DEVICE_POINTER`:
  - `idx 0`: current stylus cursor coordinates
  - `idx 1`: tip contact (PRESSED when in contact)
  - `idx 2`: barrel button (PRESSED when held)
- Control hover semantics via RetroArch settings:
  - `input_stylus_enable`: master On/Off
  - `input_stylus_require_contact_for_click`: tap requires contact
  - `input_stylus_hover_moves_pointer`: hover updates pointer 0 without PRESSED
- Provide phantom hover→tap prevention and keep pointer-only (no mouse button emulation).

SNES9x Responsibilities
- Consume `RETRO_DEVICE_POINTER` (never synthesize RETRO_MOUSE buttons) and map to either SNES Mouse or Lightgun depending on core options.
- Do not duplicate RetroArch’s hover/contact logic; rely on pointer coordinates + PRESSED state received from RetroArch.
- Keep per-port state (filters, last coordinates, relative anchors) so players do not stomp each other.

Core Options (SNES9x)
- `snes9x_mouse_mode`: Legacy mouse/touch path. Absolute only applies when S-Pen is inactive; otherwise S-Pen coordinate mode is used.
- `snes9x_spen_tap_action`: What a tap (tip contact) does; default Left Click.
- `snes9x_spen_barrel_action`: What the barrel/side button does; default Right Click.
- `snes9x_spen_hover_behavior`: Visual/logic hint for hover; default Cursor Only. Does not assert PRESSED; hover motion is controlled by RetroArch.
- `snes9x_spen_coordinate_mode`: Stylus coordinate mode; default Absolute (Relative converts absolute stylus to deltas).
- `snes9x_spen_input_mode`: Auto/Mouse/Lightgun; default Auto. Auto only activates S-Pen paths when stylus signals are present (tip/barrel/hover).
- `snes9x_spen_advanced_filtering`: Disabled/Basic/Enhanced smoothing per port.

Expected Behaviors
- Mario Paint (Mouse):
  - Hover moves cursor (enable RetroArch `input_stylus_hover_moves_pointer`).
  - Tap → Left Click; Barrel → Right Click.
  - If S-Pen is absent, legacy absolute touch uses pointer coords; otherwise S-Pen absolute path is used.

- Super Scope/Lightgun titles:
  - Hover moves crosshair (enable RetroArch `input_stylus_hover_moves_pointer`).
  - Tap/Barrel mapped to Trigger/Reload per core options.

Notes
- S-Pen uses `RETRO_DEVICE_POINTER` exclusively (idx0 coords, idx1 tip, idx2 barrel). We do not gate cursor updates on pressure; clicks come from PRESSED flags.
- Legacy absolute mouse path only runs when S-Pen is inactive; traditional relative mouse remains intact.
- If hover movement is disabled in RetroArch, pointer coords will not change during hover; this is by design to avoid conflicts.
