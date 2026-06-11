# mDS - DS slot-2 MIDI cartridge

**mDS** is a Nintendo DS / DS Lite **slot-2 (GBA Game Pak) MIDI interface**
built on an RP2040. It bridges hardware MIDI into the DS over the slot-2
SRAM bus so a DS app can be a MIDI synth. The DS is the synth; the cart is
the interface - see the [sDS synth app](../docs/08-synth-app.md), the
baseline ROM that runs on it.

> One firmware, two jobs: mDS speaks the mDS `"STDS"` protocol at **version
> 2** (MIDI clock + a MIDI event ring), so the same cart drives both the
> sDS synth *and* clock-sync apps (e.g. stutter-ds) with no changes.

## Specs

| | |
|---|---|
| **MCU** | RP2040 - dual Cortex-M0+ @ 133 MHz, 264 KB SRAM (Raspberry Pi Pico module, 2 MB flash) |
| **Host interface** | GBA slot-2 SRAM region `0x0A000000`, 4 KB window (12-bit address), serviced by a PIO state machine |
| **MIDI IN** | 31250 baud UART, opto-isolated (6N138 / H11L1) |
| **MIDI OUT** | 31250 baud UART, 2×220 Ω → TRS/DIN-5 |
| **Indicator** | onboard green LED (GPIO25): status + MIDI activity via PWM |
| **Power** | slot-2 3.3 V rail, no level shifting; 22 µF reservoir cap on VSYS |
| **Programming** | USB BOOTSEL (drag-and-drop UF2); SWD pads optional |
| **Compatibility** | DS / DS Lite only (DSi+ dropped slot-2) |

## Slot-2 bus (pin summary)

In the SRAM region the bus is **not** multiplexed: low address on A0..A11,
data returned on A16..A23 during `/RD`. Full detail + rationale in
[docs/02-pinout.md](../docs/02-pinout.md) (the firmware is the authority).

| Slot-2 signal | RP2040 GPIO | Dir | Role |
|---|---|---|---|
| A0..A7   | GPIO8..15            | in  | address low |
| A8..A11  | GPIO22, 26, 27, 28   | in  | address high (→ 4 KB window) |
| A16..A23 | GPIO0..7             | out | data bus (driven during `/RD`) |
| /CS2     | GPIO16               | in  | SRAM chip select |
| /RD      | GPIO17               | in  | read strobe |
| /WR      | GPIO18               | in  | write strobe (unused) |
| /IRQ     | GPIO19               | out | optional cart→DS line |
| MIDI TX  | GPIO20               | out | DIN/TRS OUT |
| MIDI RX  | GPIO21               | in  | opto IN |

## Firmware

- **core 1** runs a PIO slot-2 bus servicer: it answers DS reads of the
  SRAM window from a mirrored state struct, and fires MIDI-OUT bytes on
  trigger-reads at `0xE0..0xE3`.
- **core 0** parses MIDI IN (real-time clock/transport → a clock mirror;
  channel-voice notes/CC → a 16-entry event ring the DS polls) and drives
  the status LED.
- Protocol layout + the slot-2 read/write conventions:
  [docs/03-protocol.md](../docs/03-protocol.md).

### LED

| State | LED |
|---|---|
| powered, alive, no DS reading | dim slow breathe |
| a DS is linked (reading the bus) | steady glow |
| MIDI note/CC arrives | bright flash |
| MIDI clock running (transport play) | heartbeat pulse on the beat |

## Build & flash

Uses the Raspberry Pi Pico SDK. Native (Windows) toolchain recipe:

```
cd rp2040
cmake -S . -B build -G "MinGW Makefiles" -DPICO_BOARD=pico -DPICO_SDK_PATH=C:/pico-sdk
cmake --build build -j
```

(Or `-G "Unix Makefiles"` on Linux/macOS with `PICO_SDK_PATH` exported.)
Output: **`build/stutter_ds_cart.uf2`**. Hold **BOOTSEL**, plug USB, copy
the UF2 onto the `RPI-RP2` drive.

Bringup / validation checklist for new boards:
[docs/07-bringup-checklist.md](../docs/07-bringup-checklist.md).

## License

Licensed under **[CC BY-NC-SA 4.0](../LICENSE)** (Attribution-NonCommercial-
ShareAlike). You may build, modify, and share your own mDS for
**non-commercial** use, with attribution and under the same license;
**selling mDS units or derivatives is not permitted**. The "mDS" / "sDS"
names are reserved by the author (trademark is separate from this
copyright license). For commercial use, contact the author.
