/**
 * led.c -- onboard LED status + MIDI activity (Raspberry Pi Pico, GPIO25).
 *
 * The stock Pico LED is a single GREEN LED, so this conveys state through
 * brightness/patterns (via PWM), not colour:
 *   - dim slow breathe : powered + firmware alive, no DS reading the cart
 *   - steady glow       : a DS is linked (actively reading the slot-2 bus)
 *   - bright flash       : a MIDI note/CC event arrived
 *   - heartbeat pulse    : MIDI clock is running (transport playing)
 *
 * Driven from the core-0 loop (led_task), self-throttled to ~8 ms. Reading
 * sync_state here is race-free: the MIDI parser that writes it also runs on
 * core 0. (An RGB LED would reuse this exact state logic, mapped to colour.)
 */
#include "led.h"
#include "sync_state.h"
#include "bus_interface.h"

#include "hardware/pwm.h"
#include "hardware/gpio.h"
#include "pico/time.h"

#define LED_PIN 25

static uint     s_slice, s_chan;
static uint32_t s_next_us;
static uint8_t  s_prev_write_head;
static uint32_t s_prev_reads;
static uint32_t s_last_read_ms;
static uint32_t s_prev_tick;
static int      s_flash;   // note/CC activity, 0..255 (decays)
static int      s_beat;    // clock beat pulse, 0..255 (decays)

void led_init(void) {
    gpio_set_function(LED_PIN, GPIO_FUNC_PWM);
    s_slice = pwm_gpio_to_slice_num(LED_PIN);
    s_chan  = pwm_gpio_to_channel(LED_PIN);
    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv(&c, 4.0f);
    pwm_config_set_wrap(&c, 255);          // 8-bit duty -> ~122 kHz, no flicker
    pwm_init(s_slice, &c, true);
    pwm_set_chan_level(s_slice, s_chan, 0);
    s_next_us = time_us_32();
}

static void set_level(int b) {
    if (b < 0)   b = 0;
    if (b > 255) b = 255;
    pwm_set_chan_level(s_slice, s_chan, (b * b) / 255);   // gamma ~2 for a smooth fade
}

void led_task(void) {
    uint32_t now = time_us_32();
    if ((int32_t)(now - s_next_us) < 0) return;           // ~8 ms cadence
    s_next_us = now + 8000;
    uint32_t ms = now / 1000;

    sync_state_t *s = sync_state_get();
    bus_stats_t bs;
    bus_get_stats(&bs);

    // Linked = the DS has read the slot-2 bus recently.
    if (bs.reads_served != s_prev_reads) { s_prev_reads = bs.reads_served; s_last_read_ms = ms; }
    int linked = (ms - s_last_read_ms) < 500;

    // MIDI note/CC activity advances the event ring head -> flash.
    if (s->midi_write_head != s_prev_write_head) { s_prev_write_head = s->midi_write_head; s_flash = 255; }

    // MIDI clock quarter-note -> heartbeat pulse (only while transport plays).
    if ((s->flags & SYNC_FLAG_TRANSPORT_PLAY) && s->tick_counter != s_prev_tick) {
        s_prev_tick = s->tick_counter;
        if ((s->tick_counter % 24) == 0) s_beat = 150;
    }

    int base;
    if (linked) {
        base = 60;                                        // steady glow
    } else {
        uint32_t ph = ms % 2400;                          // ~2.4 s breathe
        int tri = (ph < 1200) ? (int)ph : (int)(2400 - ph);
        base = 6 + tri * 30 / 1200;                       // dim 6..36
    }

    int b = base;
    if (s_flash > b) b = s_flash;
    if (s_beat  > b) b = s_beat;
    set_level(b);

    s_flash -= 28; if (s_flash < 0) s_flash = 0;          // ~70 ms note flash
    s_beat  -= 12; if (s_beat  < 0) s_beat  = 0;          // ~120 ms beat pulse
}
