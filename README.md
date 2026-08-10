# fan_target_pxp

Idle-aware fan curve + **ShellUI Mono HUD** (temps + RAM + FPS) + controller lightbar.

## Architecture
```
fan_target.elf
  ├─ fan curve / lightbar (this process)
  ├─ inject overlay_elf → SceShellUI
  │     Mono labels: CPU / SoC / SSD / RAM / FPS
  │     colours by threshold (configurable)
  │     UDP bind 127.0.0.1:29028
  └─ inject fps_elf → game
        Gnm flip hook → FPS → UDP only
```

Live data never touches the SSD. Only optional one-shot ELF embeds at **build** time.

Config path on console: **`/data/fan_target_pxp/config.ini`** (created on first run).

### Default temp colours
| Range | Colour |
|-------|--------|
| ≤50°C | Green |
| 51–60°C | Yellow |
| 61–70°C | Orange |
| ≥71°C | Red |

### Default FPS colours
| Range | Colour |
|-------|--------|
| ≤24 | Red |
| 25–29 | Yellow |
| 30–45 | Green-yellow |
| ≥46 | Cyan-green |

### Fan control
Set `fan_control=0` in the ini to disable the fan curve while keeping
overlays, FPS injection, and lightbar.

### Lightbar
Configurable bands + **flashing red** when ≥ `lightbar_flash_temp` (default 75 °C).
A dedicated thread updates the lightbar at ~400 ms so the flash is smooth
(independent of the 5 s fan poll).
`lightbar_duration_sec` defaults to **300** (5 minutes). Set `0` for always on.
All colours and thresholds are in `config.ini` / `config.ini.sample`.

On load a one-time notification is shown:  
`Greetings by Issu.` / `Fan target 1.0`

## Build

```sh
# One-shot (recommended after clone)
bash build.sh

# Or step by step:
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk   # or your SDK path
git submodule update --init --recursive       # pulls etahen for fps_elf sources
make fps_elf          # rebuilds from etahen submodule + auto-patches
make overlay_elf
make blob
make all              # → dist/fan_target.elf
```

Load **only** `dist/fan_target.elf`. It injects the rest.

## License

GPL-3.0


Version string is defined once as `FAN_TARGET_VERSION` in `main.c` and used for the
greeting notification and the default config header.


## Docker / Windows

```sh
docker build -t fan_target_pxp .
docker run --rm -v "$PWD/dist:/out" fan_target_pxp
```

Windows (Docker Desktop):

```powershell
powershell -ExecutionPolicy Bypass -File windows.ps1
```


## Config web UI

`fan_target.elf` serves a PixelPop-themed panel on **port 25500**:

```
http://<PS5_IP>:25500/
```

Save writes `/data/fan_target_pxp/config.ini`. **Reload on console** re-parses without restarting the process.

A websrv-oriented entry (icon + `homebrew.js`) is written under `/data/fan_target_pxp/homebrew/FanTargetPXP/` so it can be copied into `/data/homebrew/` if desired.

Placeholder icon: `assets/icon0.png` (replace with your final art).


## Home screen tile (Media)

On first run, `fan_target.elf` registers **Fan Target** under the **Media** tab:

| Field | Value |
|-------|--------|
| Title ID | `PXPFT0001` |
| Category | Web Based Media App (66048) |
| Deeplink | `http://127.0.0.1:25500/` |

Install path: `/user/app/PXPFT0001/` via `sceAppInstUtilAppInstallTitleDir` (same pattern as Payload Manager / ftpsrv). No PKG required. Requires jailbreak + AppInstUtil available.

Open the tile while `fan_target.elf` is running to reach the config panel.
