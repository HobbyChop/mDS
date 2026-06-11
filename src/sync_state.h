/**
 * Shared cart protocol state mirrored into Slot-2 SRAM.
 *
 * This struct is mirrored at the start of the cartridge's SRAM window
 * (DS physical address 0x0A000000). The Pico keeps it up to date from
 * its MIDI UART RX parser on core 0; core 1's bus servicer serves DS
 * reads byte-for-byte out of the same buffer.
 *
 * ROLE ("STDS" v2 cart): a general-purpose MIDI I/O bridge -- the same
 * protocol the mDS clock cart used (magic "STDS"), at version 2 (clock +
 * MIDI event ring). MIDI IN is decoded into
 *   - a 24-PPQ clock / transport / BPM mirror (for tempo-synced apps), and
 *   - a channel-voice EVENT RING (note on/off, CC, pitch-bend, ...).
 * The DS-side synth app polls the event ring and turns the events into
 * sound on the DS's own pulse / sample / noise / FM voices (mGB style:
 * the host is the synth, the cart is the interface). MIDI OUT is driven
 * by DS trigger-reads at 0xE0..0xE3 (see bus_pio.c).
 *
 * MUST stay byte-for-byte identical to midi_sync_state_t in the DS
 * driver (ds-rom/source/synth_cart.c). Layout is a plain unaligned
 * struct the DS reads as packed bytes/halfwords/words via a
 * `volatile *`, version-checked at boot, with room to grow at the end.
 *
 * Byte layout IS the mDS v2 layout (magic "STDS", version 2), so a
 * single physical cart firmware serves both worlds: stutter-ds (and any
 * clock consumer) probes "STDS" and reads the clock fields unchanged,
 * while the sDS synth app reads the MIDI event ring. The only nuance vs
 * the original mDS v2 struct is that offset 0x17, previously pad, now
 * carries the ring read head.
 */

#ifndef SYNC_STATE_H
#define SYNC_STATE_H

#include <stdint.h>
#include <assert.h>

#define SYNC_PROTOCOL_VERSION  2            // "STDS" v2 = clock + MIDI event ring
#define SLOT2_WINDOW_SIZE      4096         // 12-bit A0..A11 address window

// flags byte (offset 0x05). Only bits 0-1 are defined; the DS-side
// "garbage gate" rejects any read whose flags have bits 2..7 set, so
// any NEW flag bit must be added on the DS side in lockstep.
#define SYNC_FLAG_CLOCK_RUNNING   (1u << 0) // a MIDI clock (0xF8) was seen
#define SYNC_FLAG_TRANSPORT_PLAY  (1u << 1) // between START/CONTINUE and STOP

// MIDI channel-voice event ring. Single-producer (UART RX parser on
// core 0), single-consumer (DS polls). The cart writes the full 4-byte
// entry into ring[midi_write_head] THEN bumps midi_write_head modulo
// MIDI_RING_LEN, so a DS read interleaved with a write sees either the
// old head (skip this event) or the new head with a fully populated
// slot -- never a half-filled entry. The DS keeps its own read cursor
// and consumes everything between it and midi_write_head each poll.
//
// Entry layout (4 bytes):
//   byte 0  status   high nibble = message type (8/9/A/B/C/D/E),
//                    low nibble  = MIDI channel
//   byte 1  data1    note # / CC # / pitch-bend LSB
//   byte 2  data2    velocity / CC value / pitch-bend MSB (0 for 2-byte msgs)
//   byte 3  tick_lo  low byte of tick_counter at receive time, for
//                    ordering / late-event detection on the DS side
//
// 16 entries absorbs ~250 ms of dense traffic at the DS's ~1 kHz poll
// rate (~10 events worst-case per poll).
#define MIDI_RING_LEN   16
typedef struct {
    uint8_t  status;
    uint8_t  data1;
    uint8_t  data2;
    uint8_t  tick_lo;
} midi_event_t;

typedef struct {
    char         magic[4];          // 0x00  "STDS"
    uint8_t      version;           // 0x04  = SYNC_PROTOCOL_VERSION
    uint8_t      flags;             // 0x05  SYNC_FLAG_*
    uint8_t      _pad[2];           // 0x06..0x07  (align clock fields)
    uint32_t     tick_counter;      // 0x08  monotonic 24-PPQ MIDI clock tick
    uint32_t     bpm_q8;            // 0x0C  BPM estimate, Q24.8
    uint32_t     micros;            // 0x10  Pico microsecond timer (liveness/drift)
    uint16_t     song_pos_16ths;    // 0x14  MIDI SPP (16th-note position)
    uint8_t      midi_write_head;   // 0x16  event ring write index (cart writes)
    uint8_t      midi_read_head;    // 0x17  event ring read index (DS writes via trigger; advisory)
    midi_event_t midi_ring[MIDI_RING_LEN]; // 0x18..0x57  16 x 4B event ring
    uint8_t      _reserved[166];    // 0x58..0xFD  room to grow
} sync_state_t;

_Static_assert(sizeof(sync_state_t) <= SLOT2_WINDOW_SIZE,
               "sync_state_t must fit in the slot-2 window");

// Returns the live sync_state mirror. The bus servicer reads bytes
// from this buffer to answer DS reads of the SRAM window; only the
// MIDI-side code writes to it.
sync_state_t *sync_state_get(void);

// Initialise the mirror: magic + version + zeros. Call once at boot,
// before launching the bus servicer on core 1.
void sync_state_init(void);

// Helpers called from midi.c real-time message handlers.
void sync_state_on_clock_tick(uint32_t now_micros);   // 0xF8
void sync_state_on_start(void);                       // 0xFA
void sync_state_on_continue(void);                    // 0xFB
void sync_state_on_stop(void);                        // 0xFC
void sync_state_on_song_pos(uint16_t spp_16ths);      // 0xF2 + 2 data

// Channel-voice message (note on/off, CC, pitch bend, program change,
// channel pressure) parsed out of the UART stream and pushed into the
// event ring for the DS to consume. `data2` is zero for 2-byte
// messages (program change, channel pressure).
void sync_state_on_midi_event(uint8_t status, uint8_t data1, uint8_t data2);

#endif // SYNC_STATE_H
