#ifndef MOCK_CAN_DRIVER_H
#define MOCK_CAN_DRIVER_H

#include "../src/uds_types.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOCK_CAN_TX_FIFO_DEPTH 32
#define MOCK_CAN_RX_FIFO_DEPTH 32

typedef struct {
    /* TX ring buffer */
    can_frame_t tx_buf[MOCK_CAN_TX_FIFO_DEPTH];
    unsigned    tx_head;
    unsigned    tx_tail;
    unsigned    tx_count;

    /* RX ring buffer */
    can_frame_t rx_buf[MOCK_CAN_RX_FIFO_DEPTH];
    unsigned    rx_head;
    unsigned    rx_tail;
    unsigned    rx_count;

    /* Configurable error injection */
    bool        force_tx_error;
    bool        force_rx_error;
    bool        bus_off;

    /* Statistics */
    unsigned    total_tx;
    unsigned    total_rx;
} mock_can_t;

void         mock_can_init(mock_can_t *m);
void         mock_can_reset(mock_can_t *m);

/* Driver-side API (called by can_transport layer) */
uds_status_t mock_can_send(mock_can_t *m, const can_frame_t *frame);
uds_status_t mock_can_recv(mock_can_t *m, can_frame_t *frame, bool *available);
bool         mock_can_is_bus_off(mock_can_t *m);

/* Test-side API */
bool         mock_can_tx_pop(mock_can_t *m, can_frame_t *out);   /* dequeue what driver sent */
void         mock_can_rx_push(mock_can_t *m, const can_frame_t *f); /* inject RX frame */
unsigned     mock_can_tx_count(mock_can_t *m);
unsigned     mock_can_rx_count(mock_can_t *m);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_CAN_DRIVER_H */
