# PS5 Fan Target (dynamic / idle-aware fork)

Fork of [drakmor/fan_target](https://github.com/drakmor/fan_target) with:

1. **Idle detection + 3 °C hysteresis** – 15-minute average temperature  
   - enter idle when avg **&lt; 55 °C**  
   - leave idle when avg **≥ 58 °C**  
   While idle the payload **does not fight** the system’s own fan target.
2. **Less polling** – every **5 s** (was 2 s); status log every **2 min**.
3. **Quiet + cool curve** – target is chosen from current temp (max of CPU / SoC) so the fan stays quieter when cool and ramps earlier under load (lower peaks).

### Curve (temp → target)

| System temp | Target |
|-------------|--------|
| ≤ 42 °C     | 91 °C  |
| 52 °C       | 84 °C  |
| 58 °C       | 78 °C  |
| 64 °C       | 72 °C  |
| 70 °C       | 67 °C  |
| 76 °C       | 63 °C  |
| ≥ 85 °C     | 60 °C  |

Linear interpolation between anchors. An extra **3 °C target hysteresis** avoids thrashing the controller near boundaries.

The original automatic fan controller still decides actual fan speed; this payload only adjusts the *target temperature*.

## SDK (no need to vendor binaries in your repo)

Use the official SDK from another repository instead of copying it into yours.

### Option A – system install (simplest)

```sh
wget https://github.com/ps5-payload-dev/sdk/releases/latest/download/ps5-payload-sdk.zip
sudo unzip -d /opt ps5-payload-sdk.zip
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
```

### Option B – git submodule (keeps SDK out of *your* tree as binary blobs)

```sh
git submodule add https://github.com/ps5-payload-dev/sdk.git sdk
git submodule update --init --recursive

# build/install the toolchain once into a local prefix
make -C sdk DESTDIR="$(pwd)/sdk-install" install
# (see upstream README for full deps: clang, lld, etc.)
```

The Makefile auto-detects, in order:

1. `./sdk/toolchain/prospero.mk` (if you installed in-tree)
2. `./sdk-install/toolchain/prospero.mk`
3. `/opt/ps5-payload-sdk`

So you never need to re-upload the SDK into your own repository.

## Build

```sh
make clean all
# → dist/fan_target.elf
```

### Compile-time overrides

| Macro                     | Default | Meaning                                      |
|---------------------------|---------|----------------------------------------------|
| `FANTARGET_POLL_MS`       | 5000    | Poll interval (ms)                           |
| `FANTARGET_LOG_SECONDS`   | 120     | Status log interval (s)                      |
| `FANTARGET_IDLE_ENTER_C`  | 55      | 15-min avg below this → enter idle           |
| `FANTARGET_HYSTERESIS_C`  | 3       | Idle exit = enter + this; also target hyst   |
| `FANTARGET_TARGET_HYST_C` | 3       | Min Δ before rewriting fan target            |
| `FANTARGET_HISTORY_SEC`   | 900     | Temperature history window (s)               |

Example:

```sh
make clean all CFLAGS="-std=c11 -Wall -Wextra -Werror -O2 -DFANTARGET_IDLE_ENTER_C=50 -DFANTARGET_HYSTERESIS_C=3"
```

## Start automatically

With **PLK Autoloader**, copy `fan_target.elf` into the payload directory and add to `autoload.txt`:

```ini
!100
fan_target.elf
```

## Log examples

```
[fan_target] started; quiet+cool curve, idle enter<55 C exit>=58 C (hyst=3 C)
[fan_target] poll=5000 ms, log every 120 s, target hyst=3 C
[fan_target] target set: 91 C -> 84 C (temp=53 C, avg15m=49 C, raw=83 C)
[fan_target] idle (avg15m=47 C < 55 C); leaving system target alone (exit when avg>=58 C)
[fan_target] status CPU=45 C, SoC=44 C, fan=12.0%, target=91 C (idle, not fighting), avg15m=47 C [IDLE]
[fan_target] left idle (avg15m=59 C >= 58 C); resuming curve control
```

## Original project

Derivative of drakmor’s `fan_target` (GPL-3.0). Upstream provides fixed-target builds (85/80/75/70/65 °C).
