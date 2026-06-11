/**
 * GBA Slot-2 Bus Interface
 * 
 * Handles multiplexed address/data bus (AD0-AD15),
 * control signals (/CS2, /RD, /WR), and optional IRQ.
 * 
 * Pin mapping:
 * AD0-AD15  -> GPIO0-GPIO15
 * /CS2      -> GPIO16
 * /RD       -> GPIO17
 * /WR       -> GPIO18
 * IRQ       -> GPIO19
 */

#ifndef BUS_INTERFACE_H
#define BUS_INTERFACE_H

#include <stdint.h>
#include <stdbool.h>

// GPIO pin assignments
#define GPIO_AD_BASE    0   // AD0-AD15 use GPIO0-15
#define GPIO_CS2        16
#define GPIO_RD         17
#define GPIO_WR         18
#define GPIO_IRQ        19

// Bus operation results
typedef enum {
    BUS_OP_READ,
    BUS_OP_WRITE,
    BUS_OP_IDLE
} bus_operation_t;

// Initialize bus interface GPIO pins
void bus_interface_init(void);

// Poll bus for read/write operations
// Returns the type of operation detected
bus_operation_t bus_interface_poll(void);

// Get current address on the bus (AD0-AD15)
uint16_t bus_get_address(void);

// Get current data on the bus (AD0-AD15 in data phase)
uint16_t bus_get_data(void);

// Set data on the bus for a read operation
void bus_set_data(uint16_t data);

// Set IRQ signal (optional)
void bus_set_irq(bool active);

// Check if /CS2 is asserted (active low)
bool bus_cs2_active(void);

// Check if /RD is asserted (active low)
bool bus_rd_active(void);

// Check if /WR is asserted (active low)
bool bus_wr_active(void);

// Diagnostic counters bumped by bus_interface_poll. Read from core 0
// for periodic status prints. Volatile because core 1 writes them.
typedef struct {
    uint32_t cs2_active_edges;  // # of times we saw /CS2 transition low
    uint32_t reads_served;      // # of completed READ phases
    uint32_t writes_seen;       // # of completed WRITE phases (v1 ignores)
    uint32_t last_address;      // most recent address latched on /CS2 fall
    uint32_t last_address_at_rd;// address re-sampled at /RD fall (post setup)
    uint32_t address_or;        // OR of every address bit ever seen
    uint16_t ad_pins_or;        // OR of AD0..15 pin state on every sample
    uint8_t  last_byte_served;  // most recent low-byte response we drove
} bus_stats_t;

void bus_get_stats(bus_stats_t *out);

#endif // BUS_INTERFACE_H
