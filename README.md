# fan_target

Idle-aware fan curve + **ShellUI Mono HUD** (temps + FPS).

## Architecture 
```
fan_target.elf
  ├─ fan / lightbar (this process)
  ├─ inject overlay_elf → SceShellUI
  │     Mono labels: CPU / SoC / FPS
  │     colours by threshold
  │     UDP bind 127.0.0.1:29028
  └─ inject fps_elf → game
        Gnm flip hook → FPS → UDP only
```

Live data never touches the SSD. Only optional one-shot ELF embeds at **build** time.

### Temp colours
| Range | Colour |
|-------|--------|
| ≤50°C | Green |
| 51–60°C | Yellow |
| 61–70°C | Orange |
| ≥71°C | Red |

### FPS colours
| Range | Colour |
|-------|--------|
| ≥40 | Green |
| 30–39 | Yellow-green |
| 20–29 | Orange-yellow |
| <20 | Red |

## Build

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make fps_elf          # game FPS payload
make overlay_elf      # ShellUI HUD
make blob             # embed both into gen/
make all              # dist/fan_target.elf
```

Load **only** `dist/fan_target.elf`. It injects the rest.

## License

GPL-3.0
