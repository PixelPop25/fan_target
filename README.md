# PS5 Fan Target

`fan_target` keeps the PS5 fan controller at a selected target temperature while
leaving the console's automatic fan control enabled.

Five ready-to-build versions are included:

- 85 °C
- 80 °C
- 75 °C
- 70 °C
- 65 °C

Start with the 85 °C version if you are unsure which one to choose. Lower targets
usually make the fan react earlier and run faster.

## What it does

The payload runs continuously in the background. It checks the current target
every two seconds and restores the selected value if a game or system component
changes it.

It does not force a fixed fan speed. The original automatic fan controller still
decides how much cooling is needed.

Starting another version automatically stops the previous `fan_target.elf`
process, so only one copy remains active.

## Log output

The payload writes a short status line to the console and klog once a minute. It
also reports target changes immediately.

```text
[fan_target] started; target=80 C
[fan_target] target changed: 80 C -> 91 C
[fan_target] target restored: 91 C -> 80 C
[fan_target] status CPU=54 C, SoC=53 C, fan=18.6%, target=80 C
```

## Build

The PS5 Payload SDK is expected at `/opt/ps5-payload-sdk`.

```sh
cd fan_target
make clean all
```

The finished files are placed in `dist`:

```text
fan_target_85c.elf
fan_target_80c.elf
fan_target_75c.elf
fan_target_70c.elf
fan_target_65c.elf
SHA256SUMS.txt
```

To build only one temperature:

```sh
make clean TARGETS=80 all
```

## Start automatically

`fan_target` can start at boot with **PLK Autoloader**. Copy the ELF you want to
use into the autoloader's payload directory and add its file name to
`autoload.txt`:

```ini
!100
fan_target_80c.elf
```

Choose only one temperature version. If several versions are listed, each one
will replace the previous process and the last one started will remain active.
The optional `!100` line adds a short delay before launch and can be adjusted
to match the rest of your autoload sequence.
