# Controls

glim is meant to be operable in the dark, by feel, by someone who has never seen
the code. One joystick, four gestures.

## The gestures

| Gesture | Effect |
|---------|--------|
| **Push up / down** | Ramp the *selected* channel brighter / dimmer. Push a little for fine control, push all the way for a fast sweep. |
| **Flick left / right** | Move to the previous / next channel (wraps around all 3). The channel you land on **blinks once** so you can see which light you're now controlling. |
| **Tap the stick (SW)** | Toggle the selected channel on / off. Turning it off remembers the level; turning a never-used channel on gives it a default brightness. |
| **Hold the stick (SW)** | All channels off. A one-gesture "goodnight". |

At power-on the selected channel blinks once, so you always know where you are.

## How it feels, and why

- **Proportional ramp.** Brightness speed follows how far you push the stick, so
  you get both fine trimming (nudge) and quick changes (full push) from one axis
  without menus or modes.
- **Perceptually linear.** Duty is gamma-corrected (square law), so the ramp
  looks even top-to-bottom instead of doing everything in the last 10%.
- **Edge-triggered selection.** A flick changes the channel *once*; you have to
  let the stick return most of the way to centre before it will change again. No
  runaway scrolling.
- **The blink is the display.** With no screen, the acknowledge-blink is how the
  device tells you which channel it thinks you mean. Only the selected channel
  blinks; the others hold steady.
- **It remembers.** State is written to EEPROM a few seconds after you stop, and
  restored on boot — so a wall switch (or a power blip) brings the room back the
  way you left it, not black or blazing.

## Tuning

Everything lives in [`include/config.h`](../include/config.h). The knobs you're
most likely to touch:

| Setting | What it does |
|---------|--------------|
| `JOY_Y_INVERT`, `JOY_X_INVERT` | Flip an axis if up-is-dimmer or right-is-wrong on your build. Cheap joysticks vary in orientation — expect to set at least one of these. |
| `RAMP_FULL_MS` | Time for a full-range sweep at full push. Lower = snappier. |
| `JOY_DEADZONE` | How far you can wobble around centre before anything happens. Raise it if a channel drifts on its own. |
| `JOY_X_THRESH` / `JOY_X_REARM` | How hard you must flick to change channel, and how far back to centre before the next flick counts. |
| `DEFAULT_LEVEL` | Brightness a channel comes up at on first boot / first tap-on. |
| `PWM_MIN_DUTY` | The dimmest lit step. Raise it if low settings flicker or drop out on the PT4115. |
| `SW_LONGPRESS_MS` | How long "hold" is before it means all-off. |

### Calibration note

The stick's centre is measured automatically at power-on, so **leave the joystick
released while it boots**. If your resting readings drift, set `GLIM_DEBUG 1` in
`config.h`, reflash, and open `pio device monitor` (115200) to watch live X/Y
values while you find good deadzone/threshold numbers.
