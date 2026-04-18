# Keyball44 ZMK Configuration

BLE wireless Keyball44 split keyboard running ZMK on SuperMini nRF52840 (nice!nano v2 compatible) with PMW3360 trackball and SSD1306 OLED displays.

## Hardware

- **MCU**: SuperMini nRF52840 (x2)
- **Trackball**: PMW3360 optical sensor (right half, polled -- no IRQ pin)
- **Display**: SSD1306 128x32 OLED (both halves, mounted sideways)
- **Battery**: 3.7V 130mAh 401030 LiPo (per half)
- **Split**: BLE wireless, right half is central

## Layout

44-key split with 7 layers:

| Layer | Index | Activation | Notes |
|-------|-------|------------|-------|
| KBR | 0 | Default | QWERTY base layer |
| NUM | 1 | Hold Delete | Numbers, arrows, BT profiles |
| SYM | 2 | Hold F | Symbols, mouse clicks (MB1-MB5) |
| FUN | 3 | Hold K | F-keys, mouse clicks, **trackball scroll mode** |
| MOUSE | 4 | -- | Arrows, page up/down, **trackball snipe mode** |
| SCROLL | 5 | -- | Transparent (reserved) |
| SNIPE | 6 | -- | Transparent (reserved) |

- Hold-tap: layer-tap 150ms balanced, mod-tap 170ms tap-preferred
- Soft-off combo: Home + End

## Trackball

- CPI 400, orientation 270, X inverted
- Scroll mode on FUN layer (hold K): divisor 10 for fine scroll control
- Snipe mode on MOUSE layer: divisor 4 for precision
- Adaptive polling: 60Hz active, 10Hz idle, 4Hz sleep
- Sensor force-wakes immediately when entering FUN layer (scroll)
- Aggressive REST downshift: RUN(100ms) -> REST1(320ms) -> REST2(3.2s) -> REST3

## Display

Custom rotated status screen (32x128 portrait canvas rotated 90 degrees to physical 128x32).

**Right half (central)** shows: battery icon + %, split pair status, BT profile, layer name, last key, caps/num lock.

**Left half (peripheral)** shows: battery icon + %, connection status.

### Power-saving display behavior (right half)

The OLED is **off by default** to save battery. It turns on when:
- NUM layer is active
- BT is not connected (to see pairing status)
- For 60 seconds after a BT connection or profile change
- For 60 seconds after boot

## Power Optimization

This build is extensively optimized for battery life on 130mAh cells. Key settings:

### Firmware

| Setting | Value | Purpose |
|---------|-------|---------|
| DC-DC converter | Enabled (standard only, HV disabled) | ~1.5mA savings |
| BT TX power | +8 dBm | Maximum reliability for split link |
| Deep sleep | 15 min (right), 5 min (left) | Left follows central to sleep faster |
| OLED idle blank | 10s (right), 5s (left) | Reduces display on-time |
| OLED contrast | 16/255 | Minimum visible brightness |
| LVGL tick | 250ms | Fewer CPU wakeups for static display |
| Serial/UART | Disabled | Saves ~600uA |
| NFC pins as GPIO | Enabled | Prevents leakage on SPI CS pin (P0.09) |
| Battery report | Every 120s | Fewer ADC + BLE wakeups |
| Split link interval | 15ms | Reduced radio duty (trackball is on central, unaffected) |
| PMW3360 REST modes | 100ms/320ms/3200ms | Maximally aggressive downshift to REST3 (~1mA) |

### Known hardware limitations

- **SuperMini regulator**: ~700uA quiescent current vs ~20uA on genuine nice!nano v2. Not firmware-fixable.
- **PMW3360 sensor**: 16-24mA active tracking, ~1mA in REST3. Gaming sensor not designed for battery power. PMW3610 draws ~2mA active but requires different breakout board.
- **Battery readings**: ZMK uses linear voltage-to-percentage mapping that doesn't match LiPo discharge curves. Percentages are approximate.
- **Peripheral sleep sync**: ZMK has no mechanism to synchronize sleep between central and peripheral (issue zmkfirmware/zmk#2408). Workaround: shorter sleep timeout on left half.

### Expected battery life (130mAh)

| Usage | Estimated Runtime |
|-------|-------------------|
| Continuous typing + trackball | ~5-6 hours |
| Mixed use (typing, reading, breaks) | ~7-9 hours |
| Idle (OLED blanked, BLE connected) | ~2-3 days |
| Deep sleep | ~8 days (SuperMini), ~270 days (nice!nano v2) |

## Building

Built via GitHub Actions. Requires ZMK v0.2 with the PMW3360 driver as an extra module.

### Local build

```bash
west build -s zmk/app -b nice_nano_v2 \
  -- -DZMK_CONFIG=/path/to/config \
     -DSHIELD=keyball44_left \
     -DZMK_EXTRA_MODULES=/path/to/pmw3360-driver

west build -s zmk/app -b nice_nano_v2 \
  -- -DZMK_CONFIG=/path/to/config \
     -DSHIELD=keyball44_right \
     -DZMK_EXTRA_MODULES=/path/to/pmw3360-driver
```

## File Structure

```
config/
  keyball44.conf              # Global config (both halves)
  keyball44.keymap            # Keymap with all layers
  boards/shields/keyball_nano/
    keyball44.dtsi            # Shared hardware (matrix, OLED, kscan)
    keyball44_left.conf       # Left-only overrides (shorter sleep/idle)
    keyball44_left.overlay    # Left column GPIOs
    keyball44_right.conf      # Right-only config (PMW3360, HID indicators)
    keyball44_right.overlay   # Right column GPIOs, SPI, trackball, input listener
    custom_status_screen.c    # Rotated OLED display with power management
    Kconfig.defconfig         # Shield defaults (split, display, LVGL)
    Kconfig.shield            # Shield detection
    CMakeLists.txt            # Conditionally includes custom display
    keyball44.zmk.yml         # Shield metadata
pmw3360-driver/               # PMW3360 Zephyr module (polling, no IRQ)
  src/
    pmw3360.c                 # Driver with adaptive polling + force_wake
    pmw3360.h                 # Register definitions
    pmw3360_priv.c            # SROM firmware blob
    pixart.h                  # Shared types, poll state definitions
  Kconfig                     # CPI, orientation, REST downshift configs
  CMakeLists.txt
  zephyr/module.yml
```
