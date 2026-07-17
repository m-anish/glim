// config.h — glim hardware map and behaviour tunables.
//
// Everything you'd want to adjust for a given build lives here. The pin
// choices are not arbitrary: on the ATtiny814 only PA3/PA4/PA5 can emit a
// TCA0 waveform (PWM), and only PA1/PA2 among the free pins can read the ADC.
// See docs/hardware.md for the datasheet reasoning.

#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Pin map (ATtiny814, 14-pin SOIC)
// ---------------------------------------------------------------------------

// LED channels → PT4115 PWM/DIM inputs. These MUST stay on PA3/PA4/PA5:
// they are TCA0 split-mode outputs WO3/WO4/WO5 (HCMP0/HCMP1/HCMP2).
#define LED1_PIN   PIN_PA3   // TCA0 WO3 / HCMP0
#define LED2_PIN   PIN_PA4   // TCA0 WO4 / HCMP1
#define LED3_PIN   PIN_PA5   // TCA0 WO5 / HCMP2

// Joystick. X/Y must be ADC-capable pins; PA1=AIN1, PA2=AIN2.
#define JOY_X_PIN  PIN_PA1   // ADC AIN1  (left/right → channel select)
#define JOY_Y_PIN  PIN_PA2   // ADC AIN2  (up/down    → brightness)
#define JOY_SW_PIN PIN_PA7   // digital, active-low with internal pull-up

// WS2812 status pixel — plain GPIO, any free pin. (PB0/PB1/PB2/PB3 remain free;
// the IR receiver goes on PB0.)
#define STATUS_PIXEL_PIN PIN_PA6

#define NUM_CHANNELS 3

// ---------------------------------------------------------------------------
// PWM
// ---------------------------------------------------------------------------

// TCA0 split clock divider. At F_CPU=16 MHz, DIV64 → 16e6/64/256 ≈ 976 Hz.
// Lower the frequency (e.g. DIV256 ≈ 244 Hz) if you need cleaner deep dimming
// from the PT4115 at the very bottom of the range; raise it if you notice
// stroboscopic shimmer. Must be one of the TCA_SPLIT_CLKSEL_DIVn_gc values.
#define PWM_CLKSEL TCA_SPLIT_CLKSEL_DIV64_gc

// Smallest non-zero duty (out of 255). The PT4115 can't cleanly resolve a
// 1-count on-time at ~1 kHz, so we floor the lit range a little above zero.
#define PWM_MIN_DUTY 3

// ---------------------------------------------------------------------------
// Brightness model
// ---------------------------------------------------------------------------

// Logical brightness is 0..255 per channel and is gamma-corrected to PWM duty
// so the joystick feels linear to the eye. DEFAULT_LEVEL is what an untouched
// channel comes up at on first-ever boot, and what a tap gives a channel that
// has never been set.
#define DEFAULT_LEVEL 110

// Time (ms) to sweep a channel across its full range at full stick deflection,
// measured at the *top* of the range. Smaller = snappier, larger = more gentle.
// Partial deflection is proportional, so a gentle push is much slower than this.
#define RAMP_FULL_MS 12500

// Extra fine control when dim. The ramp runs this many times slower at the
// bottom of the range than at the top, easing smoothly in between — so you can
// trim a night-light by tiny amounts without the top end feeling glued. Set to
// 1 to disable and get a constant rate.
//
// With the defaults, a full 0→100% sweep at full push takes roughly 23 s; the
// bottom of the range crawls, the top moves. If the whole thing feels too slow,
// drop RAMP_FULL_MS first.
#define RAMP_LOW_FACTOR 4

// Soft transitions: on/off toggles, all-off, and the boot restore glide over
// this many ms instead of snapping. Fast enough that it doesn't lag the live
// joystick ramp; slow enough to feel gentle on a step change.
#define FADE_MS 250

// Temporal dithering: buys brightness resolution below the 8-bit PWM floor by
// nudging duty ±1 across frames (first-order sigma-delta), so deep dimming
// stops stair-stepping. DITHER_BITS extra bits, 0 disables. The dither pattern
// repeats at PWM_freq / 2^DITHER_BITS, so keep it high enough to stay invisible:
// at ~976 Hz, 2 bits → 244 Hz (safe), 3 → 122 Hz, 4 → 61 Hz (risks flicker).
#define DITHER_BITS 2

// ---------------------------------------------------------------------------
// Status pixel (WS2812)
// ---------------------------------------------------------------------------

// A single addressable LED showing which channel the joystick is steering —
// the one thing the per-channel indicator LEDs can't tell you, since they mirror
// level. When every channel is off it drops to a dim glow, so the stick is still
// findable in a dark room.
#define GLIM_STATUS_PIXEL 1

// Colour per channel, 0xRRGGBB.
#define STATUS_COLOR_CH1 0xFF2000   // amber
#define STATUS_COLOR_CH2 0x00FF30   // green
#define STATUS_COLOR_CH3 0x0040FF   // blue

// Pixel brightness (0..255): normal, and when all channels are off.
#define STATUS_BRIGHT      40
#define STATUS_BRIGHT_IDLE 4

// ---------------------------------------------------------------------------
// Joystick feel
// ---------------------------------------------------------------------------

// Readings are 10-bit (0..1023); centre is auto-measured at boot. These are
// deflections away from that measured centre.
#define JOY_DEADZONE   110   // ignore small wobble around centre
#define JOY_X_THRESH   320   // deflection that commits a channel change
#define JOY_X_REARM    90    // must fall back inside this before the next change

// If an axis feels backwards on your build, flip the matching invert. "Up =
// brighter" and "right = next channel" are the intended directions.
#define JOY_Y_INVERT   0
#define JOY_X_INVERT   0

// Switch timing.
#define SW_DEBOUNCE_MS   25
#define SW_LONGPRESS_MS  700   // hold this long → all channels off

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

// Levels/mute/selection are saved to EEPROM this long after the last change,
// so a wall-switch power cycle restores the previous scene without hammering
// the EEPROM on every tick.
#define EEPROM_SAVE_DELAY_MS 3000

// ---------------------------------------------------------------------------
// Reliability
// ---------------------------------------------------------------------------

// Watchdog: auto-reset if loop() ever wedges. It's an installed, unattended
// light — cheap insurance. Timeout is ~2 s; loop() and the boot sequence both
// finish well inside that.
#define GLIM_WATCHDOG 1

// Factory reset: power on with the joystick button held. All channels swell up
// together as you hold; once they hit full and flash, saved state is wiped back
// to defaults. Release before then to cancel. FACTORY_HOLD_MS is the hold time.
#define GLIM_FACTORY_RESET 1
#define FACTORY_HOLD_MS 2000

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------

// Set to 1 to stream joystick/level telemetry on USART0 (PB2=TX, PB3=RX) at
// 115200. Handy for calibration; leave at 0 for production.
//
// Guarded so it can be overridden at build time without editing this file —
// `utils/flash.sh --debug` does exactly that.
#ifndef GLIM_DEBUG
#define GLIM_DEBUG 0
#endif
