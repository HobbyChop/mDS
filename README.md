<table>
  <tr>
    <td align="center"><img src="pics/mDS01.jpeg" width="300"><br></td>
    <td align="center"><img src="pics/mDS-PCB.png" width="300"><br></td>
    <td align="center"><img src="pics/mDS03.jpeg" width="300"><br></td>
  </tr>
</table>


# mDS - DS slot-2 MIDI cartridge

**mDS** is a Nintendo DS / DS Lite **slot-2 (GBA Game Pak) MIDI interface**
built on an RP2040. It bridges hardware MIDI into the DS over the slot-2
SRAM bus so a DS app can be a MIDI synth. The DS is the synth; the cart is
the interface - see the [sDS synth app](https://github.com/HobbyChop/sDS), the
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
data returned on A16..A23 during `/RD`.

| Slot-2 signal | RP2040 GPIO | Dir | Role |
|---|---|---|---|
| A0..A7   | GPIO8..15            | in  | address low |
| A8..A11  | GPIO22, 26, 27, 28   | in  | address high (4 KB window) |
| A16..A23 | GPIO0..7             | out | data bus (driven during `/RD`) |
| /CS2     | GPIO16               | in  | SRAM chip select |
| /RD      | GPIO17               | in  | read strobe |
| /WR      | GPIO18               | in  | write strobe (unused) |
| /IRQ     | GPIO19               | out | optional cart→DS line |
| MIDI TX  | GPIO20               | out | DIN/TRS OUT |
| MIDI RX  | GPIO21               | in  | opto IN |

## Firmware

- **core 1** runs a PIO slot-2 bus servicer (pinned to SRAM for constant-time
  serves): it answers DS reads of the SRAM window from a mirrored state struct,
  and emits MIDI OUT on trigger-reads - fixed transport at `0xE0..0xE3` plus a
  general byte sender at `0xC0..0xDF` (full notes / CC / pitch-bend / etc.).
- **core 0** parses MIDI IN (real-time clock/transport - a clock mirror;
  channel-voice notes/CC - a 16-entry event ring the DS polls) and drives
  the status LED.

### LED

| State | LED |
|---|---|
| powered, alive, no DS reading | dim slow breathe |
| a DS is linked (reading the bus) | steady glow |
| MIDI note/CC arrives | bright flash |
| MIDI clock running (transport play) | heartbeat pulse on the beat |

## Development specification

The contract for building against the mDS: writing a DS ROM that talks to the
cart, hacking the firmware, or building the hardware.

### Host link (slot-2): a read-only channel

The DS reads the cart's mirrored state from the slot-2 **SRAM region**
(`0x0A000000`). Slot-2 `/WR` is unreliable across DS units, so the link is
effectively **read-only**: DS→cart commands are *read side effects* (see MIDI
OUT below). DS-side access rules:

- Call `gbacartOpen()` (claims the slot + sets the ARM9 MPU so the region
  doesn't fault), then select the **slowest** SRAM timing, `GBA_WAIT_SRAM_18`
  (~540 ns `/RD`-low window). Faster timings shrink the cart's response window
  and corrupt reads, this timing is part of the contract, not a default.
- Reads are **8-bit**. A 32-bit `LDR` replicates one byte across the word, so
  multi-byte fields need four explicit little-endian `LDRB`s; single bytes are
  a plain `LDRB`.
- **Effective window = 256 bytes** (`0x00..0xFF`, address bits A0..A7). The PCB
  wires A8..A11 (4 KB-capable), but the firmware decodes only the low 8 bits:
  CPU-sampling the high nibble raced the bus, and the whole protocol fits under
  `0x100`. Reads at `0x100+` alias back into the low 256.

### Memory map - `"STDS"` v2 mirror

| Offset | Field | Type | Notes |
|---|---|---|---|
| `0x00` | `magic` | char[4] | `"STDS"` |
| `0x04` | `version` | u8 | `2` (clock + event ring) |
| `0x05` | `flags` | u8 | bit0 clock-running, bit1 transport-play |
| `0x06` | `_pad` | u8[2] | alignment |
| `0x08` | `tick_counter` | u32 | monotonic 24-PPQ MIDI-clock tick |
| `0x0C` | `bpm_q8` | u32 | BPM estimate, Q24.8 |
| `0x10` | `micros` | u32 | Pico µs timer (liveness / drift) |
| `0x14` | `song_pos_16ths` | u16 | MIDI Song Position (16th notes) |
| `0x16` | `midi_write_head` | u8 | event-ring write index (cart writes) |
| `0x17` | `midi_read_head` | u8 | DS-advisory read index |
| `0x18..0x57` | `midi_ring[16]` | 4B ea. | channel-voice event ring |
| `0x58..0xFD` | `_reserved` | | reads back 0 |
| `0xC0..0xE3` | TX triggers | | read side effects (see MIDI OUT); read back 0 |

Fields are single-writer (cart core 0) / single-reader (DS), word-atomic; no
multi-field consistency is promised, read one field per poll.

### MIDI IN - the event ring (cart → DS)

Channel-voice messages land in a 16-entry ring; each entry is 4 bytes:

| Byte | Meaning |
|---|---|
| 0 | `status` (high nibble = type 8/9/A/B/C/D/E, low nibble = channel) |
| 1 | `data1` (note / CC# / bend LSB) |
| 2 | `data2` (velocity / CC value / bend MSB; 0 for 2-byte messages) |
| 3 | `tick_lo` (low byte of `tick_counter` at receive) |

Single-producer (cart) / single-consumer (DS). **Publish:** the cart fills the
slot, *then* bumps `midi_write_head`, so a DS read interleaved with a write sees
either the old head (skip) or the new head with a full slot, never a torn entry.
**Consume:** keep a private cursor; each poll read `midi_write_head` and
dispatch up to it, bounded by the ring length so a corrupt head can't spin
forever. Real-time clock/transport bytes update the clock mirror (`flags`,
`tick_counter`, `bpm_q8`, `song_pos_16ths`), not the ring.

### MIDI OUT - trigger-reads (DS → cart → UART)

The DS "sends" by reading magic addresses; the cart emits on its UART TX as a
side effect. The read still returns the (zero) SRAM byte, so use a normal
volatile load and discard it.

| Read addr | Effect |
|---|---|
| `0xE0` / `0xE1` / `0xE2` / `0xE3` | emit `0xF8` clock / `0xFA` start / `0xFC` stop / `0xFB` continue (1 read, fixed) |
| `0xC0..0xCF` | latch high nibble = `addr & 0xF` (emits nothing) |
| `0xD0..0xDF` | emit byte `(hi << 4) + (addr & 0xF)` on UART TX |

**Arbitrary bytes** (full notes / CC / pitch-bend / SysEx) use the `0xC0..0xDF`
pair: send byte `B` as two adjacent reads, `0xC0 | (B>>4)` then
`0xD0 | (B&0xF)`. About 2 µs/byte at the slow timing (~150x the 31250-baud
wire), so the cart feeds MIDI OUT at full rate. DS helpers:
`synth_cart_midi_send_byte`, `synth_cart_midi_send`, and
`synth_cart_note_on/off/cc`.

### Timing & firmware architecture

- **Clock:** runs at the SDK-default **125 MHz** (not overclocked; the M0+ core
  draws off the unbuffered slot-2 rail).
- **Bus servicer** (`bus.pio`, PIO0 SM0): `wait` for `/CS2` then `/RD` (the
  address is guaranteed stable at `/RD`-fall), sample A0..A7 (autopush); core 1
  looks up `mirror[addr]` and pushes a 32-bit response (low 16 = data byte,
  high 16 = pindir mask `0x00FF`); the PIO drives the data during `/RD`-low and
  releases to high-Z at `/RD`-rise. The serve code is **SRAM-resident**, so a
  flash cache miss can't stretch a serve; the GPIO input synchroniser adds a
  fixed ~2-cycle (~16 ns) start-of-window tax.
- **Core split:** core 1 = the bus servicer (tight loop, no allocation); core 0
  = MIDI RX parse → mirror updates, status LED, USB-CDC debug. Concurrency is
  single-writer/single-reader on `volatile` storage, no locks.

### Bandwidth & limits

- The 31250-baud MIDI wire (~3 KB/s, ~1000 short msgs/s) is the ceiling, same as
  any MIDI device. The cart's read path (~1 byte/µs) has ~300x headroom, so the
  slow SRAM timing is never the MIDI bottleneck, and the 16-entry ring is huge
  relative to the DS's continuous polling.
- Read-only link, 256-byte window, byte-at-a-time: ideal for control/clock, not
  a bulk-data pipe (~1 MB/s hard ceiling).

### Writing a DS ROM against mDS

1. `gbacartOpen()`, then set `GBA_WAIT_SRAM_18` timing.
2. Verify `magic == "STDS"` at `0x00` and `version == 2` at `0x04`.
3. Each frame/loop: poll the ring, read `midi_write_head`, dispatch from your
   private cursor up to it.
4. To send MIDI out, use the trigger-read helpers; keep the two nibble reads
   adjacent and never read `0xC0..0xE3` unintentionally (side effects).
5. Reference driver: the sDS app's [`synth_cart.c`](https://github.com/HobbyChop/sDS).

See [`docs/02-pinout.md`](../docs/02-pinout.md),
[`docs/03-protocol.md`](../docs/03-protocol.md),
[`docs/04-ds-hardware-tips.md`](../docs/04-ds-hardware-tips.md), and
[`docs/07-bringup-checklist.md`](../docs/07-bringup-checklist.md) for depth.

## Build & flash

Uses the Raspberry Pi Pico SDK. Native (Windows) toolchain recipe:

```
cd rp2040
cmake -S . -B build -G "MinGW Makefiles" -DPICO_BOARD=pico -DPICO_SDK_PATH=C:/pico-sdk
cmake --build build -j
```

(Or `-G "Unix Makefiles"` on Linux/macOS with `PICO_SDK_PATH` exported.)
Output: **`build/mDS_v12.uf2`** (the artifact name is versioned via the target's
`OUTPUT_NAME` in `CMakeLists.txt`). Hold **BOOTSEL**, plug USB, copy the UF2
onto the `RPI-RP2` drive.

## License

Licensed under **[CC BY-NC-SA 4.0](../LICENSE)** (Attribution-NonCommercial-
ShareAlike). You may build, modify, and share your own mDS for
**non-commercial** use, with attribution and under the same license;
**selling mDS units or derivatives is not permitted**.

**Intent:** use the mDS however you like, including to make and **sell
music** (the license does not cover what you create with it). The
restriction is only on dealing in the device/design itself: no selling
mDS units or kits, no manufacturing for sale, no selling modified
firmware/hardware.

The "mDS" / "sDS" names are reserved by the author (trademark is separate
from this copyright license). For commercial use, contact the author.
