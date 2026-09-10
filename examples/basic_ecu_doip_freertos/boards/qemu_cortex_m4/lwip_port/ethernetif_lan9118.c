// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: .../boards/qemu_cortex_m4/lwip_port/ethernetif_lan9118.c
 *
 * PURPOSE: lwIP netif driver for the SMSC LAN9118 Ethernet MAC as emulated by
 *          QEMU's mps2-an386 machine (hw/net/lan9118.c, base 0x40200000).
 *
 *          This is the piece compat-tests#7 was actually missing. Before it,
 *          the example compiled against boards/qemu_cortex_m4/lwip_stub,
 *          whose every socket call returns -1 unconditionally — so the DoIP
 *          CI leg could never exchange a byte no matter what the harness did.
 *
 *          REGISTER SEMANTICS were taken from QEMU's own device model rather
 *          than the SMSC datasheet, because QEMU is the only target this
 *          driver runs on and the model implements a documented subset:
 *            - RX data FIFO   : offsets 0x00..0x1f (read)
 *            - TX data FIFO   : offsets 0x20..0x3f (write)
 *            - RX status FIFO : 0x40 (pop), 0x44 (peek)
 *            - BYTE_TEST      : reads 0x87654321 — endianness/presence probe
 *            - ID_REV         : reads 0x01180001
 *            - RX_FIFO_INF    : (status_fifo_used << 16) | (data_used << 2)
 *
 *          TRANSMIT is a single-segment write: TX command 'A' (buffer size in
 *          bits 0..10, first-segment bit 13, last-segment bit 12), then
 *          command 'B', then the frame as little-endian 32-bit words. QEMU's
 *          tx_fifo_push() assembles bytes least-significant-first — its own
 *          comment notes the datasheet is unclear and that empirical results
 *          are little-endian.
 *
 *          RECEIVE pops one RX status word per frame; bits 16..29 carry the
 *          frame length *including* the 4-byte CRC that the model appends, so
 *          the payload handed to lwIP is length - 4.
 *
 *          POLLED, NOT INTERRUPT-DRIVEN. The RX path runs from an ordinary
 *          FreeRTOS task (see eds_net.c), which keeps three things out of the
 *          design: an ISR that must not call blocking lwIP APIs, a
 *          FromISR/critical-section interaction with the tcpip mailbox, and
 *          any NVIC priority coupling to configMAX_SYSCALL_INTERRUPT_PRIORITY.
 *          At DoIP's traffic level (one diagnostic client, request/response)
 *          polling costs nothing measurable and removes a whole class of
 *          concurrency bug.
 *
 * SAFETY  : Example driver — not safety-assessed.
 * STANDARD: MISRA C:2012 alignment intended. No malloc (lwIP pbuf pools and
 *           file-scope static staging buffers only), no recursion, and every
 *           hardware wait loop is bounded.
 * =============================================================================
 */

#include "ethernetif_lan9118.h"
#include "board_uart.h"

#include "lwip/opt.h"
#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"
#include "lwip/snmp.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * Register map (QEMU mps2-an386: hw/arm/mps2.c ethernet_base = 0x40200000)
 * ------------------------------------------------------------------------ */

#define LAN9118_BASE            (0x40200000UL)

#define REG_RX_DATA_FIFO        (0x00UL)
#define REG_TX_DATA_FIFO        (0x20UL)
#define REG_RX_STATUS_FIFO      (0x40UL)
#define REG_ID_REV              (0x50UL)
#define REG_IRQ_CFG             (0x54UL)
#define REG_INT_STS             (0x58UL)
#define REG_INT_EN              (0x5CUL)
#define REG_BYTE_TEST           (0x64UL)
#define REG_RX_CFG              (0x6CUL)
#define REG_TX_CFG              (0x70UL)
#define REG_HW_CFG              (0x74UL)
#define REG_RX_DP_CTRL          (0x78UL)
#define REG_RX_FIFO_INF         (0x7CUL)
#define REG_TX_FIFO_INF         (0x80UL)
#define REG_MAC_CSR_CMD         (0xA4UL)
#define REG_MAC_CSR_DATA        (0xA8UL)

#define LAN9118_REG(off)  (*(volatile uint32_t *)(LAN9118_BASE + (off)))

/* Expected probe values (QEMU lan9118_readl). */
#define BYTE_TEST_EXPECTED      (0x87654321UL)
#define ID_REV_LAN9118          (0x01180001UL)
#define ID_REV_CHIP_MASK        (0xFFFF0000UL)
#define ID_REV_LAN9118_CHIP     (0x01180000UL)

/* HW_CFG */
#define HW_CFG_SRST             (0x00000001UL)

/* TX_CFG */
#define TX_CFG_TX_ON            (0x00000002UL)
#define TX_CFG_TXS_DUMP         (0x00008000UL)
#define TX_CFG_TXD_DUMP         (0x00004000UL)

/* RX_DP_CTRL */
#define RX_DP_CTRL_FFWD         (0x80000000UL)

/* MAC CSR command register */
#define MAC_CSR_CMD_BUSY        (0x80000000UL)
#define MAC_CSR_CMD_READ        (0x40000000UL)
#define MAC_CSR_CMD_ADDR_MASK   (0x0000000FUL)

/* Indirect MAC register indices */
#define MAC_REG_CR              (1UL)
#define MAC_REG_ADDRH           (2UL)
#define MAC_REG_ADDRL           (3UL)

/* MAC_CR bits */
#define MAC_CR_TXEN             (0x00000008UL)
#define MAC_CR_RXEN             (0x00000004UL)
/* NOTE: MAC_CR_BCAST is a *disable*. QEMU's lan9118_filter() accepts a
 * broadcast frame only when this bit is CLEAR. Setting it would silently
 * drop every ARP request and make the ECU unreachable. */
#define MAC_CR_BCAST_DISABLE    (0x00000800UL)

/* TX command 'A' */
#define TX_CMD_A_LAST_SEG       (0x00001000UL)
#define TX_CMD_A_FIRST_SEG      (0x00002000UL)
#define TX_CMD_A_BUFSIZE_MASK   (0x000007FFUL)

/* RX status word */
#define RX_STATUS_LEN_SHIFT     (16U)
#define RX_STATUS_LEN_MASK      (0x3FFFUL)
#define RX_STATUS_ERROR         (0x00008000UL)

/* RX_FIFO_INF */
#define RX_FIFO_INF_SFUSED_SHIFT (16U)
#define RX_FIFO_INF_SFUSED_MASK  (0x00FFUL)

/* Ethernet framing */
#define ETH_CRC_LEN             (4U)
#define ETH_FRAME_MAX           (1536U)
#define ETH_FRAME_MIN           (14U)

#define BYTES_PER_WORD          (4U)

/** Bound for every hardware poll loop. QEMU completes these synchronously;
 *  the bound exists so real silicon cannot hang the driver. */
#define LAN9118_WAIT_ITERATIONS (100000UL)

/* ---------------------------------------------------------------------------
 * Module state
 *
 * Static staging buffers rather than dynamic allocation. TX and RX get
 * separate buffers on purpose: linkoutput runs on lwIP's tcpip thread while
 * lan9118_poll() runs on the RX task, so a shared buffer would be a genuine
 * data race rather than a theoretical one.
 * ------------------------------------------------------------------------ */

static uint32_t s_id_rev = 0UL;

static uint8_t s_tx_buf[ETH_FRAME_MAX] __attribute__((aligned(4)));
static uint8_t s_rx_buf[ETH_FRAME_MAX] __attribute__((aligned(4)));

/* ---------------------------------------------------------------------------
 * Indirect MAC register access
 * ------------------------------------------------------------------------ */

static bool mac_csr_wait_idle(void)
{
    uint32_t spins = 0UL;

    while (spins < LAN9118_WAIT_ITERATIONS) {
        if ((LAN9118_REG(REG_MAC_CSR_CMD) & MAC_CSR_CMD_BUSY) == 0UL) {
            return true;
        }
        spins++;
    }
    return false;
}

static bool mac_csr_write(uint32_t reg, uint32_t val)
{
    if (!mac_csr_wait_idle()) {
        return false;
    }
    LAN9118_REG(REG_MAC_CSR_DATA) = val;
    LAN9118_REG(REG_MAC_CSR_CMD)  =
        MAC_CSR_CMD_BUSY | (reg & MAC_CSR_CMD_ADDR_MASK);
    return mac_csr_wait_idle();
}

static bool mac_csr_read(uint32_t reg, uint32_t *out)
{
    if (out == NULL) {
        return false;
    }
    if (!mac_csr_wait_idle()) {
        return false;
    }
    LAN9118_REG(REG_MAC_CSR_CMD) =
        MAC_CSR_CMD_BUSY | MAC_CSR_CMD_READ | (reg & MAC_CSR_CMD_ADDR_MASK);
    if (!mac_csr_wait_idle()) {
        return false;
    }
    *out = LAN9118_REG(REG_MAC_CSR_DATA);
    return true;
}

/* ---------------------------------------------------------------------------
 * Transmit
 * ------------------------------------------------------------------------ */

static err_t lan9118_linkoutput(struct netif *netif, struct pbuf *p)
{
    uint32_t len;
    uint32_t words;
    uint32_t i;
    uint32_t cmd_a;
    uint32_t cmd_b;

    if ((netif == NULL) || (p == NULL)) {
        return ERR_ARG;
    }

    len = (uint32_t)p->tot_len;
    if ((len == 0UL) || (len > ETH_FRAME_MAX)) {
        return ERR_VAL;
    }

    /* Flatten the pbuf chain into a word-aligned staging buffer. The TX FIFO
     * is written 32 bits at a time, so a chain whose segments have arbitrary
     * lengths cannot be streamed directly without re-packing anyway. */
    (void)memset(s_tx_buf, 0, sizeof(s_tx_buf));
    if (pbuf_copy_partial(p, s_tx_buf, (u16_t)len, 0U) != (u16_t)len) {
        return ERR_BUF;
    }

    /* Single-segment transfer: first and last at once, zero data offset. */
    cmd_a = TX_CMD_A_FIRST_SEG | TX_CMD_A_LAST_SEG |
            (len & TX_CMD_A_BUFSIZE_MASK);
    /* Command B: the low half is the frame length; the high half is a tag
     * echoed back through the TX status FIFO. We do not read that FIFO, so
     * the tag value only has to be stable. */
    cmd_b = (len & 0x0000FFFFUL) | ((len & 0x0000FFFFUL) << 16U);

    LAN9118_REG(REG_TX_DATA_FIFO) = cmd_a;
    LAN9118_REG(REG_TX_DATA_FIFO) = cmd_b;

    words = (len + (BYTES_PER_WORD - 1U)) / BYTES_PER_WORD;
    for (i = 0UL; i < words; i++) {
        uint32_t w;
        /* Little-endian assembly, matching QEMU's tx_fifo_push(). Built byte
         * by byte rather than by casting s_tx_buf to uint32_t* so that this
         * stays free of alignment and strict-aliasing assumptions. */
        w  =  (uint32_t)s_tx_buf[(i * BYTES_PER_WORD) + 0U];
        w |= ((uint32_t)s_tx_buf[(i * BYTES_PER_WORD) + 1U]) << 8U;
        w |= ((uint32_t)s_tx_buf[(i * BYTES_PER_WORD) + 2U]) << 16U;
        w |= ((uint32_t)s_tx_buf[(i * BYTES_PER_WORD) + 3U]) << 24U;
        LAN9118_REG(REG_TX_DATA_FIFO) = w;
    }

    MIB2_STATS_NETIF_ADD(netif, ifoutoctets, (u32_t)len);
    LINK_STATS_INC(link.xmit);

    return ERR_OK;
}

/* ---------------------------------------------------------------------------
 * Receive
 * ------------------------------------------------------------------------ */

/** Pop and discard the frame currently at the head of the RX FIFO. */
static void lan9118_rx_discard(void)
{
    LAN9118_REG(REG_RX_DP_CTRL) = RX_DP_CTRL_FFWD;
}

/**
 * Read one complete frame out of the RX data FIFO into @p s_rx_buf.
 *
 * @param status   the frame's RX status word, already popped.
 * @param out_len  receives the payload length (CRC stripped).
 * @return true if a frame was read and is worth handing to lwIP.
 */
static bool lan9118_rx_frame(uint32_t status, uint32_t *out_len)
{
    uint32_t frame_len;
    uint32_t payload_len;
    uint32_t words;
    uint32_t i;

    frame_len = (status >> RX_STATUS_LEN_SHIFT) & RX_STATUS_LEN_MASK;

    if ((status & RX_STATUS_ERROR) != 0UL) {
        lan9118_rx_discard();
        return false;
    }

    /* frame_len counts the 4-byte CRC the MAC appends. */
    if ((frame_len <= ETH_CRC_LEN) ||
        (frame_len > (ETH_FRAME_MAX + ETH_CRC_LEN))) {
        lan9118_rx_discard();
        return false;
    }
    payload_len = frame_len - ETH_CRC_LEN;
    if (payload_len < ETH_FRAME_MIN) {
        lan9118_rx_discard();
        return false;
    }

    /* The FIFO delivers whole words; read the CRC too and drop it. */
    words = (frame_len + (BYTES_PER_WORD - 1U)) / BYTES_PER_WORD;
    for (i = 0UL; i < words; i++) {
        uint32_t w   = LAN9118_REG(REG_RX_DATA_FIFO);
        uint32_t off = i * BYTES_PER_WORD;

        /* Only store bytes that fall inside the payload; the trailing CRC
         * and any final partial word are read to drain the FIFO, not kept. */
        if ((off + 0U) < payload_len) {
            s_rx_buf[off + 0U] = (uint8_t)(w & 0xFFUL);
        }
        if ((off + 1U) < payload_len) {
            s_rx_buf[off + 1U] = (uint8_t)((w >> 8U) & 0xFFUL);
        }
        if ((off + 2U) < payload_len) {
            s_rx_buf[off + 2U] = (uint8_t)((w >> 16U) & 0xFFUL);
        }
        if ((off + 3U) < payload_len) {
            s_rx_buf[off + 3U] = (uint8_t)((w >> 24U) & 0xFFUL);
        }
    }

    *out_len = payload_len;
    return true;
}

uint32_t lan9118_poll(struct netif *netif)
{
    uint32_t delivered = 0UL;
    uint32_t pending;

    if (netif == NULL) {
        return 0UL;
    }

    pending = (LAN9118_REG(REG_RX_FIFO_INF) >> RX_FIFO_INF_SFUSED_SHIFT) &
              RX_FIFO_INF_SFUSED_MASK;

    while (pending > 0UL) {
        uint32_t status = LAN9118_REG(REG_RX_STATUS_FIFO);
        uint32_t len    = 0UL;

        pending--;

        if (lan9118_rx_frame(status, &len)) {
            struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);

            if (p == NULL) {
                /* Out of pbufs: the frame is already drained from the FIFO,
                 * so it is simply lost. TCP will retransmit. */
                LINK_STATS_INC(link.memerr);
                LINK_STATS_INC(link.drop);
            } else {
                if (pbuf_take(p, s_rx_buf, (u16_t)len) != ERR_OK) {
                    (void)pbuf_free(p);
                    LINK_STATS_INC(link.drop);
                } else {
                    MIB2_STATS_NETIF_ADD(netif, ifinoctets, (u32_t)len);
                    LINK_STATS_INC(link.recv);

                    if (netif->input(p, netif) != ERR_OK) {
                        (void)pbuf_free(p);
                        LINK_STATS_INC(link.drop);
                    } else {
                        delivered++;
                    }
                }
            }
        }
    }

    return delivered;
}

/* ---------------------------------------------------------------------------
 * Initialisation
 * ------------------------------------------------------------------------ */

uint32_t lan9118_id_rev(void)
{
    return s_id_rev;
}

err_t lan9118_netif_init(struct netif *netif)
{
    uint32_t byte_test;
    uint32_t spins = 0UL;
    uint32_t mac_lo;
    uint32_t mac_hi;
    uint32_t mac_cr = 0UL;

    if (netif == NULL) {
        return ERR_ARG;
    }

    /* 1. Presence/endianness probe before touching anything else. */
    byte_test = LAN9118_REG(REG_BYTE_TEST);
    if (byte_test != BYTE_TEST_EXPECTED) {
        board_uart_puts("[eth] BYTE_TEST mismatch: ");
        board_uart_put_hex32(byte_test);
        board_uart_puts("\r\n");
        return ERR_IF;
    }

    s_id_rev = LAN9118_REG(REG_ID_REV);
    if ((s_id_rev & ID_REV_CHIP_MASK) != ID_REV_LAN9118_CHIP) {
        board_uart_puts("[eth] unexpected ID_REV: ");
        board_uart_put_hex32(s_id_rev);
        board_uart_puts("\r\n");
        return ERR_IF;
    }

    /* 2. Soft reset, then wait for it to clear (bounded). */
    LAN9118_REG(REG_HW_CFG) = HW_CFG_SRST;
    while (((LAN9118_REG(REG_HW_CFG) & HW_CFG_SRST) != 0UL) &&
           (spins < LAN9118_WAIT_ITERATIONS)) {
        spins++;
    }
    if (spins >= LAN9118_WAIT_ITERATIONS) {
        board_uart_puts("[eth] soft reset did not complete\r\n");
        return ERR_IF;
    }

    /* 3. Interrupts stay masked — this driver is polled. */
    LAN9118_REG(REG_INT_EN)  = 0UL;
    LAN9118_REG(REG_INT_STS) = 0xFFFFFFFFUL;

    /* 4. Program the station address from the netif hwaddr.
     *    ADDRL holds bytes 0..3, ADDRH bytes 4..5 — this ordering is what
     *    QEMU's do_mac_write() writes back into its filter address, so
     *    getting it wrong makes unicast frames silently vanish. */
    mac_lo = ((uint32_t)netif->hwaddr[0])        |
             (((uint32_t)netif->hwaddr[1]) << 8U) |
             (((uint32_t)netif->hwaddr[2]) << 16U) |
             (((uint32_t)netif->hwaddr[3]) << 24U);
    mac_hi = ((uint32_t)netif->hwaddr[4])        |
             (((uint32_t)netif->hwaddr[5]) << 8U);

    if (!mac_csr_write(MAC_REG_ADDRL, mac_lo) ||
        !mac_csr_write(MAC_REG_ADDRH, mac_hi)) {
        board_uart_puts("[eth] MAC address programming failed\r\n");
        return ERR_IF;
    }

    /* 5. No RX offset/padding — the driver reads packed words. */
    LAN9118_REG(REG_RX_CFG) = 0UL;

    /* 6. Flush both TX FIFOs, then enable the transmitter. */
    LAN9118_REG(REG_TX_CFG) = TX_CFG_TXS_DUMP | TX_CFG_TXD_DUMP;
    LAN9118_REG(REG_TX_CFG) = TX_CFG_TX_ON;

    /* 7. Enable MAC TX and RX. BCAST_DISABLE deliberately left clear so ARP
     *    requests are received. */
    if (!mac_csr_write(MAC_REG_CR, MAC_CR_TXEN | MAC_CR_RXEN)) {
        board_uart_puts("[eth] MAC_CR write failed\r\n");
        return ERR_IF;
    }
    if (!mac_csr_read(MAC_REG_CR, &mac_cr)) {
        return ERR_IF;
    }
    if ((mac_cr & (MAC_CR_TXEN | MAC_CR_RXEN)) !=
        (MAC_CR_TXEN | MAC_CR_RXEN)) {
        board_uart_puts("[eth] MAC_CR readback wrong: ");
        board_uart_put_hex32(mac_cr);
        board_uart_puts("\r\n");
        return ERR_IF;
    }

    /* 8. Describe the interface to lwIP. */
#if LWIP_NETIF_HOSTNAME
    netif->hostname = "eds-doip";
#endif
    netif->name[0]     = 'e';
    netif->name[1]     = 'n';
    netif->output      = etharp_output;
    netif->linkoutput  = lan9118_linkoutput;
    netif->mtu         = 1500U;
    netif->hwaddr_len  = ETH_HWADDR_LEN;
    netif->flags       = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                         NETIF_FLAG_LINK_UP | NETIF_FLAG_ETHERNET;

    NETIF_INIT_SNMP(netif, snmp_ifType_ethernet_csmacd, 100000000UL);

    return ERR_OK;
}
