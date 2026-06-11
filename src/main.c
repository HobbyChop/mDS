/**
 * RP2040 Slot-2 MIDI Cartridge Firmware -- sDS synth cart ("STDS" v2).
 *
 * Speaks the mDS "STDS" protocol at version 2 (clock + MIDI event ring),
 * so ONE flashed firmware serves both stutter-ds clock sync and the sDS
 * MIDI synth -- see sync_state.h.
 *
 * Role: a memory-mapped MIDI I/O bridge the DS reads from. This is the
 * interface half of an mGB-style setup -- the DS ROM is the synthesizer
 * (pulse / sample / noise / FM voices on native DS audio); the cart
 * just gets MIDI in and out of the DS. The Pico's job is to:
 *   - Receive MIDI bytes on UART1 RX (GPIO21) at 31250 baud.
 *   - Parse real-time messages (0xF8 clock / 0xFA start / 0xFC stop /
 *     0xF2 SPP) into the clock/transport mirror, AND channel-voice
 *     messages (note on/off, CC, pitch-bend, ...) into the event ring.
 *   - Serve DS reads of the SRAM window at 0x0A000000 from that mirror.
 *   - Emit MIDI OUT bytes on UART1 TX when the DS does a trigger-read
 *     at 0xE0..0xE3 (see bus_pio.c midi_tx_dispatch).
 *
 * Core split:
 *   core 0 -- MIDI UART RX/parse + sync_state updates + USB debug
 *   core 1 -- tight PIO loop servicing the GBA bus + MIDI TX triggers
 *
 * The DS side is in `ds-rom/source/synth_cart.c`; the shared layout
 * lives in `sync_state.h` and the DS driver's `midi_sync_state_t`.
 * Keep them byte-for-byte aligned.
 *
 * Status / known limits:
 *   - DS->cart commands use read-trigger side effects (slot-2 SRAM
 *     /WR is unreliable across DS variants); only 0xE0..0xE3 MIDI OUT
 *     triggers are wired so far.
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "hardware/gpio.h"

#include "bus_interface.h"
#include "bus_pio.h"
#include "midi.h"
#include "sync_state.h"
#include "led.h"

// USB-CDC stdio is convenient for development -- prints status,
// MIDI byte counts, etc. Disable for final builds to save flash
// + boot time.
#define WITH_DEBUG_STDIO  1

void core1_main(void) {
    // Bus-servicing core. PIO state machine catches /CS2 + /RD edges
    // with sub-clock-cycle precision; this core just drains addresses
    // from the RX FIFO, looks up bytes, and pushes responses. No
    // allocation, no logging.
    while (1) {
        bus_pio_service_blocking();
    }
}

int main(void) {
#if WITH_DEBUG_STDIO
    // CMakeLists.txt sets PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS so
    // this call blocks until the USB CDC host opens the port (or 3 s
    // timeout). That makes the banner + tracer prints below reliably
    // visible during bringup instead of racing with Windows CDC
    // enumeration.
    stdio_init_all();
#endif

    // Tracer prints between init steps -- if any of these ever hangs
    // or crashes in the future, the last printed message tells us
    // which step died. Cheap insurance, runs only on debug builds.
#if WITH_DEBUG_STDIO
    printf("init: sync_state\n");
#endif
    sync_state_init();
#if WITH_DEBUG_STDIO
    printf("init: sync_state ok\n");
    printf("init: bus_pio\n");
#endif
    bus_pio_init();
#if WITH_DEBUG_STDIO
    printf("init: bus_pio ok\n");
    printf("init: midi\n");
#endif
    midi_init();
    led_init();   // onboard green LED (GPIO25): status + MIDI activity
#if WITH_DEBUG_STDIO
    printf("init: midi ok\n");
    printf("sDS synth cart fw v0.1 -- STDS protocol v%u\n",
           (unsigned)SYNC_PROTOCOL_VERSION);
#endif

    // Hand the bus interface to core 1 so MIDI parsing on core 0
    // never starves it. Each core has its own stack.
    multicore_launch_core1(core1_main);

    // Core 0: MIDI poll. The UART has its own FIFO and we drain it
    // each iteration; the real-time clock-tick path updates
    // sync_state without further locking.
    //
    // 1 Hz heartbeat: time_reached() / make_timeout_time_ms() avoid
    // a sleep, so MIDI byte latency stays sub-ms. The line carries
    // uptime (= core 0 isn't wedged) plus the live sync_state fields
    // (= MIDI parse state once the input is wired up). Bus servicer
    // on core 1 doesn't pay anything for this; the slot-2 timing
    // budget is unchanged.
#if WITH_DEBUG_STDIO
    absolute_time_t next_beat = make_timeout_time_ms(1000);
#endif
    while (1) {
        midi_poll_input();
        led_task();   // status + MIDI-activity on the onboard LED (self-throttled)
#if WITH_DEBUG_STDIO
        // Gate the heartbeat printf on USB-host-connected. When the
        // host (= PlatformIO monitor) ISN'T draining the CDC FIFO,
        // printf blocks for tens of ms waiting for the FIFO to free
        // up -- and during that block midi_poll_input() can't drain
        // the UART RX FIFO, so MIDI byte handling (including the
        // 0xFA START byte that triggers transport-rise) is delayed
        // unpredictably. That delay is a major contributor to
        // randomized DS-side start alignment with the DAW. With
        // this gate, headless runs (DS-only, no PC) skip the
        // heartbeat entirely and the MIDI poll loop stays tight;
        // debug runs (monitor open) still see heartbeat as before.
        if (time_reached(next_beat) && stdio_usb_connected()) {
            next_beat = make_timeout_time_ms(1000);
            sync_state_t *s = sync_state_get();
            bus_stats_t bs;
            bus_get_stats(&bs);
            printf("alive uptime=%u ms | midi rx=%u tick=%u bpm_q8=%u flags=0x%02x"
                   " | bus cs2=%u rd=%u wr=%u"
                   " addr_cs2=0x%04x addr_rd=0x%04x or=0x%04x last=0x%02x\n",
                   (unsigned)to_ms_since_boot(get_absolute_time()),
                   (unsigned)midi_rx_byte_count(),
                   (unsigned)s->tick_counter,
                   (unsigned)s->bpm_q8,
                   (unsigned)s->flags,
                   (unsigned)bs.cs2_active_edges,
                   (unsigned)bs.reads_served,
                   (unsigned)bs.writes_seen,
                   (unsigned)bs.last_address,
                   (unsigned)bs.last_address_at_rd,
                   (unsigned)bs.address_or,
                   (unsigned)bs.last_byte_served);
        } else if (time_reached(next_beat)) {
            // Re-arm the timer even when we skipped, so reconnecting
            // the monitor mid-session doesn't dump a stale 1-second
            // backlog and resumes 1 Hz from then on.
            next_beat = make_timeout_time_ms(1000);
        }
#endif
    }
    return 0;
}
