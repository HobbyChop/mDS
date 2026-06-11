/**
 * MIDI Interface Implementation.
 *
 * Handles BOTH real-time messages (0xF8..0xFF) and channel-voice
 * messages. Real-time bytes go straight into sync_state so the
 * DS can lock to the cart's MIDI clock with no software latency
 * between RX and visible state change. Channel-voice messages
 * (note on/off, CC, etc.) are queued for future use.
 *
 * MIDI real-time spec: status bytes 0xF8..0xFF can appear at any
 * point in the stream -- including mid-message -- and must be
 * handled WITHOUT disturbing the running-status parser. They also
 * have no data bytes.
 */

#include "midi.h"
#include "sync_state.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "pico/util/queue.h"

// MIDI real-time status bytes.
#define MIDI_RT_CLOCK     0xF8
#define MIDI_RT_START     0xFA
#define MIDI_RT_CONTINUE  0xFB
#define MIDI_RT_STOP      0xFC
#define MIDI_RT_ACTIVE    0xFE   // ignored
#define MIDI_RT_RESET     0xFF   // ignored
// System-common (have data bytes).
#define MIDI_SC_SPP       0xF2   // 3 bytes: F2 LSB MSB

// Channel-voice parser state.
static midi_message_t last_message = {0, 0, 0};
static uint8_t midi_buffer[3];
static uint8_t midi_buffer_idx = 0;
static bool has_message = false;

// SPP (song position pointer) accumulator: F2 needs two data bytes.
static int  spp_state = 0;   // 0 = idle, 1 = waiting LSB, 2 = waiting MSB
static uint8_t spp_lsb = 0;

// Raw-RX counter. Bumped on EVERY byte read from the UART, regardless
// of what the parser does with it. Lets us tell at a glance whether
// MIDI bytes are reaching us (front-end + cable working) vs. stuck
// upstream of the parser.
static volatile uint32_t s_rx_bytes = 0;
uint32_t midi_rx_byte_count(void) { return s_rx_bytes; }

void midi_init(void) {
    // Initialize UART for MIDI
    uart_init(MIDI_UART, MIDI_BAUD_RATE);
    gpio_set_function(MIDI_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(MIDI_RX_PIN, GPIO_FUNC_UART);

    // Enable an internal pull-up on RX. The 6N137 opto's output is
    // open-collector and needs an external pull-up to idle high when
    // the LED side has no current (= no MIDI signal). If that pull-up
    // is missing, weak, or the opto is disconnected entirely, GP21
    // floats and the UART parses random noise as MIDI bytes -- with
    // occasional 0xF8s registering as fake clock ticks. The internal
    // pull-up guarantees a clean idle-high regardless, and adds
    // harmlessly in parallel with any correct external pull-up.
    gpio_pull_up(MIDI_RX_PIN);

    // Set up UART parameters for MIDI
    // 8 data bits, 1 stop bit, no parity
    uart_set_format(MIDI_UART, 8, 1, UART_PARITY_NONE);

    // Enable UART interrupt for RX (optional)
    uart_set_fifo_enabled(MIDI_UART, true);
}

bool midi_poll_input(void) {
    bool got_channel_msg = false;
    while (uart_is_readable(MIDI_UART)) {
        uint8_t byte = uart_getc(MIDI_UART);
        s_rx_bytes++;

        // --- Real-time messages: 0xF8..0xFF. Per spec these
        // appear ANYWHERE in the stream (even mid-message) and
        // don't disturb the running-status parser. Handle first,
        // then continue. ---
        if (byte >= 0xF8) {
            uint32_t now = time_us_32();
            switch (byte) {
                case MIDI_RT_CLOCK:    sync_state_on_clock_tick(now); break;
                case MIDI_RT_START:    sync_state_on_start();         break;
                case MIDI_RT_CONTINUE: sync_state_on_continue();      break;
                case MIDI_RT_STOP:     sync_state_on_stop();          break;
                case MIDI_RT_ACTIVE:                                  break;
                case MIDI_RT_RESET:                                   break;
                default: break;
            }
            continue;
        }

        // --- SPP (Song Position Pointer) accumulator. ---
        // 0xF2 LSB MSB -- value is 14-bit count of 16th notes.
        if (byte == MIDI_SC_SPP) { spp_state = 1; continue; }
        if (spp_state == 1)      { spp_lsb = byte; spp_state = 2; continue; }
        if (spp_state == 2)      {
            uint16_t spp = (uint16_t)spp_lsb | ((uint16_t)byte << 7);
            sync_state_on_song_pos(spp);
            spp_state = 0;
            continue;
        }

        // --- Channel-voice parsing (notes / CC / etc). ---
        if (byte & 0x80) {
            // Other system-common we don't handle yet -- drop running
            // status to avoid corrupting it.
            if (byte >= 0xF0 && byte <= 0xF7) {
                midi_buffer_idx = 0;
                continue;
            }
            midi_buffer[0]  = byte;
            midi_buffer_idx = 1;
        } else {
            if (midi_buffer_idx == 0) continue;   // data byte with no status
            if (midi_buffer_idx < 3) midi_buffer[midi_buffer_idx++] = byte;
            // Two-byte messages (0xCn program change, 0xDn channel
            // pressure) complete at midi_buffer_idx == 2. Three-byte
            // messages (0x8/9/A/B/E) complete at midi_buffer_idx == 3.
            uint8_t status_byte = midi_buffer[0];
            uint8_t status_high = status_byte & 0xF0;
            int two_byte_msg = (status_high == 0xC0 || status_high == 0xD0);
            int complete = two_byte_msg ? (midi_buffer_idx >= 2)
                                        : (midi_buffer_idx >= 3);
            if (complete) {
                last_message.status = status_byte;
                last_message.data1  = midi_buffer[1];
                last_message.data2  = two_byte_msg ? 0 : midi_buffer[2];
                has_message     = true;
                got_channel_msg = true;
                // Surface the parsed event in the sync_state ring so
                // the DS side can consume it once the bus protocol is
                // wired to expose the extended window (see
                // sync_state.h `midi_ring` field). Until then, the
                // ring still fills correctly on the cart side -- this
                // keeps the firmware ready for DS-side hookup without
                // a future flash being required for protocol changes.
                sync_state_on_midi_event(last_message.status,
                                         last_message.data1,
                                         last_message.data2);
                // Running status: keep the status byte so subsequent
                // data-only bytes start a fresh message body.
                midi_buffer_idx = 1;
            }
        }
    }
    return got_channel_msg;
}

midi_message_t midi_get_message(void) {
    has_message = false;
    return last_message;
}

void midi_send_message(midi_message_t msg) {
    uart_putc(MIDI_UART, msg.status);
    uart_putc(MIDI_UART, msg.data1);
    uart_putc(MIDI_UART, msg.data2);
}

void midi_send_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    midi_message_t msg;
    msg.status = MIDI_NOTE_ON | (channel & 0x0F);
    msg.data1 = note & 0x7F;
    msg.data2 = velocity & 0x7F;
    midi_send_message(msg);
}

void midi_send_note_off(uint8_t channel, uint8_t note) {
    midi_message_t msg;
    msg.status = MIDI_NOTE_OFF | (channel & 0x0F);
    msg.data1 = note & 0x7F;
    msg.data2 = 0;
    midi_send_message(msg);
}

void midi_send_cc(uint8_t channel, uint8_t controller, uint8_t value) {
    midi_message_t msg;
    msg.status = MIDI_CC | (channel & 0x0F);
    msg.data1 = controller & 0x7F;
    msg.data2 = value & 0x7F;
    midi_send_message(msg);
}
