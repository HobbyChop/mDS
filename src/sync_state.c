/**
 * Sync-state mirror.
 *
 * Owns the live struct the DS reads via the cartridge bus, plus the
 * MIDI handlers that keep it fresh. Single-writer (MIDI UART RX parser
 * on core 0) / single-reader (bus servicer on core 1): per-field words
 * are written atomically so the reader sees a coherent value for any
 * single field. Multi-field consistency isn't required -- the DS reads
 * one field per poll, and the event-ring publish protocol (write slot,
 * THEN bump head) makes torn ring reads impossible.
 *
 * BPM estimation: rolling average of the last 24 clock-tick spacings
 * (one beat's worth), updated once per beat to avoid flapping. This is
 * carried for tempo-synced DS apps (arps, LFOs, delays); the core
 * synth path only needs the event ring.
 */

#include "sync_state.h"

#include <string.h>

// Backing storage. Volatile because the bus servicer (other core)
// reads from it without synchronisation. We hand the bus servicer a
// pointer to this same buffer, so writes here are immediately visible
// to DS reads.
static volatile uint8_t state_buf[SLOT2_WINDOW_SIZE];
static volatile sync_state_t *state = (volatile sync_state_t *)state_buf;

// BPM rolling window: micros timestamp of each of the last N clock
// ticks. After N ticks land we compute BPM from the elapsed time
// (24 ticks = 1 beat at 24 PPQ).
#define BPM_WINDOW   24
static uint32_t bpm_window[BPM_WINDOW];
static uint32_t bpm_window_idx = 0;
static uint32_t bpm_window_filled = 0;

void sync_state_init(void) {
    memset((void *)state_buf, 0, SLOT2_WINDOW_SIZE);
    // "STDS" v2 == the mDS clock+MIDI-ring protocol. One physical cart
    // firmware serves both: clock consumers (stutter-ds, which probes
    // "STDS" and reads the clock fields) AND the sDS synth app (which
    // reads the MIDI event ring). Using "STDS" v2 means stutter-ds needs
    // no changes -- it already accepts this magic + version.
    state->magic[0] = 'S';
    state->magic[1] = 'T';
    state->magic[2] = 'D';
    state->magic[3] = 'S';
    state->version  = SYNC_PROTOCOL_VERSION;
    state->flags    = 0;
    state->bpm_q8   = 120u << 8;     // sensible default until first BPM lock
    bpm_window_idx    = 0;
    bpm_window_filled = 0;
}

sync_state_t *sync_state_get(void) {
    return (sync_state_t *)state;
}

void sync_state_on_clock_tick(uint32_t now_micros) {
    state->flags |= SYNC_FLAG_CLOCK_RUNNING;
    state->micros = now_micros;
    state->tick_counter++;

    // Record into the rolling BPM window.
    bpm_window[bpm_window_idx] = now_micros;
    bpm_window_idx = (bpm_window_idx + 1) % BPM_WINDOW;
    int was_filling = (bpm_window_filled < BPM_WINDOW);
    if (was_filling) bpm_window_filled++;

    // Once we've seen a full beat (24 ticks), update BPM. Span between
    // the oldest and newest sample = (BPM_WINDOW - 1) intervals = 23
    // tick-spacings = nearly one full beat.
    if (bpm_window_filled == BPM_WINDOW) {
        uint32_t oldest_idx = bpm_window_idx;   // points at the oldest
        uint32_t span_us = now_micros - bpm_window[oldest_idx];
        if (span_us > 0) {
            // 23 ticks of span -> seconds_per_beat = span_us * 24/23 / 1e6
            // bpm = 60 / seconds_per_beat = 57500000 / span_us
            // Q24.8 fixed-point so the IIR keeps sub-BPM precision.
            uint64_t input_q8 = (57500000ull << 8) / span_us;

            // Clamp to a sane musical range (20..300 BPM in Q24.8).
            if (input_q8 < (20u  << 8)) input_q8 = 20u  << 8;
            if (input_q8 > (300u << 8)) input_q8 = 300u << 8;

            if (was_filling) {
                // First full-window result: SNAP directly to the input
                // instead of running it through the IIR from the boot
                // default, so the very first displayed BPM is correct.
                state->bpm_q8 = (uint32_t)input_q8;
            } else {
                // Steady state: one-pole IIR new = (7/8)*old + (1/8)*input.
                // Reacts to real tempo changes within ~1 s while
                // averaging out per-tick MIDI jitter.
                uint32_t prev = state->bpm_q8;
                state->bpm_q8 = (uint32_t)((((uint64_t)prev * 7u) + input_q8) >> 3);
            }
        }
    }
}

// MIDI clock is the tempo source; transport flags reflect DAW state;
// the cart's tick_counter is MONOTONIC. DAWs in loop mode re-send START
// at each loop boundary -- resetting position there makes the song
// clock lurch backward, so (like most chiptune MIDI slaves) we use
// clock for tempo only and let the DS own playback position.
//
// START / CONTINUE set the play + clock-running flags but do NOT reset
// tick_counter. STOP clears the play flag. SPP updates song position
// without rewriting tick_counter.

void sync_state_on_start(void) {
    state->flags |= (SYNC_FLAG_CLOCK_RUNNING | SYNC_FLAG_TRANSPORT_PLAY);
}

void sync_state_on_continue(void) {
    state->flags |= (SYNC_FLAG_CLOCK_RUNNING | SYNC_FLAG_TRANSPORT_PLAY);
}

void sync_state_on_stop(void) {
    state->flags &= ~SYNC_FLAG_TRANSPORT_PLAY;
}

void sync_state_on_song_pos(uint16_t spp_16ths) {
    // Reflect the DAW's transport position for display; leave
    // tick_counter alone (the DS owns its own playback position).
    state->song_pos_16ths = spp_16ths;
}

void sync_state_on_midi_event(uint8_t status, uint8_t data1, uint8_t data2) {
    // Append the event to the ring. Single-producer (this runs only
    // from the core 0 MIDI poll), so no lock is needed. Publish order
    // matters: WRITE the slot fully, THEN advance the head, so a DS
    // read interleaved with this write sees either the old head (skip)
    // or the new head with a fully populated slot -- never half-filled.
    uint8_t idx = state->midi_write_head;
    if (idx >= MIDI_RING_LEN) idx = 0;   // defensive against torn read
    state->midi_ring[idx].status  = status;
    state->midi_ring[idx].data1   = data1;
    state->midi_ring[idx].data2   = data2;
    // Stash the low byte of tick_counter so the DS can timestamp the
    // event against the song clock and detect dropped events.
    state->midi_ring[idx].tick_lo = (uint8_t)(state->tick_counter & 0xFF);
    // Commit the slot THEN advance the head. volatile guarantees the
    // compiler emits the stores in order, and ARMv6-M does not reorder
    // plain memory writes, so the head never advances before the slot
    // contents are visible.
    idx = (uint8_t)((idx + 1u) % MIDI_RING_LEN);
    state->midi_write_head = idx;
}
