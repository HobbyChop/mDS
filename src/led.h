/**
 * led.h -- onboard green LED (GPIO25) status + MIDI activity indicator.
 */
#ifndef LED_H
#define LED_H

void led_init(void);   // configure GPIO25 for PWM
void led_task(void);   // call frequently from the core-0 loop (self-throttled)

#endif // LED_H
