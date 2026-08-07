# PS5 Fan Target

Idle-aware fan curve, lightbar, and on-screen FPS/temps overlay.

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
