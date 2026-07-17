// glim — a joystick-controlled 3-channel LED dimmer on an ATtiny814.
//
//   up / down     ramp the selected channel brighter / dimmer (speed follows
//                 how far you push)
//   left / right  select which of the 3 channels you're controlling; the newly
//                 selected channel blinks once so you know which light answered
//   tap switch    toggle the selected channel on / off (fades)
//   hold switch   all channels off (fades)
//
// Signal flow per channel:
//   setpoint level  → slewed display level (soft transitions)
//                   → gamma curve (perceptually linear)
//                   → high-res duty → dithering ISR (sub-LSB resolution)
//                   → TCA0 split-mode PWM on PA3/PA4/PA5.
//
// The PWM is driven straight from TCA0 rather than analogWrite(), because the
// LED pins (PA3/PA4/PA5 = WO3/WO4/WO5) only exist as timer outputs in split
// mode. millis() lives on TCD0, so TCA0 is ours to take over.

#include <Arduino.h>
#include <EEPROM.h>
#include <avr/wdt.h>
#include <util/atomic.h>
#include "config.h"

#if GLIM_STATUS_PIXEL
#include <tinyNeoPixel_Static.h>
#endif

#define DITHER_STEPS (1u << DITHER_BITS)          // sub-frames per dither cycle
#define HR_MAX       ((uint16_t)255 << DITHER_BITS) // full-scale high-res duty
#define FADE_SLEW    (((int32_t)255 << 8) / FADE_MS) // display slew, level<<8 per ms

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static uint8_t  level[NUM_CHANNELS];    // setpoint brightness 0..255 (0 = dark)
static bool     muted[NUM_CHANNELS];    // toggled off, but remembers its level
static int32_t  levelAcc[NUM_CHANNELS]; // ramp accumulator, level << 8
static int32_t  disp[NUM_CHANNELS];     // slewed display level, level << 8
static uint8_t  selected = 0;           // channel the joystick is steering

// Shared with the dither ISR — high-res duty target per channel (0..HR_MAX).
static volatile uint16_t dutyHR[NUM_CHANNELS];

static int16_t  centreX = 512;          // joystick centres, auto-measured at boot
static int16_t  centreY = 512;

static uint32_t lastTick = 0;
static bool     dirty = false;          // state changed since last EEPROM save
static uint32_t dirtyAt = 0;

// EEPROM layout: magic + version so future firmware can migrate rather than
// wipe the saved scene.
#define EE_MAGIC   0x676C               // "gl"
#define EE_VERSION 1
struct Persist {
  uint16_t magic;
  uint8_t  version;
  uint8_t  selected;
  uint8_t  level[NUM_CHANNELS];
  uint8_t  muted[NUM_CHANNELS];
};

// ---------------------------------------------------------------------------
// PWM (TCA0 split mode) + dithering
// ---------------------------------------------------------------------------

// Logical brightness → high-res duty. Square law (~gamma 2.0) so equal joystick
// travel gives roughly equal perceived change, lifted off zero by PWM_MIN_DUTY
// so the lowest lit step is actually visible on the PT4115.
static uint16_t gammaHR(uint8_t lvl) {
  if (lvl == 0) return 0;
  const uint16_t minHR = (uint16_t)PWM_MIN_DUTY << DITHER_BITS;
  const uint16_t maxHR = HR_MAX;
  return minHR + (uint16_t)(((uint32_t)(maxHR - minHR) * lvl * lvl) / (255UL * 255UL));
}

static inline void writeHCMP(uint8_t ch, uint8_t v) {
  switch (ch) {
    case 0: TCA0.SPLIT.HCMP0 = v; break; // PA3 / WO3
    case 1: TCA0.SPLIT.HCMP1 = v; break; // PA4 / WO4
    case 2: TCA0.SPLIT.HCMP2 = v; break; // PA5 / WO5
  }
}

static inline void setHR(uint8_t ch, uint16_t hr) {
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { dutyHR[ch] = hr; }
}

// Fires at the high-byte timer's BOTTOM, once per PWM period (~976 Hz). Renders
// each channel's high-res duty to the 8-bit compare register, spreading the
// fractional part over time via first-order sigma-delta.
ISR(TCA0_HUNF_vect) {
  static uint8_t ditherAcc[NUM_CHANNELS];
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    uint16_t hr   = dutyHR[ch];
    uint8_t  base = hr >> DITHER_BITS;
    uint8_t  frac = hr & (DITHER_STEPS - 1);
    uint8_t  a    = ditherAcc[ch] + frac;
    uint8_t  carry = 0;
    if (a >= DITHER_STEPS) { a -= DITHER_STEPS; carry = 1; }
    ditherAcc[ch] = a;
    uint16_t out = (uint16_t)base + carry;
    if (out > 255) out = 255;
    writeHCMP(ch, (uint8_t)out);
  }
  TCA0.SPLIT.INTFLAGS = TCA_SPLIT_HUNF_bm;
}

static void pwmInit() {
  takeOverTCA0();                                  // core stops managing TCA0
  PORTA.DIRSET = PIN3_bm | PIN4_bm | PIN5_bm;      // WO3/WO4/WO5 as outputs

  TCA0.SPLIT.CTRLA = 0;                             // stop before reconfiguring
  TCA0.SPLIT.CTRLD = TCA_SPLIT_SPLITM_bm;          // two 8-bit timers
  TCA0.SPLIT.CTRLB = TCA_SPLIT_HCMP0EN_bm |        // enable WO3/WO4/WO5, which
                     TCA_SPLIT_HCMP1EN_bm |        // takes the pins away from
                     TCA_SPLIT_HCMP2EN_bm;         // the PORT output register
  TCA0.SPLIT.HPER = 255;
  TCA0.SPLIT.LPER = 255;
  TCA0.SPLIT.HCMP0 = 0;
  TCA0.SPLIT.HCMP1 = 0;
  TCA0.SPLIT.HCMP2 = 0;
  TCA0.SPLIT.INTFLAGS = TCA_SPLIT_HUNF_bm;         // clear stale flag
  TCA0.SPLIT.INTCTRL = TCA_SPLIT_HUNF_bm;          // dither ISR each period
  TCA0.SPLIT.CTRLA = PWM_CLKSEL | TCA_SPLIT_ENABLE_bm;
}

// ---------------------------------------------------------------------------
// Rendering: setpoint → slewed display → high-res duty (ISR does the rest)
// ---------------------------------------------------------------------------

static void renderChannel(uint8_t ch) {
  setHR(ch, gammaHR((uint8_t)(disp[ch] >> 8)));
}

// Glide each channel's display level toward its target (0 when muted). Fast
// enough to track the live joystick ramp without lag, slow enough that a
// toggle or boot restore reads as a gentle fade.
static void slewAndRender(uint32_t dtMs) {
  int32_t step = FADE_SLEW * (int32_t)dtMs;
  if (step <= 0) step = 1;
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    int32_t target = (int32_t)(muted[ch] ? 0 : level[ch]) << 8;
    int32_t d = target - disp[ch];
    if (d > step)       disp[ch] += step;
    else if (d < -step) disp[ch] -= step;
    else                disp[ch]  = target;
    renderChannel(ch);
  }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

static void markDirty() { dirty = true; dirtyAt = millis(); }

static void loadState() {
  Persist p;
  EEPROM.get(0, p);
  if (p.magic == EE_MAGIC && p.version == EE_VERSION && p.selected < NUM_CHANNELS) {
    selected = p.selected;
    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
      level[i] = p.level[i];
      muted[i] = p.muted[i];
    }
  } else {
    // Blank/unrecognised EEPROM: come up with a gentle default so a freshly
    // installed light does something the moment it gets power.
    selected = 0;
    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
      level[i] = DEFAULT_LEVEL;
      muted[i] = false;
    }
  }
  for (uint8_t i = 0; i < NUM_CHANNELS; i++) levelAcc[i] = (int32_t)level[i] << 8;
}

static void saveState() {
  Persist p;
  p.magic = EE_MAGIC;
  p.version = EE_VERSION;
  p.selected = selected;
  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    p.level[i] = level[i];
    p.muted[i] = muted[i];
  }
  EEPROM.put(0, p);   // .put uses .update internally → only changed bytes wear
  dirty = false;
}

// ---------------------------------------------------------------------------
// Status pixel — colour says which channel the joystick is steering.
// ---------------------------------------------------------------------------

#if GLIM_STATUS_PIXEL
static uint8_t pixelBuf[3];
static tinyNeoPixel statusPixel = tinyNeoPixel(1, STATUS_PIXEL_PIN, NEO_GRB, pixelBuf);

// Only pushed when it actually changes: the WS2812 bit-bang runs with
// interrupts off for ~30 µs, and there's no reason to nudge the dither ISR more
// often than necessary.
static void updateStatusPixel(bool force) {
  static uint8_t lastSel = 0xFF;
  static bool    lastIdle = false;

  bool idle = true;
  for (uint8_t c = 0; c < NUM_CHANNELS; c++)
    if (!muted[c] && level[c] > 0) { idle = false; break; }

  if (!force && selected == lastSel && idle == lastIdle) return;
  lastSel = selected;
  lastIdle = idle;

  static const uint32_t colours[NUM_CHANNELS] = {
    STATUS_COLOR_CH1, STATUS_COLOR_CH2, STATUS_COLOR_CH3
  };
  uint32_t c = colours[selected];
  uint16_t scale = idle ? STATUS_BRIGHT_IDLE : STATUS_BRIGHT;
  statusPixel.setPixelColor(0,
    (uint8_t)(((uint16_t)((c >> 16) & 0xFF) * scale) / 255),
    (uint8_t)(((uint16_t)((c >>  8) & 0xFF) * scale) / 255),
    (uint8_t)(((uint16_t)( c        & 0xFF) * scale) / 255));
  statusPixel.show();
}
#else
static void updateStatusPixel(bool) {}
#endif

// If the joystick button is held at power-on, wipe saved state back to
// defaults. All channels swell up together as a "charging" cue while held; a
// flash confirms the wipe. Released early → cancelled, normal boot. Runs after
// pwmInit() (so it can drive the LEDs) and before loadState() (so the wipe
// takes effect). The watchdog isn't running yet, so the long hold is safe.
static void setAllHR(uint16_t hr) {
  for (uint8_t c = 0; c < NUM_CHANNELS; c++) setHR(c, hr);
}

static void factoryResetCheck() {
  if (digitalRead(JOY_SW_PIN) != LOW) return;      // not held → normal boot

  uint32_t t0 = millis();
  while (digitalRead(JOY_SW_PIN) == LOW) {
    uint32_t held = millis() - t0;
    if (held >= FACTORY_HOLD_MS) {
      Persist blank;                                // invalidate saved state
      blank.magic = 0xFFFF;
      EEPROM.put(0, blank);
      for (uint8_t f = 0; f < 6; f++) {             // confirm: flash all channels
        setAllHR((f & 1) ? 0 : gammaHR(DEFAULT_LEVEL));
        delay(110);
      }
      setAllHR(0);
      return;                                       // loadState() → defaults
    }
    setAllHR(gammaHR((uint8_t)((held * 255) / FACTORY_HOLD_MS)));  // swell up
    delay(5);
  }
  setAllHR(0);                                      // released early → cancel
}

// ---------------------------------------------------------------------------
// Feedback: blink the selected channel so the user sees which one they picked.
// Only the selected channel is disturbed; the others hold their dithered PWM.
// ---------------------------------------------------------------------------

static void ackBlink(uint8_t ch) {
  bool lit = !muted[ch] && level[ch] > 0;
  for (uint8_t i = 0; i < 2; i++) {
    // If the light is on, dip it; if it's off, pulse it up — either way it
    // visibly "answers".
    setHR(ch, lit ? 0 : gammaHR(DEFAULT_LEVEL));
    delay(80);
    setHR(ch, gammaHR((uint8_t)(disp[ch] >> 8)));   // back to current display
    delay(90);
  }
}

// ---------------------------------------------------------------------------
// Switch: debounced, distinguishes tap from hold.
// ---------------------------------------------------------------------------

static bool     swPressed = false;
static uint32_t swChangedAt = 0;
static bool     swLongFired = false;

static void handleSwitch() {
  bool raw = (digitalRead(JOY_SW_PIN) == LOW);   // active-low
  uint32_t now = millis();

  if (raw != swPressed) {
    if (now - swChangedAt < SW_DEBOUNCE_MS) return; // bounce
    swPressed = raw;
    swChangedAt = now;
    if (swPressed) {
      swLongFired = false;                         // new press begins
    } else if (!swLongFired) {
      // Released before the long-press threshold → tap: toggle selected.
      bool lit = !muted[selected] && level[selected] > 0;
      if (lit) {
        muted[selected] = true;                    // off, remembering level
      } else {
        muted[selected] = false;                   // on
        if (level[selected] == 0) {
          level[selected] = DEFAULT_LEVEL;
          levelAcc[selected] = (int32_t)DEFAULT_LEVEL << 8;
        }
      }
      markDirty();
    }
    return;
  }

  // Held past the threshold → all off.
  if (swPressed && !swLongFired && now - swChangedAt >= SW_LONGPRESS_MS) {
    swLongFired = true;
    for (uint8_t i = 0; i < NUM_CHANNELS; i++) muted[i] = true;
    markDirty();
  }
}

// ---------------------------------------------------------------------------
// Joystick axes
// ---------------------------------------------------------------------------

static int16_t deflection(uint8_t pin, int16_t centre, bool invert) {
  int16_t d = (int16_t)analogRead(pin) - centre;
  return invert ? (int16_t)-d : d;
}

// Up/down: ramp the selected channel, speed proportional to deflection.
static void handleBrightness(uint32_t dtMs) {
  int16_t y = deflection(JOY_Y_PIN, centreY, JOY_Y_INVERT);
  int16_t mag = (y < 0 ? -y : y) - JOY_DEADZONE;
  if (mag <= 0) return;

  // step (in level<<8 units) ≈ mag * dt * gain, chosen so full deflection
  // (~390 past the deadzone) sweeps 0..255<<8 in RAMP_FULL_MS.
  const int32_t gainNum = ((int32_t)255 << 8);
  int32_t step = ((int32_t)mag * (int32_t)dtMs * gainNum) /
                 ((int32_t)390 * RAMP_FULL_MS);
  if (step == 0) step = 1;

  int32_t &acc = levelAcc[selected];
  if (y > 0) { acc += step; if (acc > (255 << 8)) acc = (255 << 8); }
  else       { acc -= step; if (acc < 0) acc = 0; }

  level[selected] = (uint8_t)(acc >> 8);
  muted[selected] = false;              // actively adjusting un-mutes
  markDirty();
}

// Left/right: edge-triggered channel select with re-arming, plus ack blink.
static bool xArmed = true;

static void handleSelect() {
  int16_t x = deflection(JOY_X_PIN, centreX, JOY_X_INVERT);
  int16_t ax = x < 0 ? -x : x;

  if (xArmed && ax > JOY_X_THRESH) {
    if (x > 0) selected = (selected + 1) % NUM_CHANNELS;
    else       selected = (selected + NUM_CHANNELS - 1) % NUM_CHANNELS;
    xArmed = false;
    ackBlink(selected);
    markDirty();
  } else if (!xArmed && ax < JOY_X_REARM) {
    xArmed = true;
  }
}

// ---------------------------------------------------------------------------
// Boot / loop
// ---------------------------------------------------------------------------

static void calibrateCentre() {
  // Assumes the stick is released at power-on. Average a few reads.
  int32_t sx = 0, sy = 0;
  const uint8_t n = 16;
  for (uint8_t i = 0; i < n; i++) {
    sx += analogRead(JOY_X_PIN);
    sy += analogRead(JOY_Y_PIN);
    delay(4);
  }
  centreX = sx / n;
  centreY = sy / n;
}

void setup() {
  pinMode(JOY_SW_PIN, INPUT_PULLUP);
#if GLIM_STATUS_PIXEL
  pinMode(STATUS_PIXEL_PIN, OUTPUT);   // required by the _Static variant
#endif
  pwmInit();

#if GLIM_DEBUG
  Serial.begin(115200);
  Serial.println(F("glim boot"));
#endif

#if GLIM_FACTORY_RESET
  factoryResetCheck();
#endif
  calibrateCentre();
  loadState();
  updateStatusPixel(true);

  // Soft-start: displays begin at 0 and glide up to the restored scene.
  for (uint8_t i = 0; i < NUM_CHANNELS; i++) disp[i] = 0;
  uint32_t t0 = millis(), prev = t0;
  while (millis() - t0 <= (uint32_t)FADE_MS + 40) {
    uint32_t now = millis();
    slewAndRender(now - prev);
    prev = now;
    delay(2);
  }

  ackBlink(selected);          // show which channel is active at startup

#if GLIM_WATCHDOG
  _PROTECTED_WRITE(WDT.CTRLA, WDT_PERIOD_2KCLK_gc);   // ~2 s
#endif
  lastTick = millis();
}

void loop() {
  uint32_t now = millis();
  uint32_t dt = now - lastTick;
  lastTick = now;

  handleBrightness(dt);
  handleSelect();
  handleSwitch();
  slewAndRender(dt);
  updateStatusPixel(false);

  if (dirty && (now - dirtyAt) >= EEPROM_SAVE_DELAY_MS) saveState();

#if GLIM_WATCHDOG
  wdt_reset();
#endif

#if GLIM_DEBUG
  static uint32_t dbg = 0;
  if (now - dbg > 250) {
    dbg = now;
    Serial.print(F("ch=")); Serial.print(selected);
    Serial.print(F(" lvl=")); Serial.print(level[selected]);
    Serial.print(F(" mute=")); Serial.print(muted[selected]);
    Serial.print(F(" x=")); Serial.print(analogRead(JOY_X_PIN));
    Serial.print(F(" y=")); Serial.println(analogRead(JOY_Y_PIN));
  }
#endif
}
