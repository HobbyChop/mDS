/**
 * PIO-driven slot-2 bus servicer.
 *
 * Replaces the CPU-polled implementation in bus_interface.c. See
 * bus.pio for the state machine + protocol notes. Stats API mirrors
 * bus_interface.h's bus_stats_t so existing diagnostic prints in
 * main.c keep working untouched.
 */

#ifndef BUS_PIO_H
#define BUS_PIO_H

#include <stdint.h>
#include <stdbool.h>
#include "bus_interface.h"   // for bus_stats_t

// Initialise PIO0 SM0 with the slot2_bus program. Sets up the AD0..15
// pin functions, the /CS2 + /RD wait pins, the FIFO config, then
// starts the SM. Returns true on success.
bool bus_pio_init(void);

// Service one address-then-response transaction. Blocking: returns
// after we've handed a byte back to the SM, or immediately if no
// address has been pushed yet (use the no-wait variant if you don't
// want to block).
//
// Call this in a tight loop on the dedicated core (core 1).
void bus_pio_service_blocking(void);

// Same as above but returns false immediately if RX FIFO is empty.
// Useful if you want to interleave other core-1 work.
bool bus_pio_service_nonblocking(void);

#endif // BUS_PIO_H
