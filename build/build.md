How to build your own mDS slot-2 MIDI cartridge: parts, assembly, and
flashing.

## Bill of materials

### Electronics

| Qty | Part | Notes |
|----:|------|-------|
| 1 | Raspberry Pi Pico (RP2040) | Not RP2040-Zero. |
| 1 | mDS PCB | Slot-2 edge fingers + footprints. See `hardware/`. |
| 1 | H11L1 optocoupler | MIDI IN isolation (high-speed, logic output). |
| 1 | 1N4148 diode | MIDI IN reverse protection (opto input LED). |
| 2 | 220 Ω resistor | MIDI line resistors (OUT drive + IN loop). |
| 1 | 1 kΩ resistor | H11L1 output pull-up. |
| 1 | 10 kΩ resistor | pull-up (6N137 enable / line bias). |
| 2 | 100 nF ceramic capacitor | decoupling (H11L1 Vcc + rail). |
| 2 | PJ-320E 3.5 mm jack | MIDI IN + MIDI OUT (TRS-MIDI). |

The status indicator is the Pico's onboard green LED (GPIO25): no extra part.

### Fasteners and mechanical

| Qty | Part |
|----:|------|
| 2 | M2.5 x 6 mm screws |
| 2 | M2.5 x 6 mm screws |
| 2 | M2.5 x 6 mm standoffs |
| 1 set | 3D-printed shell pieces (3) |

**Shell:** print from the STL files for the shell pieces.

## Tools

Soldering iron + solder, flux, side cutters, a small Phillips driver for
the M2.5 hardware, and a USB cable (data, not charge-only) for flashing.

## Assembly

1. **Front-end:** solder the H11L1 and its 1N4148, the 220 Ω / 1 kΩ / 10 kΩ
   resistors, and the 100 nF decoupling; fit the two
   PJ-320E jacks. Exact placement follows the PCB silkscreen / schematic.

**Important!** Clip the bottom of the pins as flush as possible to the board, so the shell pieces will fit.
   
3. **Pico:** mount the Pico to the PCB (solder no header). 
4. **Mechanical:** fit the board into the printed shell using the standoffs
   and screws as the shell design dictates.
   
**Important!** Do NOT plug in a usb cable while the cartridge is inserted into your DS. USB is for flashing firmware only!
   
5. **Flash + test**

## Flash the firmware

Build `mDS.uf2`
then hold **BOOTSEL**, plug USB, and copy the UF2 onto the `RPI-RP2` drive.
On a USB serial monitor you should see the banner
`sDS synth cart fw ... STDS protocol v2` and the green LED begin its slow
"alive" breathe.

## License

Build-your-own is welcome for non-commercial (resell) use, with attribution and
share-alike, under [CC BY-NC-SA 4.0](../LICENSE). Selling mDS units or
derivatives is not permitted; the "mDS" / "sDS" names are reserved.
