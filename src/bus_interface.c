/**
 * GBA Slot-2 Bus Interface Implementation -- fast polled.
 *
 * Multiplexed address/data bus handling for slot-2 SRAM region access.
 * For v1 we only serve READS -- the DS-side (midi_sync.c) never writes.
 * When the DS reads the SRAM window, we look up the address inside
 * sync_state's 256-byte mirror and drive the matching byte back on
 * the data lines.
 *
 * Performance: the slot-2 SRAM region cycles in ~120-200 ns per
 * access on a DS Lite. The original implementation looped over the
 * 16 AD pins with gpio_get / gpio_put / gpio_set_dir -- ~16 register
 * accesses each = ~500 ns just to capture the address, blowing the
 * window. This version uses the SIO bulk GPIO registers (sio_hw->gpio_in
 * / gpio_out / gpio_oe_set / gpio_oe_clr) so the whole address-read
 * + data-drive sequence runs in ~5 register writes total (~40 ns).
 * That puts us inside the budget with margin, assuming core 1 stays
 * dedicated to the poll loop (no IRQ handlers enabled there).
 *
 * Next step if this still glitches on real hardware: convert to a
 * PIO state machine + DMA chain. The .pio program would replicate
 * this same algorithm (wait CS2 low -> sample pins -> wait RD low ->
 * drive pins -> wait CS2 high -> release). See README.md TODO.
 */

#include "bus_interface.h"
#include "sync_state.h"
#include "hardware/gpio.h"
#include "hardware/structs/sio.h"

#include <stdint.h>

// AD0..AD15 mask, used to gate the bulk SIO register operations.
#define AD_MASK   0x0000FFFFu

static uint16_t last_address  = 0;
static uint16_t last_data     = 0;
static bool     cs2_was_active = false;
// Track whether we're currently driving AD0..15 so we only flip
// pindirs when the state actually changes (idempotent re-drives
// inside the same /CS2 cycle don't pay the OE-bank write cost).
static bool     ad_driving    = false;

// Diagnostic counters. Core 1 writes (via bus_interface_poll); core 0
// reads (via bus_get_stats). Volatile so the compiler doesn't hoist
// the writes out of the hot loop. 32-bit reads/writes are atomic on
// Cortex-M0+ so no cross-core lock needed for these scalars.
static volatile uint32_t s_cs2_edges        = 0;
static volatile uint32_t s_reads_served     = 0;
static volatile uint32_t s_writes_seen      = 0;
static volatile uint32_t s_last_address     = 0;
static volatile uint32_t s_last_address_rd  = 0;
static volatile uint32_t s_address_or       = 0;
static volatile uint16_t s_ad_pins_or       = 0;
static volatile uint8_t  s_last_byte_served = 0;

void bus_get_stats(bus_stats_t *out) {
    if (!out) return;
    out->cs2_active_edges   = s_cs2_edges;
    out->reads_served       = s_reads_served;
    out->writes_seen        = s_writes_seen;
    out->last_address       = s_last_address;
    out->last_address_at_rd = s_last_address_rd;
    out->address_or         = s_address_or;
    out->ad_pins_or         = s_ad_pins_or;
    out->last_byte_served   = s_last_byte_served;
}

void bus_interface_init(void) {
    // AD0..AD15: inputs at boot. We flip to output only when serving
    // a read; the SIO_OE bulk write makes the toggle cheap.
    for (int i = 0; i < 16; i++) {
        gpio_init(GPIO_AD_BASE + i);
        gpio_set_dir(GPIO_AD_BASE + i, GPIO_IN);
        gpio_pull_down(GPIO_AD_BASE + i);
    }
    sio_hw->gpio_oe_clr = AD_MASK;        // belt + suspenders

    // Control pins: inputs, pulled to their inactive level.
    gpio_init(GPIO_CS2); gpio_set_dir(GPIO_CS2, GPIO_IN); gpio_pull_up(GPIO_CS2);
    gpio_init(GPIO_RD ); gpio_set_dir(GPIO_RD , GPIO_IN); gpio_pull_up(GPIO_RD );
    gpio_init(GPIO_WR ); gpio_set_dir(GPIO_WR , GPIO_IN); gpio_pull_up(GPIO_WR );

    // IRQ line: output, idle low.
    gpio_init(GPIO_IRQ);
    gpio_set_dir(GPIO_IRQ, GPIO_OUT);
    gpio_put(GPIO_IRQ, 0);
}

// ---- single-shot helpers (kept for the header / smoke-test paths) ------
//
// The hot loop below doesn't call these -- it inlines the SIO reads
// directly so a single sio_hw->gpio_in load covers all four signals
// (CS2 + RD + WR + AD0..15). These exist only for any external caller
// that wants to peek at one signal in isolation.

bool bus_cs2_active(void) { return (sio_hw->gpio_in & (1u << GPIO_CS2)) == 0; }
bool bus_rd_active (void) { return (sio_hw->gpio_in & (1u << GPIO_RD )) == 0; }
bool bus_wr_active (void) { return (sio_hw->gpio_in & (1u << GPIO_WR )) == 0; }

uint16_t bus_get_address(void) { return (uint16_t)(sio_hw->gpio_in & AD_MASK); }
uint16_t bus_get_data   (void) { return (uint16_t)(sio_hw->gpio_in & AD_MASK); }

void bus_set_data(uint16_t data) {
    // Drive low 16 GPIOs to `data`. Output value first, then enable
    // outputs -- doing it the other way around briefly drives whatever
    // gpio_out held from a prior cycle, which would be a tiny glitch
    // on the bus (DS-side latches data on /RD's edge, so probably
    // invisible, but free to avoid).
    uint32_t v = sio_hw->gpio_out;
    v = (v & ~AD_MASK) | ((uint32_t)data & AD_MASK);
    sio_hw->gpio_out    = v;
    sio_hw->gpio_oe_set = AD_MASK;
    ad_driving = true;
}

static inline void bus_release_data(void) {
    if (ad_driving) {
        sio_hw->gpio_oe_clr = AD_MASK;
        ad_driving = false;
    }
}

void bus_set_irq(bool active) {
    if (active) sio_hw->gpio_set = (1u << GPIO_IRQ);
    else        sio_hw->gpio_clr = (1u << GPIO_IRQ);
}

// Look up one sync_state byte by offset. Inlined into the hot loop
// since `serve_byte` is the only thing the CPU does between sampling
// the address and driving the data byte.
static inline uint8_t serve_byte(uint16_t addr_offset) {
    sync_state_t *s = sync_state_get();
    const uint8_t *raw = (const uint8_t *)s;
    return raw[addr_offset & 0xFFu];
}

bus_operation_t bus_interface_poll(void) {
    // Single read of all GPIO inputs -- amortise the SIO load across
    // the four signals we care about. Each access on a DS slot-2 bus
    // is ~120-200 ns; this hot loop must complete a full
    // sample-decide-drive cycle inside that window.
    uint32_t pins = sio_hw->gpio_in;
    bool cs2 = (pins & (1u << GPIO_CS2)) == 0;

    if (!cs2) {
        // /CS2 inactive -- cycle just ended (or we were idle). Release
        // the bus so we don't fight whatever else lives in the slot
        // address space when the DS isn't selecting us.
        bus_release_data();
        cs2_was_active = false;
        return BUS_OP_IDLE;
    }

    // /CS2 active. If this is the falling edge, latch the address.
    // The address pins are driven by the DS the moment /CS2 falls
    // (and held through the full cycle), so capturing here gives us
    // a stable value to look up.
    if (!cs2_was_active) {
        last_address   = (uint16_t)(pins & AD_MASK);
        cs2_was_active = true;
        s_cs2_edges++;
        s_last_address = last_address;
        s_address_or  |= last_address;
        s_ad_pins_or  |= last_address;
    }

    if ((pins & (1u << GPIO_RD)) == 0) {
        // READ phase. Re-sample the address pins here too -- by the
        // time /RD has fallen the DS has had ample setup time to
        // stabilise its address drivers, so this value is more
        // trustworthy than the snapshot we took on /CS2 fall (which
        // might catch the bus before DS drivers engage).
        // If we ARE outputting (idempotent re-drive), the address we
        // sample here is our own driven value not the DS's, so guard
        // on ad_driving being false to keep it clean.
        if (!ad_driving) {
            uint16_t a_rd = (uint16_t)(pins & AD_MASK);
            s_last_address_rd = a_rd;
            s_address_or     |= a_rd;
            s_ad_pins_or     |= a_rd;
        }
        // READ phase. Build the 16-bit response: low byte from the
        // sync_state mirror, high byte from the next offset (so a
        // halfword read at an even address returns two consecutive
        // bytes -- matches how the DS reads u32 fields as 2 halfwords).
        uint16_t offset = last_address & 0xFFu;
        uint16_t data   = serve_byte(offset);
        if ((last_address & 1) == 0) {
            data |= ((uint16_t)serve_byte(offset + 1)) << 8;
        }
        // Drive AD0..15. Idempotent if we already drove this cycle;
        // ad_driving guards the OE write so back-to-back polls inside
        // the same /RD don't waste a register write per iteration.
        uint32_t v = sio_hw->gpio_out;
        v = (v & ~AD_MASK) | ((uint32_t)data & AD_MASK);
        sio_hw->gpio_out = v;
        if (!ad_driving) {
            sio_hw->gpio_oe_set = AD_MASK;
            ad_driving = true;
            // Only count on the OE-flip so back-to-back polls in the
            // same /RD pulse don't inflate the counter.
            s_reads_served++;
            s_last_byte_served = (uint8_t)(data & 0xFFu);
        }
        return BUS_OP_READ;
    }

    if ((pins & (1u << GPIO_WR)) == 0) {
        // WRITE phase. v1 ignores DS-side writes, but if we were
        // driving the bus a moment ago (back-to-back read then
        // write inside the same /CS2 -- unlikely but possible)
        // release first to avoid contention with whatever the DS
        // is about to drive.
        bus_release_data();
        last_data = (uint16_t)(pins & AD_MASK);
        (void)last_data;
        s_writes_seen++;
        return BUS_OP_WRITE;
    }

    // /CS2 active, neither strobe yet -- DS is in the setup window
    // between asserting /CS2 and asserting /RD or /WR. Stay parked.
    return BUS_OP_IDLE;
}
