// glim — a joystick-controlled 3-channel LED dimmer on an ATtiny814.
//
//   up / down     ramp the selected channel brighter / dimmer (speed follows
//                 how far you push)
//   left / right  select which of the 3 channels you're controlling; the newly
//                 selected channel blinks once so you know which light answered
//   tap switch    toggle the selected channel on / off
//   hold switch   all channels off
//
// Levels are gamma-corrected for a linear-feeling ramp and persisted to EEPROM
// a few seconds after the last change, so a wall-switch power cycle restores
// the previous scene.
//
// The PWM is driven straight from TCA0 in split mode rather than analogWrite(),
// because the three LED pins (PA3/PA4/PA5 = WO3/WO4/WO5) only exist as timer
// outputs in split mode. millis() lives on TCD0, so TCA0 is ours to take over.

#include <Arduino.h>
#include <EEPROM.h>
#include "config.h"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static uint8_t  level[NUM_CHANNELS];    // logical brightness 0..255 (0 = dark)
static bool     muted[NUM_CHANNELS];    // toggled off, but remembers its level
static int32_t  levelAcc[NUM_CHANNELS]; // ramp accumulator, level << 8
static uint8_t  selected = 0;           // channel the joystick is steering

static int16_t  centreX = 512;          // auto-measured at boot
static int16_t  centreY = 512;

static uint32_t lastTick = 0;           // for ramp dt
static bool     dirty = false;          // state changed since last EEPROM save
static uint32_t dirtyAt = 0;            // when it last changed

// EEPROM layout: a magic marker so we can tell a blank chip from saved state.
#define EE_MAGIC 0x676C          // "gl"
struct Persist {
  uint16_t magic;
  uint8_t  selected;
  uint8_t  level[NUM_CHANNELS];
  uint8_t  muted[NUM_CHANNELS];
};

// ---------------------------------------------------------------------------
// PWM (TCA0 split mode)
// ---------------------------------------------------------------------------

// Map logical brightness → PWM duty. Square law (~gamma 2.0) so equal joystick
// travel gives roughly equal perceived brightness change, lifted off zero by
// PWM_MIN_DUTY so the lowest lit step is actually visible on the PT4115.
static uint8_t dutyFor(uint8_t lvl) {
  if (lvl == 0) return 0;
  uint32_t span = 255u - PWM_MIN_DUTY;
  return PWM_MIN_DUTY + (uint8_t)((span * lvl * lvl) / (255u * 255u));
}

static void pwmWrite(uint8_t ch, uint8_t duty) {
  switch (ch) {
    case 0: TCA0.SPLIT.HCMP0 = duty; break; // PA3 / WO3
    case 1: TCA0.SPLIT.HCMP1 = duty; break; // PA4 / WO4
    case 2: TCA0.SPLIT.HCMP2 = duty; break; // PA5 / WO5
  }
}

// Push a channel's logical level (respecting mute) out to the driver.
static void applyChannel(uint8_t ch) {
  pwmWrite(ch, muted[ch] ? 0 : dutyFor(level[ch]));
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
  TCA0.SPLIT.CTRLA = PWM_CLKSEL | TCA_SPLIT_ENABLE_bm;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

static void markDirty() {
  dirty = true;
  dirtyAt = millis();
}

static void loadState() {
  Persist p;
  EEPROM.get(0, p);
  if (p.magic == EE_MAGIC && p.selected < NUM_CHANNELS) {
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
  p.selected = selected;
  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    p.level[i] = level[i];
    p.muted[i] = muted[i];
  }
  EEPROM.put(0, p);   // .put uses .update internally → only changed bytes wear
  dirty = false;
}

// ---------------------------------------------------------------------------
// Feedback: blink the selected channel so the user sees which one they picked.
// Only the selected channel is disturbed; the others keep their PWM in hardware.
// ---------------------------------------------------------------------------

static void ackBlink(uint8_t ch) {
  bool lit = !muted[ch] && level[ch] > 0;
  for (uint8_t i = 0; i < 2; i++) {
    // If the light is on, dip it; if it's off, pulse it up — either way it
    // visibly "answers".
    pwmWrite(ch, lit ? 0 : dutyFor(DEFAULT_LEVEL));
    delay(80);
    applyChannel(ch);
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
      applyChannel(selected);
      markDirty();
    }
    return;
  }

  // Held past the threshold → all off.
  if (swPressed && !swLongFired && now - swChangedAt >= SW_LONGPRESS_MS) {
    swLongFired = true;
    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
      muted[i] = true;
      applyChannel(i);
    }
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

  // step (in level<<8 units) ≈ mag * dt * gain. Gain chosen so full deflection
  // (~390 past the deadzone) sweeps 0..255<<8 in RAMP_FULL_MS.
  //   full = (255<<8) / RAMP_FULL_MS per ms, spread over ~390 of magnitude.
  const int32_t gainNum = ((int32_t)255 << 8);
  int32_t step = ((int32_t)mag * (int32_t)dtMs * gainNum) /
                 ((int32_t)390 * RAMP_FULL_MS);
  if (step == 0) step = 1;

  int32_t &acc = levelAcc[selected];
  if (y > 0) { acc += step; if (acc > (255 << 8)) acc = (255 << 8); }
  else       { acc -= step; if (acc < 0) acc = 0; }

  level[selected] = (uint8_t)(acc >> 8);
  muted[selected] = false;              // actively adjusting un-mutes
  applyChannel(selected);
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
  pwmInit();

#if GLIM_DEBUG
  Serial.begin(115200);
  Serial.println(F("glim boot"));
#endif

  calibrateCentre();
  loadState();
  for (uint8_t i = 0; i < NUM_CHANNELS; i++) applyChannel(i);

  ackBlink(selected);          // show which channel is active at startup
  lastTick = millis();
}

void loop() {
  uint32_t now = millis();
  uint32_t dt = now - lastTick;
  lastTick = now;

  handleBrightness(dt);
  handleSelect();
  handleSwitch();

  if (dirty && (now - dirtyAt) >= EEPROM_SAVE_DELAY_MS) saveState();

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
