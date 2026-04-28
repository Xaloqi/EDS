#include "mock_can_driver.h"
#include <string.h>
#include <stdio.h>

void mock_can_init(mock_can_t *m)
{
    if (!m) return;
    memset(m, 0, sizeof(*m));
}

void mock_can_reset(mock_can_t *m)
{
    if (!m) return;
    memset(m, 0, sizeof(*m));
}

uds_status_t mock_can_send(mock_can_t *m, const can_frame_t *frame)
{
    if (!m || !frame) return UDS_ERR_NULL_PTR;
    if (m->force_tx_error) return UDS_ERR;
    if (m->bus_off)        return UDS_ERR;
    if (m->tx_count >= MOCK_CAN_TX_FIFO_DEPTH) return UDS_ERR_BUFFER_OVERFLOW;

    m->tx_buf[m->tx_tail] = *frame;
    m->tx_tail = (m->tx_tail + 1u) % MOCK_CAN_TX_FIFO_DEPTH;
    m->tx_count++;
    m->total_tx++;
    return UDS_OK;
}

uds_status_t mock_can_recv(mock_can_t *m, can_frame_t *frame, bool *available)
{
    if (!m || !frame || !available) return UDS_ERR_NULL_PTR;
    if (m->force_rx_error) { *available = false; return UDS_ERR; }

    if (m->rx_count == 0u) {
        *available = false;
        return UDS_OK;
    }

    *frame     = m->rx_buf[m->rx_head];
    m->rx_head = (m->rx_head + 1u) % MOCK_CAN_RX_FIFO_DEPTH;
    m->rx_count--;
    m->total_rx++;
    *available = true;
    return UDS_OK;
}

bool mock_can_is_bus_off(mock_can_t *m)
{
    return m ? m->bus_off : false;
}

bool mock_can_tx_pop(mock_can_t *m, can_frame_t *out)
{
    if (!m || !out || m->tx_count == 0u) return false;
    *out      = m->tx_buf[m->tx_head];
    m->tx_head = (m->tx_head + 1u) % MOCK_CAN_TX_FIFO_DEPTH;
    m->tx_count--;
    return true;
}

void mock_can_rx_push(mock_can_t *m, const can_frame_t *f)
{
    if (!m || !f) return;
    if (m->rx_count >= MOCK_CAN_RX_FIFO_DEPTH) return;
    m->rx_buf[m->rx_tail] = *f;
    m->rx_tail = (m->rx_tail + 1u) % MOCK_CAN_RX_FIFO_DEPTH;
    m->rx_count++;
}

unsigned mock_can_tx_count(mock_can_t *m) { return m ? m->tx_count : 0u; }
unsigned mock_can_rx_count(mock_can_t *m) { return m ? m->rx_count : 0u; }
