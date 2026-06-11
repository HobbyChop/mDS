How to build your own mDS slot-2 MIDI cartridge: parts, assembly, and
flashing.

## Bill of materials

### Electronics

| Qty | Part | Notes |
|----:|------|-------|
| 1 | Raspberry Pi Pico (RP2040) | Standard Pico module (GPIO16 = `/CS2`, GPIO25 green LED). Not Pico W / RP2040-Zero. |
| 1 | mDS PCB | Slot-2 edge fingers + footprints. See `hardware/`. |
| 1 | 6N137 optocoupler | MIDI IN isolation (high-speed, logic output). |
| 1 | 1N4148 diode | MIDI IN reverse protection (opto input LED). |
| 2 | 220 Ω resistor | MIDI line resistors (OUT drive + IN loop). |
| 1 | 1 kΩ resistor | 6N137 output pull-up. |
| 1 | 10 kΩ resistor | pull-up (6N137 enable / line bias). |
| 2 | 100 nF ceramic capacitor | decoupling (6N137 Vcc + rail). |
| 1 | 22 µF capacitor (low-ESR) | VSYS reservoir; fixes brown-out under bus traffic. |
| 2 | PJ-320E 3.5 mm jack | MIDI IN + MIDI OUT (TRS-MIDI). |

The status indicator is the Pico's onboard green LED (GPIO25): no extra part.

Exact resistor/cap placement follows the mDS PCB silkscreen / schematic
(a 6N137 MIDI front-end); GPIO mapping is in [02-pinout.md](02-pinout.md).

### Fasteners and mechanical

| Qty | Part |
|----:|------|
| 2 | M2.5 x 8 mm screws |
| 2 | M2.5 x 5 mm screws |
| 2 | M2.5 x 6 mm standoffs |
| 1 set | 3D-printed shell pieces |

**Shell:** print from the STL files for the shell pieces in `hardware/`
(see that folder). Print orientation, infill, and exact fit are defined by
those models.

> Screw roles depend on the shell STL design and are easy to mix up:
> typically the 6 mm standoffs space the board off the shell boss, the
> 8 mm screws pass through the shell into the standoffs, and the 4 mm
> screws retain the Pico. Match lengths to your print rather than forcing
> a screw that bottoms out or protrudes.

## Tools

Soldering iron + solder, flux, side cutters, a small Phillips driver for
the M2.5 hardware, and a USB cable (data, not charge-only) for flashing.

## Assembly

1. **Front-end:** solder the 6N137 and its 1N4148, the 220 Ω / 1 kΩ / 10 kΩ
   resistors, and the 100 nF decoupling + 22 µF VSYS caps; fit the two
   PJ-320E jacks. Exact placement follows the PCB silkscreen / schematic;
   GPIO mapping is in [02-pinout.md](02-pinout.md).
2. **Pico:** mount the Pico to the PCB (solder or header). Keep GPIO23/24/29
   (Pico module reserves) clear of bus use; only GPIO25 (LED) is used off
   the bus.
3. **Mechanical:** fit the board into the printed shell using the standoffs
   and screws as the shell design dictates (see the note above), then close
   the shell halves.
4. **Flash + test** before final close-up if you can, so the board is still
   accessible (next sections).

## Flash the firmware

Build `stutter_ds_cart.uf2` per [../rp2040/README.md](../rp2040/README.md),
then hold **BOOTSEL**, plug USB, and copy the UF2 onto the `RPI-RP2` drive.
On a USB serial monitor you should see the banner
`sDS synth cart fw ... STDS protocol v2` and the green LED begin its slow
"alive" breathe.

## Validate

Run the bringup checklist before trusting a new board:
[07-bringup-checklist.md](07-bringup-checklist.md). In short: confirm the
VSYS rail holds under load, insert into a DS / DS Lite, and use the
diagnostic ROM to verify `STDS` magic + version 2 and a clean STRESS pass
across the slot-2 window.

## License

Build-your-own is welcome for non-commercial use, with attribution and
share-alike, under [CC BY-NC-SA 4.0](../LICENSE). Selling mDS units or
derivatives is not permitted; the "mDS" / "sDS" names are reserved.
