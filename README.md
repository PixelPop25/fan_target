# PS5 Fan Target

Idle-aware fan curve, lightbar, and on-screen FPS/temps overlay.

## How it works (etaHEN model)

```
fan_target.elf starts
  │
  ├─ Fan / lightbar loop (this process)
  │
  ├─ get_shellui_pid("SceShellUI")
  │     └─ inject overlay_elf  →  runs inside SceShellUI
  │           Mono already present → CreateLabel HUD
  │           UDP bind 127.0.0.1:29028
  │           update labels every 100ms
  │
  └─ game up → inject fps_elf into game
        Gnm flip hook → FPS every 250ms → UDP :29028
```

Injection matches etaHEN `Inject_Toolbox` / `inject_elf` (ptrace + elfldr_load).
Sources under `third_party/injector/` (from etaHEN libNineS / libelfldr).

In-tree fallback: push ELF to local elfldr on port 9021.

## FPS latency

FPS is a rolling window (250ms). Localhost UDP adds well under 1ms — not the bottleneck.
Labels refresh every 100ms in ShellUI.

## Build

```sh
make fps_elf PS5_PAYLOAD_SDK=/path/to/sdk   # embeds fps measurement ELF
# build overlay_elf similarly, embed, then:
make all
```

## config.ini

`/data/fan_target/config.ini` — `fps=1` enables inject path.

## License

GPL-3.0. Patterns/code derived from etaHEN (LightningMods) and upstream fan_target.
