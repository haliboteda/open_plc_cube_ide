// eth_test.c
//
// See eth_test.h for what this proves and what it deliberately does not.

#include "eth_test.h"
#include "main.h"

#include <stdio.h>
#include <string.h>

/* LAN8742A registers, the four that say anything useful. */
#define PHY_BCR         0x00u   /* basic control */
#define PHY_BSR         0x01u   /* basic status */
#define PHY_ID1         0x02u
#define PHY_ID2         0x03u
#define PHY_SCSR        0x1Fu   /* special control/status: speed and duplex */

#define PHY_BSR_LINK    (1u << 2)
#define PHY_BSR_ANDONE  (1u << 5)

/* SCSR bits 4:2 - the LAN8742A's resolved speed and duplex. */
#define PHY_SCSR_SPEED_Pos  2u
#define PHY_SCSR_SPEED_Msk  (0x7u << PHY_SCSR_SPEED_Pos)
#define PHY_SPEED_10_HD     1u
#define PHY_SPEED_100_HD    2u
#define PHY_SPEED_10_FD     5u
#define PHY_SPEED_100_FD    6u

/* An MDIO transaction is a handful of MDC cycles. 10 ms is far more than it
 * can take, and bounding it is the point: a missing PHY must report as absent
 * rather than hang the command loop that asked. */
#define MDIO_TIMEOUT_MS 10u

static uint8_t  eth_mdio_ready;
static uint32_t eth_mdio_errors;

/* MDIO's clock is divided down from HCLK, and the divider has to be told which
 * band HCLK is in. Computed rather than hardcoded so that a change to the
 * clock tree cannot leave a wrong constant behind - the encoding is from the
 * MACMDIOAR CR field. */
static uint32_t eth_mdio_cr(void)
{
    uint32_t hclk = HAL_RCC_GetHCLKFreq();

    if (hclk < 35000000u)  { return 2u << ETH_MACMDIOAR_CR_Pos; }  /* 20-35   */
    if (hclk < 60000000u)  { return 3u << ETH_MACMDIOAR_CR_Pos; }  /* 35-60   */
    if (hclk < 100000000u) { return 0u << ETH_MACMDIOAR_CR_Pos; }  /* 60-100  */
    if (hclk < 150000000u) { return 1u << ETH_MACMDIOAR_CR_Pos; }  /* 100-150 */
    if (hclk < 250000000u) { return 4u << ETH_MACMDIOAR_CR_Pos; }  /* 150-250 */
    if (hclk < 300000000u) { return 5u << ETH_MACMDIOAR_CR_Pos; }  /* 250-300 */
    if (hclk < 500000000u) { return 6u << ETH_MACMDIOAR_CR_Pos; }  /* 300-500 */
    return 7u << ETH_MACMDIOAR_CR_Pos;                             /* 500-800 */
}

/* Two pins and one clock - not the nine RMII pins. Values from
 * LWIP/Target/ethernetif.c's HAL_ETH_MspInit. */
static void ETH_Test_MdioInit(void)
{
    GPIO_InitTypeDef gpio = {0};

    if (eth_mdio_ready) {
        return;
    }

    printf("ETH_TEST: MDIO bring-up - PC1 = MDC, PA2 = MDIO, AF11\r\n");

    __HAL_RCC_ETH1MAC_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF11_ETH;

    gpio.Pin = GPIO_PIN_1;                 /* PC1 = ETH_MDC */
    HAL_GPIO_Init(GPIOC, &gpio);
    gpio.Pin = GPIO_PIN_2;                 /* PA2 = ETH_MDIO */
    HAL_GPIO_Init(GPIOA, &gpio);

    /* CR survives every later transaction: the read below preserves the whole
     * register and only replaces the address and command fields, which is how
     * the HAL does it too. */
    MODIFY_REG(ETH->MACMDIOAR, ETH_MACMDIOAR_CR, eth_mdio_cr());

    printf("ETH_TEST: HCLK %lu Hz, MDIO clock range field 0x%lX\r\n",
           (unsigned long)HAL_RCC_GetHCLKFreq(),
           (unsigned long)((ETH->MACMDIOAR & ETH_MACMDIOAR_CR) >> ETH_MACMDIOAR_CR_Pos));

    eth_mdio_ready = 1;
}

/* Returns 0 on timeout, and then *out is untouched. Written against the
 * registers rather than through HAL_ETH_ReadPHYRegister because that call
 * wants an initialised ETH_HandleTypeDef, which would mean HAL_ETH_Init, which
 * would mean the DMA and the RMII clock - the very things this file exists to
 * avoid needing. */
static int ETH_Test_ReadPhy(uint8_t addr, uint8_t reg, uint16_t *out)
{
    uint32_t cmd = ETH->MACMDIOAR;
    uint32_t start;

    cmd &= ~(ETH_MACMDIOAR_PA | ETH_MACMDIOAR_RDA | ETH_MACMDIOAR_MOC);
    cmd |= ((uint32_t)addr << ETH_MACMDIOAR_PA_Pos) & ETH_MACMDIOAR_PA;
    cmd |= ((uint32_t)reg  << ETH_MACMDIOAR_RDA_Pos) & ETH_MACMDIOAR_RDA;
    cmd |= ETH_MACMDIOAR_MOC_RD;
    cmd |= ETH_MACMDIOAR_MB;

    ETH->MACMDIOAR = cmd;

    start = HAL_GetTick();
    while ((ETH->MACMDIOAR & ETH_MACMDIOAR_MB) != 0u) {
        if ((HAL_GetTick() - start) > MDIO_TIMEOUT_MS) {
            eth_mdio_errors++;
            return 0;
        }
    }

    *out = (uint16_t)(ETH->MACMDIODR & 0xFFFFu);
    return 1;
}

/* Scans instead of assuming an address. The board's own network code lets the
 * vendor BSP discover it, so hardcoding one here would be a second, private
 * assumption that could drift from the board. Reporting which address answered
 * is also the useful half of the answer when a PHY is fitted but strapped
 * differently than expected. */
static int ETH_Test_FindPhy(uint8_t *addr_out, uint32_t *id_out)
{
    for (uint8_t a = 0u; a < 32u; a++) {
        uint16_t id1 = 0, id2 = 0;

        if (!ETH_Test_ReadPhy(a, PHY_ID1, &id1) ||
            !ETH_Test_ReadPhy(a, PHY_ID2, &id2)) {
            continue;
        }
        /* All-ones and all-zeros are what an empty address reads back as. */
        if ((id1 == 0xFFFFu && id2 == 0xFFFFu) || (id1 == 0u && id2 == 0u)) {
            continue;
        }
        *addr_out = a;
        *id_out = ((uint32_t)id1 << 16) | (uint32_t)id2;
        return 1;
    }
    return 0;
}

static void ETH_Test_Decode(eth_probe_t *out)
{
    out->link         = (out->bsr & PHY_BSR_LINK)   ? 1u : 0u;
    out->autoneg_done = (out->bsr & PHY_BSR_ANDONE) ? 1u : 0u;

    switch ((out->scsr & PHY_SCSR_SPEED_Msk) >> PHY_SCSR_SPEED_Pos) {
    case PHY_SPEED_10_HD:  out->speed_mbit = 10;  out->full_duplex = 0; break;
    case PHY_SPEED_100_HD: out->speed_mbit = 100; out->full_duplex = 0; break;
    case PHY_SPEED_10_FD:  out->speed_mbit = 10;  out->full_duplex = 1; break;
    case PHY_SPEED_100_FD: out->speed_mbit = 100; out->full_duplex = 1; break;
    default:
        /* Nothing resolved - no cable, or negotiation still running. Reported
         * as 0 rather than guessed at from BCR, which only says what was
         * advertised and not what the link settled on. */
        out->speed_mbit = 0;
        out->full_duplex = 0;
        break;
    }
}

int ETH_Test_Probe(eth_probe_t *out)
{
    eth_probe_t local;

    if (out == NULL) {
        out = &local;
    }
    memset(out, 0, sizeof(*out));

    ETH_Test_MdioInit();
    out->mdio_ready = eth_mdio_ready;

    if (!ETH_Test_FindPhy(&out->addr, &out->phy_id)) {
        out->mdio_errors = eth_mdio_errors;
        printf("ETH_TEST: no PHY answered on any of the 32 MDIO addresses\r\n");
        return 0;
    }
    out->found = 1;

    (void) ETH_Test_ReadPhy(out->addr, PHY_BCR,  &out->bcr);
    (void) ETH_Test_ReadPhy(out->addr, PHY_BSR,  &out->bsr);
    (void) ETH_Test_ReadPhy(out->addr, PHY_SCSR, &out->scsr);
    ETH_Test_Decode(out);
    out->mdio_errors = eth_mdio_errors;

    printf("ETH_TEST: PHY at address %u, id 0x%08lX, link %s",
           (unsigned)out->addr, (unsigned long)out->phy_id,
           out->link ? "UP" : "down");
    if (out->speed_mbit != 0u) {
        printf(", %u Mbit/s %s duplex", (unsigned)out->speed_mbit,
               out->full_duplex ? "full" : "half");
    }
    printf("\r\n");
    return 1;
}

void ETH_Test_Run(void)
{
    printf("ETH_TEST: PHY watch - plug and unplug the cable, link= follows\r\n");

    for (;;) {
        eth_probe_t p;

        if (ETH_Test_Probe(&p)) {
            printf("ETH_TEST: addr=%u id=0x%08lX link=%u autoneg=%u speed=%u fd=%u"
                   " bcr=0x%04X bsr=0x%04X scsr=0x%04X mdio_errors=%lu\r\n",
                   (unsigned)p.addr, (unsigned long)p.phy_id, (unsigned)p.link,
                   (unsigned)p.autoneg_done, (unsigned)p.speed_mbit,
                   (unsigned)p.full_duplex, (unsigned)p.bcr, (unsigned)p.bsr,
                   (unsigned)p.scsr, (unsigned long)p.mdio_errors);
        }
        HAL_Delay(1000);
    }
}
