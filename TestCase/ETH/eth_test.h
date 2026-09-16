// eth_test.h
//
// Ethernet PHY bring-up test - the LAN8742A on the Bridge board, reached over
// the MAC's MDIO management interface.
//
// *** This deliberately does NOT bring up lwIP, the ETH DMA or the RMII data
// *** path. MDIO is a separate management interface: MDC is generated from
// *** HCLK, so the PHY answers before the link is up and without the 50 MHz
// *** RMII reference clock the data path needs. That is what lets this run
// *** from the port tool image, which has no network stack in it at all.
//
// What it therefore proves, and what it does not:
//
//   proves        the PHY is present and powered, the MDC/MDIO pair is wired
//                 (PC1 / PA2), the PHY's identity, whether a cable is plugged
//                 in, and what speed and duplex auto-negotiation settled on
//   does NOT      that a single frame can be carried. Throughput needs the
//                 DMA, a stack and a peer - that is the eth *session*
//                 (porttool_eth.c), which is a separate thing from this file
//                 and brings lwIP up when it starts
//
// This is the "10/100 Mbps auto-negotiation" observation the production test
// guide asks for at stations 6 and 10 (OPLC-MFG-TEST-001 3.6 / 3.10).
//
// Pin and clock values are taken from the jointly-debugged network bring-up in
// LWIP/Target/ethernetif.c (HAL_ETH_MspInit) - PC1 = ETH_MDC, PA2 = ETH_MDIO,
// AF11. Only those two of the nine RMII pins are touched here.
//
// ⚠️ The PHY part on this board is the commercial-temperature grade (0..70 C).
// The hardware specification's own note says an industrial part is needed for
// the -20..55 C target, so a PHY that answers here at room temperature is not
// evidence about the temperature range.

#ifndef INC_ETH_TEST_H_
#define INC_ETH_TEST_H_

#include <stdint.h>

/* Reported as measured, with no verdict: the limit belongs to the PC (see
 * $PROD/docs/tables/DECISIONS.md 22). */
typedef struct {
    uint8_t  mdio_ready;   /* the MDIO interface was set up */
    uint8_t  found;        /* a PHY answered at some address */
    uint8_t  addr;         /* which address, 0..31; only valid when found */
    uint32_t phy_id;       /* PHYID1<<16 | PHYID2, the part's identity */
    uint16_t bcr;          /* register 0, raw */
    uint16_t bsr;          /* register 1, raw */
    uint16_t scsr;         /* register 31, raw - LAN8742A speed/duplex */
    uint8_t  link;         /* BSR bit 2 */
    uint8_t  autoneg_done; /* BSR bit 5 */
    uint16_t speed_mbit;   /* 10 or 100, 0 when not resolved */
    uint8_t  full_duplex;
    uint32_t mdio_errors;  /* MDIO transactions that timed out */
} eth_probe_t;

/* One pass: set up MDIO if needed, find the PHY, read it, fill in and return.
 * Returns 1 when a PHY answered. Exists for pt.run. */
int ETH_Test_Probe(eth_probe_t *out);

/* Runs forever, printing the same readings once a second - for a person with a
 * cable in one hand. Plug and unplug it and watch link= follow. */
void ETH_Test_Run(void);

#endif /* INC_ETH_TEST_H_ */
