// port_can.c
//
// FDCAN1 hardware layer - see port_can.h.
//
// Lifted out of TestCase/CAN/can_test.c on 2026-09-08, value for value, so the
// port tool could drive CAN without entering a test that never returns. The
// timing table, the FIFO sizes and the "no filter at all" choice are that
// file's, with its reasoning kept.

#include "port_can.h"

#include <stdio.h>

#define CAN_TX_PORT GPIOB
#define CAN_TX_PIN  GPIO_PIN_9    /* FDCAN1_TX -> U8 pin 1 */
#define CAN_RX_PORT GPIOI
#define CAN_RX_PIN  GPIO_PIN_9    /* FDCAN1_RX <- U8 pin 3 */
#define CAN_PIN_AF  GPIO_AF9_FDCAN1

/* Valid only while the kernel clock is HSE. 1 + Seg1 + Seg2 = tq per bit, and
 * HSE / (prescaler * tq) is the bit rate exactly, with no rounding. */
typedef struct {
    uint32_t bps;
    uint16_t prescaler;
    uint16_t seg1;
    uint16_t seg2;
    uint16_t sjw;
} can_timing_t;

static const can_timing_t CAN_TIMINGS[PORT_CAN_RATE_COUNT] = {
    {  125000u, 1u, 174u, 25u, 4u },
    {  250000u, 1u,  87u, 12u, 4u },
    {  500000u, 1u,  43u,  6u, 4u },
    { 1000000u, 1u,  21u,  3u, 3u },
};

/* 32 frames of slack between the bus and the console: a frame at 500 kbit/s is
 * about 110 us and one console line costs 5 ms at 115200 baud, so the FIFO is
 * what keeps a burst from being lost while printing. HAL caps FIFO0 at 64
 * elements and TxBuffers + TxFifoQueue at 32. */
#define CAN_RX_FIFO_ELMTS 32u
#define CAN_TX_FIFO_ELMTS  8u

static FDCAN_HandleTypeDef s_h;
static int s_pins_ready;
static int s_open;

uint32_t PortCan_RateBps(uint8_t index)
{
    if (index >= PORT_CAN_RATE_COUNT) {
        return 0u;
    }
    return CAN_TIMINGS[index].bps;
}

int PortCan_RateIndex(uint32_t bps, uint8_t *out_index)
{
    for (uint8_t i = 0; i < PORT_CAN_RATE_COUNT; i++) {
        if (CAN_TIMINGS[i].bps == bps) {
            if (out_index != NULL) {
                *out_index = i;
            }
            return 1;
        }
    }
    return 0;
}

int PortCan_Timing(uint8_t index, uint16_t *prescaler, uint16_t *seg1,
                   uint16_t *seg2, uint16_t *sjw)
{
    const can_timing_t *t;

    if (index >= PORT_CAN_RATE_COUNT) {
        return 0;
    }
    t = &CAN_TIMINGS[index];
    if (prescaler != NULL) { *prescaler = t->prescaler; }
    if (seg1 != NULL)      { *seg1 = t->seg1; }
    if (seg2 != NULL)      { *seg2 = t->seg2; }
    if (sjw != NULL)       { *sjw = t->sjw; }
    return 1;
}

uint32_t PortCan_Dlc(uint8_t len)
{
    static const uint32_t DLC[9] = {
        FDCAN_DLC_BYTES_0, FDCAN_DLC_BYTES_1, FDCAN_DLC_BYTES_2,
        FDCAN_DLC_BYTES_3, FDCAN_DLC_BYTES_4, FDCAN_DLC_BYTES_5,
        FDCAN_DLC_BYTES_6, FDCAN_DLC_BYTES_7, FDCAN_DLC_BYTES_8
    };
    return DLC[(len > 8u) ? 8u : len];
}

int PortCan_Init(void)
{
    RCC_PeriphCLKInitTypeDef p = { 0 };
    GPIO_InitTypeDef g = { 0 };

    if (s_pins_ready) {
        return 1;
    }

    p.PeriphClockSelection = RCC_PERIPHCLK_FDCAN;
    p.FdcanClockSelection  = RCC_FDCANCLKSOURCE_HSE;
    if (HAL_RCCEx_PeriphCLKConfig(&p) != HAL_OK) {
        printf("CAN_TEST: FDCAN kernel clock select FAILED\r\n");
        return 0;
    }
    __HAL_RCC_FDCAN_CLK_ENABLE();

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOI_CLK_ENABLE();

    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_NOPULL;
    g.Speed     = GPIO_SPEED_FREQ_LOW;
    g.Alternate = CAN_PIN_AF;

    g.Pin = CAN_TX_PIN;
    HAL_GPIO_Init(CAN_TX_PORT, &g);
    g.Pin = CAN_RX_PIN;
    HAL_GPIO_Init(CAN_RX_PORT, &g);

    s_pins_ready = 1;
    return 1;
}

int PortCan_Open(uint8_t rate_index, uint32_t mode, int auto_retx)
{
    const can_timing_t *t;

    if (rate_index >= PORT_CAN_RATE_COUNT) {
        return 0;
    }
    if (!PortCan_Init()) {
        return 0;
    }
    if (s_open) {
        PortCan_Close();
    }
    t = &CAN_TIMINGS[rate_index];

    s_h.Instance                  = FDCAN1;
    s_h.Init.FrameFormat          = FDCAN_FRAME_CLASSIC;
    s_h.Init.Mode                 = mode;
    s_h.Init.AutoRetransmission   = auto_retx ? ENABLE : DISABLE;
    s_h.Init.TransmitPause        = DISABLE;
    s_h.Init.ProtocolException    = DISABLE;
    s_h.Init.NominalPrescaler     = t->prescaler;
    s_h.Init.NominalSyncJumpWidth = t->sjw;
    s_h.Init.NominalTimeSeg1      = t->seg1;
    s_h.Init.NominalTimeSeg2      = t->seg2;
    s_h.Init.DataPrescaler        = 1u;   /* classic CAN: data phase unused */
    s_h.Init.DataSyncJumpWidth    = 1u;
    s_h.Init.DataTimeSeg1         = 1u;
    s_h.Init.DataTimeSeg2         = 1u;
    s_h.Init.MessageRAMOffset     = 0u;
    s_h.Init.StdFiltersNbr        = 0u;   /* the global filter takes everything */
    s_h.Init.ExtFiltersNbr        = 0u;
    s_h.Init.RxFifo0ElmtsNbr      = CAN_RX_FIFO_ELMTS;
    s_h.Init.RxFifo0ElmtSize      = FDCAN_DATA_BYTES_8;
    s_h.Init.RxFifo1ElmtsNbr      = 0u;
    s_h.Init.RxFifo1ElmtSize      = FDCAN_DATA_BYTES_8;
    s_h.Init.RxBuffersNbr         = 0u;
    s_h.Init.RxBufferSize         = FDCAN_DATA_BYTES_8;
    s_h.Init.TxEventsNbr          = 0u;
    s_h.Init.TxBuffersNbr         = 0u;
    s_h.Init.TxFifoQueueElmtsNbr  = CAN_TX_FIFO_ELMTS;
    s_h.Init.TxFifoQueueMode      = FDCAN_TX_FIFO_OPERATION;
    s_h.Init.TxElmtSize           = FDCAN_DATA_BYTES_8;

    if (HAL_FDCAN_Init(&s_h) != HAL_OK) {
        printf("CAN_TEST: HAL_FDCAN_Init FAILED\r\n");
        return 0;
    }

    /* No ID filter at all: during bring-up the question is whether anything
     * arrived, not whether the expected ID arrived. */
    if (HAL_FDCAN_ConfigGlobalFilter(&s_h, FDCAN_ACCEPT_IN_RX_FIFO0,
                                     FDCAN_ACCEPT_IN_RX_FIFO0,
                                     FDCAN_FILTER_REMOTE,
                                     FDCAN_FILTER_REMOTE) != HAL_OK) {
        printf("CAN_TEST: HAL_FDCAN_ConfigGlobalFilter FAILED\r\n");
        return 0;
    }

    if (HAL_FDCAN_Start(&s_h) != HAL_OK) {
        printf("CAN_TEST: HAL_FDCAN_Start FAILED\r\n");
        return 0;
    }
    s_open = 1;
    return 1;
}

void PortCan_Close(void)
{
    if (!s_open) {
        return;
    }
    (void) HAL_FDCAN_Stop(&s_h);
    (void) HAL_FDCAN_DeInit(&s_h);
    s_open = 0;
}

int PortCan_Send(uint32_t id, const uint8_t *data, uint8_t len)
{
    FDCAN_TxHeaderTypeDef tx = { 0 };

    if (!s_open) {
        return 0;
    }

    tx.Identifier          = id;
    tx.IdType              = FDCAN_STANDARD_ID;
    tx.TxFrameType         = FDCAN_DATA_FRAME;
    tx.DataLength          = PortCan_Dlc(len);
    tx.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    tx.BitRateSwitch       = FDCAN_BRS_OFF;
    tx.FDFormat            = FDCAN_CLASSIC_CAN;
    tx.TxEventFifoControl  = FDCAN_NO_TX_EVENTS;
    tx.MessageMarker       = 0u;

    return (HAL_FDCAN_AddMessageToTxFifoQ(&s_h, &tx, data) == HAL_OK) ? 1 : 0;
}

int PortCan_Receive(uint32_t *id, uint8_t *data, uint8_t *len)
{
    FDCAN_RxHeaderTypeDef h;
    uint8_t d[8];

    if (!s_open) {
        return 0;
    }
    if (HAL_FDCAN_GetRxFifoFillLevel(&s_h, FDCAN_RX_FIFO0) == 0u) {
        return 0;
    }
    if (HAL_FDCAN_GetRxMessage(&s_h, FDCAN_RX_FIFO0, &h, d) != HAL_OK) {
        return 0;
    }

    if (id != NULL)  { *id = h.Identifier; }
    if (len != NULL) { *len = (uint8_t)((h.DataLength > FDCAN_DLC_BYTES_8) ? 8u : h.DataLength); }
    if (data != NULL) {
        for (int i = 0; i < 8; i++) {
            data[i] = d[i];
        }
    }
    return 1;
}

void PortCan_Counters(uint32_t *tec, uint32_t *rec)
{
    FDCAN_ErrorCountersTypeDef ec = { 0 };

    if (s_open) {
        (void) HAL_FDCAN_GetErrorCounters(&s_h, &ec);
    }
    if (tec != NULL) { *tec = ec.TxErrorCnt; }
    if (rec != NULL) { *rec = ec.RxErrorCnt; }
}

uint32_t PortCan_LastError(void)
{
    FDCAN_ProtocolStatusTypeDef ps = { 0 };

    if (!s_open) {
        return 0u;
    }
    (void) HAL_FDCAN_GetProtocolStatus(&s_h, &ps);
    return ps.LastErrorCode;
}

int PortCan_Alive(void)
{
    return (FDCAN1->ENDN == PORT_CAN_ENDN_EXPECT) ? 1 : 0;
}

uint32_t PortCan_ClockHz(void)
{
    switch (__HAL_RCC_GET_FDCAN_SOURCE()) {
    case RCC_FDCANCLKSOURCE_HSE: return HSE_VALUE;
    default:                     return 0u;   /* not a rate this table covers */
    }
}

const char *PortCan_ClockName(void)
{
    switch (__HAL_RCC_GET_FDCAN_SOURCE()) {
    case RCC_FDCANCLKSOURCE_HSE:  return "HSE";
    case RCC_FDCANCLKSOURCE_PLL:  return "PLL1Q";
    case RCC_FDCANCLKSOURCE_PLL2: return "PLL2Q";
    default:                      return "unknown";
    }
}

FDCAN_HandleTypeDef *PortCan_Handle(void)
{
    return &s_h;
}
