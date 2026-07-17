# CLAUDE.md

Guidance for Claude Code (claude.ai/code) when working in this repository.

## Project

glim is a joystick-controlled 3-channel LED dimmer. An **ATtiny814** drives three
**PT4115** constant-current LED drivers via hardware PWM, and reads a cheap analog
thumb joystick for control. Purpose: simple, screenless, easily-adjustable home
lighting. It's the small sibling of `lokki` (campus-scale LED automation) in the
`starstucklab` family.

Firmware is C++ on **megaTinyCore**, built with **PlatformIO**, flashed over
**serialUPDI**.

## The pin map is load-bearing — do not "simplify" it

LEDs are on **PA3/PA4/PA5**, joystick on **PA1/PA2** (X/Y) + **PA7** (SW). This is
forced by silicon, not preference (datasheet DS40001912A, Table 5-1):

- **PA1/PA2 cannot do PWM.** No timer reaches them. TCA0 WO3/WO4/WO5 are hardwired
  to PA3/PA4/PA5; WO0/1/2 to PB0/1/2. PA1/PA2 are ADC pins (AIN1/AIN2).
- **PB2/PB3 cannot do ADC.** Only PB0/PB1 reach the ADC on PORTB.

If you're tempted to move a PWM channel to PA1/PA2 or an analog input to PB2/PB3,
it will silently not work. Re-read `docs/hardware.md` first. The full datasheet is
in `docs/`.

## PWM is bare-metal TCA0, not analogWrite()

`analogWrite()` can't drive WO3/WO4/WO5, so `pwmInit()` in `src/main.cpp`:

- calls `takeOverTCA0()` (safe: `millis()` is on TCD0, so TCA0 is free);
- runs TCA0 in **split mode** and writes duty to `HCMP0/HCMP1/HCMP2`
  (PA3/PA4/PA5), higher = brighter, ~976 Hz at 16 MHz.

Don't reintroduce `analogWrite()` on the LED pins, and don't put `millis()` on
TCA0 (it would break the PWM takeover).

## Everything tunable is in `include/config.h`

Pins, PWM frequency/floor, ramp speed, joystick deadzones/thresholds, axis
inversion, EEPROM save delay, debug flag. Prefer changing `config.h` over
hardcoding in `main.cpp`. The firmware is deliberately one file plus config so it
stays portable to a rev2 board or a different MCU.

## Build / flash workflow

Everything goes through `utils/flash.sh` — it auto-detects the USB-serial
programmer (`utils/find-port.sh`) so no port is hardcoded in `platformio.ini`.

```bash
utils/flash.sh                    # build + upload
utils/flash.sh --build            # compile only
utils/flash.sh --fuses            # once per fresh chip
utils/flash.sh --debug --monitor  # GLIM_DEBUG=1 build, upload, then console
utils/flash.sh --port /dev/... --slow   # override port / drop to 115200
```

`--debug` works by passing `-DGLIM_DEBUG=1`, which is why `GLIM_DEBUG` in
`config.h` is wrapped in `#ifndef` — keep that guard if you add build-time
toggles. Note `--monitor` needs the adapter's RX on **PB2**, not the UPDI node.

Never set `board_hardware.updipin` to anything but `updi` — it would lock UPDI
out of the chip.

## Verifying changes

There is **no host test suite** — this is firmware. Verification means flashing to
the ATtiny814 and driving the joystick. Compiling (`utils/flash.sh --build`)
catches syntax and register-name errors but not behaviour.

Cheap joysticks vary in orientation: after any change touching the axes, confirm
up=brighter and right=next-channel on real hardware, and flip `JOY_*_INVERT` if
not.

## Flash/RAM budget

ATtiny814 = 8 KB flash, 512 B SRAM. Keep it lean: avoid large lookup tables in
RAM (the brightness curve is computed, not tabled, on purpose), and keep `Serial`
behind `GLIM_DEBUG`.
