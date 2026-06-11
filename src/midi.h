/**
 * MIDI Interface Driver
 * 
 * UART-based MIDI I/O at 31250 baud
 * TX on GPIO20, RX on GPIO21
 */

#ifndef MIDI_H
#define MIDI_H

#include <stdint.h>
#include <stdbool.h>

#define MIDI_BAUD_RATE 31250
#define MIDI_UART uart1
#define MIDI_TX_PIN 20
#define MIDI_RX_PIN 21

// MIDI message types
typedef enum {
    MIDI_NOTE_OFF = 0x80,
    MIDI_NOTE_ON = 0x90,
    MIDI_CC = 0xB0,
    MIDI_PROGRAM_CHANGE = 0xC0,
    MIDI_PITCH_BEND = 0xE0
} midi_status_t;

// MIDI message structure
typedef struct {
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
} midi_message_t;

// Initialize MIDI interface
void midi_init(void);

// Poll for incoming MIDI messages
// Returns true if a complete message was received
bool midi_poll_input(void);

// Diagnostic: total raw bytes read from UART RX since boot. Surfaced
// in the heartbeat so we can tell whether MIDI bytes are reaching the
// UART at all (vs. parser issue downstream).
uint32_t midi_rx_byte_count(void);

// Get the last received MIDI message
midi_message_t midi_get_message(void);

// Send a MIDI message
void midi_send_message(midi_message_t msg);

// Send a note on message
void midi_send_note_on(uint8_t channel, uint8_t note, uint8_t velocity);

// Send a note off message
void midi_send_note_off(uint8_t channel, uint8_t note);

// Send a control change message
void midi_send_cc(uint8_t channel, uint8_t controller, uint8_t value);

#endif // MIDI_H
