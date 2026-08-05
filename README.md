# PS5 Fan Target (dynamic / idle-aware fork)

Fork of [drakmor/fan_target](https://github.com/drakmor/fan_target) with:

1. **Idle (5 min average, 3 °C hysteresis)**  
   - enter idle when 5‑minute average **< 50 °C**  
   - leave idle when average **≥ 53 °C**  
   While idle, the system’s own fan target is left alone (not overbearing).

2. **Fan curve** (near system default when cool):

| System temp | Target |
|-------------|--------|
| ≤ 40 °C     | 91 °C  |
| 52 °C       | 88 °C  |
| 58 °C       | 84 °C  |
| 64 °C       | 75 °C  |
| 70 °C       | 72 °C  |
| 75 °C       | 70 °C  |
| ≥ 85 °C     | 68 °C  |

Linear interpolation between anchors. **3 °C target hysteresis** reduces ioctl chatter.

3. **Controller lightbar** (best-effort, first **~3 minutes** only):

| Temp        | Colour |
|-------------|--------|
| < 53 °C     | Blue   |
| 55–62 °C    | Green  |
| 64–70 °C    | Orange |
| ≥ 72 °C     | Red    |

Gaps (53–54, 63, 71) keeps the previous colour.  
May only work on the home screen; games/ShellCore often override. Failures are silent.

4. Poll every **5 s**. Status log every **2 min** (klog/console only — not SSD).

## Build

Requires [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk).

```sh
make clean all
# → dist/fan_target.elf
```

Links: `-lkernel_sys -lScePad -lSceUserService`

## Notes

Derivative of drakmor’s `fan_target` (GPL-3.0).

The original automatic fan controller still decides actual fan speed; this payload only adjusts the *target temperature*.


## Start automatically

With **Any Autoloader**, copy `fan_target.elf` into the payload directory and add to `autoload.txt`:

```ini
!100
fan_target.elf
```

## Original project

Derivative of drakmor’s `fan_target` (GPL-3.0). Upstream provides fixed-target builds (85/80/75/70/65 °C).
