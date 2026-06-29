# mDS cart -- DS-side MIDI examples

Minimal, copy-pasteable C for talking to the **mDS** slot-2 MIDI cart from a
Nintendo DS homebrew (devkitARM + libnds/calico). It covers **sending** and
**receiving** MIDI notes, CC, and clock/transport.

The cart exposes a small memory-mapped struct at the slot-2 base. Two quirks to
know up front:

1. **Read in single bytes.** The slot-2 SRAM region replicates a byte across a
   wider load, so a 32-bit read must be done as four `LDRB`s, not one `LDR`.
2. **Output is conveyed by *reads*, not writes** (the cart can't be written
   reliably). Each outgoing byte is two reads: a read of `0xC0|hi` latches the
   high nibble, then a read of `0xD0|lo` makes the cart emit `(hi<<4)|lo` on its
   UART TX.

---

## 1. Register map & low-level access

```c
#include <nds.h>
#include <calico/nds/gbacart.h>   // gbacartOpen / gbacartSetTiming

#define MDS_BASE        0x0A000000u   // slot-2 base

// offsets into the cart's mirrored sync struct
#define MDS_MAGIC       0x00          // "STDS" (4 bytes) when present
#define MDS_FLAGS       0x05          // bit1 = transport playing
#define MDS_TICK        0x08          // u32, monotonic 24-PPQ clock tick
#define MDS_BPM_Q8      0x0C          // u32, BPM in Q8.8 fixed point
#define MDS_WRITE_HEAD  0x16          // cart's ring write cursor (u8)
#define MDS_READ_HEAD   0x17          // our consume cursor (u8)
#define MDS_RING        0x18          // 16 entries x 4 bytes: status, d1, d2, tick_lo
#define MDS_RING_LEN    16
#define MDS_RING_ENTRY  4

#define MDS_TX_HI       0xC0          // read 0xC0|hi  -> latch high nibble
#define MDS_TX_LO       0xD0          // read 0xD0|lo  -> emit (hi<<4)|lo on UART

#define MDS_FLAG_PLAY   (1u << 1)

// A single-byte read is correct; only wide loads hit the replication quirk.
static inline u8 mds_rd8(u32 off) {
    return *(volatile u8 *)(MDS_BASE + off);
}

// 32-bit read as four explicit LDRBs (so the compiler can't fold them to one LDR).
static u32 mds_rd32(u32 off) {
    u32 base = MDS_BASE + off, b0, b1, b2, b3;
    __asm__ __volatile__(
        "ldrb %0,[%4,#0]\n ldrb %1,[%4,#1]\n ldrb %2,[%4,#2]\n ldrb %3,[%4,#3]\n"
        : "=&l"(b0), "=&l"(b1), "=&l"(b2), "=&l"(b3) : "l"(base) : "memory");
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}
```

---

## 2. Detect / open the cart

```c
static int   s_active   = 0;
static u8    s_read_head = 0;

// Open slot-2, set timing, and verify the cart magic. Safe to call again to
// re-probe. Returns true if an mDS cart is present.
bool mds_open(void) {
    if (!gbacartOpen()) { s_active = 0; return false; }   // claim slot-2 + MPU
    gbacartSetTiming(GBA_WAIT_SRAM_MASK, GBA_WAIT_SRAM_18);

    // The open/timing hand-off is async, so retry the magic read a few times.
    for (int i = 0; i < 16; ++i) {
        if (mds_rd8(MDS_MAGIC+0)=='S' && mds_rd8(MDS_MAGIC+1)=='T' &&
            mds_rd8(MDS_MAGIC+2)=='D' && mds_rd8(MDS_MAGIC+3)=='S') {
            s_active    = 1;
            s_read_head = mds_rd8(MDS_WRITE_HEAD) % MDS_RING_LEN;   // start synced
            return true;
        }
        swiDelay(2000);
    }
    gbacartClose();
    s_active = 0;
    return false;
}

bool mds_present(void) { return s_active != 0; }
```

> Tip: don't touch slot-2 before `mds_open()` -- a raw read on some flashcarts
> before the cart is claimed can wedge them. Probe a couple of frames into boot.

---

## 3. Send MIDI OUT (notes, CC, raw bytes)

```c
// One raw byte out (use this for realtime bytes: clock 0xF8, start 0xFA, stop 0xFC).
void mds_send_byte(u8 b) {
    if (!s_active) return;
    (void)mds_rd8((u32)MDS_TX_HI | (u32)(b >> 4));     // latch high nibble
    (void)mds_rd8((u32)MDS_TX_LO | (u32)(b & 0x0F));   // low nibble -> cart emits b
}

// One channel-voice message (status 0x80..0xEF).
void mds_send(u8 status, u8 d1, u8 d2) {
    if (!s_active || status < 0x80 || status >= 0xF0) return;
    mds_send_byte(status);
    mds_send_byte(d1 & 0x7F);
    mds_send_byte(d2 & 0x7F);
}

static inline void mds_note_on (u8 ch, u8 note, u8 vel) { mds_send(0x90 | (ch & 0xF), note, vel); }
static inline void mds_note_off(u8 ch, u8 note, u8 vel) { mds_send(0x80 | (ch & 0xF), note, vel); }
static inline void mds_cc      (u8 ch, u8 num,  u8 val) { mds_send(0xB0 | (ch & 0xF), num,  val); }

// Example: play middle C on channel 1 for one beat, then release.
void example_play_note(void) {
    mds_note_on(0, 60, 100);     // ch 1 (0-based), note 60, velocity 100
    // ... hold for a beat ...
    mds_note_off(0, 60, 0);
}
```

---

## 4. Receive MIDI IN (drain the event ring)

External gear -> cart UART RX -> 16-entry ring -> your DS. Drain it every frame.

```c
// Pop one received channel-voice event. Returns true and fills out-params when
// one was waiting; false when the ring is drained. Skips torn/empty slots.
bool mds_recv(u8 *status, u8 *d1, u8 *d2) {
    if (!s_active) return false;
    u8 wh = mds_rd8(MDS_WRITE_HEAD) % MDS_RING_LEN;
    int guard = 0;
    while (s_read_head != wh && guard++ < MDS_RING_LEN) {
        u32 e = MDS_RING + (u32)s_read_head * MDS_RING_ENTRY;
        u8 s = mds_rd8(e+0), a = mds_rd8(e+1), b = mds_rd8(e+2);   // [3] = tick_lo
        s_read_head = (s_read_head + 1) % MDS_RING_LEN;
        if (s & 0x80) {                  // valid status byte = a real event
            if (status) *status = s;
            if (d1) *d1 = a;
            if (d2) *d2 = b;
            return true;
        }
    }
    return false;
}

// Example: consume everything received this frame.
void example_read_input(void) {
    u8 st, d1, d2;
    while (mds_recv(&st, &d1, &d2)) {
        u8 type = st & 0xF0, ch = st & 0x0F;
        if      (type == 0x90 && d2 > 0) { /* note on : note=d1 vel=d2 ch=ch */ }
        else if (type == 0x80 || (type == 0x90 && d2 == 0)) { /* note off */ }
        else if (type == 0xB0) { /* control change: cc=d1 val=d2 */ }
    }
}
```

---

## 5. Clock & transport

### Reading an incoming clock (slave)

```c
int  mds_bpm(void) {                          // 0 if no clock
    if (!s_active) return 0;
    u32 q8 = mds_rd32(MDS_BPM_Q8);
    if (q8 < (20u<<8) || q8 > (300u<<8)) return 0;   // sanity gate
    return (int)((q8 + 128u) >> 8);                  // Q8 -> nearest BPM
}

u32  mds_tick(void) { return s_active ? mds_rd32(MDS_TICK) : 0; }   // 24 PPQ
bool mds_playing(void) { return s_active && (mds_rd8(MDS_FLAGS) & MDS_FLAG_PLAY); }

// Example: derive bar/beat and step a 16-step sequencer off the incoming clock.
// The tick is monotonic 24-PPQ; the host zeroes it on transport Start.
void example_follow_clock(void) {
    if (!mds_playing()) return;
    u32 t    = mds_tick();
    int beat = (t / 24) % 4;           // 24 ticks per quarter note
    int step = (t / 6) % 16;           // 6 ticks = 1/16 note -> 16 steps/bar
    (void)beat; (void)step;            // fire your step here when it changes
}
```

### Being the master (send clock out)

MIDI clock is 24 pulses per quarter note. **For tight timing, drive this from a
hardware-timer IRQ** (≈1–2 kHz) rather than the 60 Hz VBlank loop -- emitting one
F8 per frame bunches pulses on frame boundaries (up to ~16 ms jitter).

```c
// Call at your tick cadence (ideally a timer ISR). Sends 24 F8 per quarter.
void mds_clock_pulse(void) { mds_send_byte(0xF8); }   // realtime clock
void mds_transport_start(void) { mds_send_byte(0xFA); }   // slaves reset to bar 1 + run
void mds_transport_stop (void) { mds_send_byte(0xFC); }   // slaves halt

// Phase-accumulator example for a VBlank-loop master (carries the remainder so
// there's no long-term drift; jitter is frame-granular -- use a timer for tight).
static int s_acc = 0;
void example_emit_clock_per_frame(int bpm) {   // call once per 60 Hz frame
    s_acc += bpm;                               // 1 quarter = 3600 units @ 60fps
    while (s_acc >= 150) { mds_clock_pulse(); s_acc -= 150; }   // 3600/24 = 150
}
```

---

## 6. Putting it together (per-frame skeleton)

```c
int main(void) {
    // ... video / audio init ...
    if (!mds_open()) { /* show "no mDS cart" */ }

    while (1) {
        swiWaitForVBlank();
        scanKeys();

        // INPUT: consume incoming notes/CC/clock
        example_read_input();
        if (mds_playing()) example_follow_clock();

        // OUTPUT: e.g. a pad press sends a note
        if (keysDown() & KEY_A) mds_note_on(0, 60, 100);
        if (keysUp()   & KEY_A) mds_note_off(0, 60, 0);
    }
}
```

---

### Notes
- Channels are 0-based on the wire (`ch & 0x0F`); MIDI "channel 1" = `0`.
- `mds_recv()` consumes the ring -- if you only want clock/BPM for a readout and
  aren't reading notes, keep the read cursor synced instead (`s_read_head =
  mds_rd8(MDS_WRITE_HEAD) % MDS_RING_LEN;` each frame) so the ring never appears
  full to the cart.
- Velocity-0 note-on is the conventional note-off; handle both.
- All reads are cheap (a couple of `LDRB`s); a full message out is 6 reads.
