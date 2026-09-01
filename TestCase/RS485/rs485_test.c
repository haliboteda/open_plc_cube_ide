// rs485_test.c
//
// RS485 bring-up - see rs485_test.h.

#include "rs485_test.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

#define RS485_BAUD          115200U
#define RS485_DIR_PORT      GPIOD
#define RS485_DIR_PIN       GPIO_PIN_4
#define RS485_TX_PIN        GPIO_PIN_5
#define RS485_RX_PIN        GPIO_PIN_6

#define RS485_BANNER_MS     3000U
/* 0x55 bytes ahead of the banner: alternating bits give the scope a clean
 * square wave to trigger on. Set to 0 to send the text alone. */
#define RS485_SCOPE_PREAMBLE 16
#define RS485_IDLE_GAP_MS   20U      /* silence that ends an inbound frame */
#define RS485_RX_MAX        64U
#define RS485_PROBE_MS      5000U   /* one D1 window */
#define RS485_D2_FRAMES     10
/* ISS-B6 diagnostics. Both off = boot straight into the R2/R4 loop.
 * D1+D2 cost 11 s at boot; D3 never returns and hides R2/R4 entirely. */
#define RS485_DIAG_PHASES   0       /* D1 direction reach + D2 held-driver burst */
#define RS485_D3_METER      0       /* D3 meter square, blocks R2/R4 */
/* Distinct dwell times, so a meter alone tells the steps apart. */
#define RS485_D3_MARK_MS    10000U
#define RS485_D3_SPACE_MS   4000U
#define RS485_D3_OFF_MS     2000U

static UART_HandleTypeDef huart_rs485;

/* PD4 drives /RE and DE at once: high = transmit, low = receive. */
static void RS485_DriveEnable(int on)
{
    HAL_GPIO_WritePin(RS485_DIR_PORT, RS485_DIR_PIN,
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void RS485_Send(const uint8_t *data, uint16_t len)
{
    RS485_DriveEnable(1);
    /* HAL_UART_Transmit returns only after TC, so the last bit is on the wire
     * before the driver is turned off. */
    HAL_StatusTypeDef st = HAL_UART_Transmit(&huart_rs485, (uint8_t *)data, len, 200);
    if (st != HAL_OK) {
        printf("[RS485] HAL_UART_Transmit returned %d\r\n", (int)st);
    }
    RS485_DriveEnable(0);
}

/* Phase R1: nothing needs to be attached. Both pins are driven as plain GPIO
 * and read back, which separates "no cable" from "this pin is dead". */
static void RS485_Phase_PinLevel(void)
{
    GPIO_InitTypeDef gpio = {0};
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Pin   = RS485_DIR_PIN | RS485_TX_PIN;
    HAL_GPIO_Init(RS485_DIR_PORT, &gpio);

    int ok = 1;
    for (int level = 0; level <= 1; level++) {
        GPIO_PinState want = level ? GPIO_PIN_SET : GPIO_PIN_RESET;
        HAL_GPIO_WritePin(RS485_DIR_PORT, RS485_DIR_PIN | RS485_TX_PIN, want);
        HAL_Delay(2);
        GPIO_PinState dir = HAL_GPIO_ReadPin(RS485_DIR_PORT, RS485_DIR_PIN);
        GPIO_PinState tx  = HAL_GPIO_ReadPin(RS485_DIR_PORT, RS485_TX_PIN);
        printf("[R1 ] drive %d -> PD4=%d PD5=%d\r\n", level,
               dir == GPIO_PIN_SET, tx == GPIO_PIN_SET);
        if (dir != want || tx != want) {
            ok = 0;
        }
    }
    printf("[R1 ] %s\r\n", ok ? "PASS - both pins follow what is written"
                              : "FAIL - a pin did not read back what was written");
}

static int RS485_Init(void)
{
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_USART2_CLK_ENABLE();

    RS485_Phase_PinLevel();

    GPIO_InitTypeDef gpio = {0};

    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Pin   = RS485_DIR_PIN;
    HAL_GPIO_Init(RS485_DIR_PORT, &gpio);
    RS485_DriveEnable(0);

    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF7_USART2;
    gpio.Pin       = RS485_TX_PIN;
    HAL_GPIO_Init(RS485_DIR_PORT, &gpio);

    /* RO goes high-Z when /RE is driven high, so the pull-up is what makes an
     * off receiver read as an idle line instead of as noise. D1 depends on it. */
    gpio.Pull = GPIO_PULLUP;
    gpio.Pin  = RS485_RX_PIN;
    HAL_GPIO_Init(RS485_DIR_PORT, &gpio);

    huart_rs485.Instance                    = USART2;
    huart_rs485.Init.BaudRate               = RS485_BAUD;
    huart_rs485.Init.WordLength             = UART_WORDLENGTH_8B;
    huart_rs485.Init.StopBits               = UART_STOPBITS_1;
    huart_rs485.Init.Parity                 = UART_PARITY_NONE;
    huart_rs485.Init.Mode                   = UART_MODE_TX_RX;
    huart_rs485.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
    huart_rs485.Init.OverSampling           = UART_OVERSAMPLING_16;
    huart_rs485.Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
    huart_rs485.Init.ClockPrescaler         = UART_PRESCALER_DIV1;
    huart_rs485.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

    if (HAL_UART_Init(&huart_rs485) != HAL_OK) {
        printf("[RS485] HAL_UART_Init FAILED\r\n");
        return 0;
    }
    return 1;
}

#if RS485_DIAG_PHASES

/* Count inbound bytes for `ms`, leaving the direction pin wherever it is. */
static uint32_t RS485_CountRx(uint32_t ms)
{
    uint32_t t0 = HAL_GetTick();
    uint32_t n  = 0;
    uint8_t  b;
    while ((HAL_GetTick() - t0) < ms) {
        if (HAL_UART_Receive(&huart_rs485, &b, 1, 0) == HAL_OK) {
            n++;
        }
    }
    return n;
}

/* Phase D1: does PD4 actually reach U6? Driving it high must switch the
 * receiver off. /RE and DE are one net, so a receiver that keeps working
 * proves the driver never turns on either. The host must keep sending. */
static void RS485_Phase_DirReach(void)
{
    printf("[D1 ] hold PD4 HIGH %lu ms - host must keep sending\r\n",
           (unsigned long)RS485_PROBE_MS);
    RS485_DriveEnable(1);
    uint32_t hi = RS485_CountRx(RS485_PROBE_MS);

    printf("[D1 ] hold PD4 LOW  %lu ms\r\n", (unsigned long)RS485_PROBE_MS);
    RS485_DriveEnable(0);
    uint32_t lo = RS485_CountRx(RS485_PROBE_MS);

    printf("[D1 ] rx while HIGH=%lu  rx while LOW=%lu\r\n",
           (unsigned long)hi, (unsigned long)lo);
    if (lo == 0) {
        printf("[D1 ] INCONCLUSIVE - nothing arrived either way, host was silent\r\n");
    } else if (hi == 0) {
        printf("[D1 ] PD4 REACHES U6 - receiver went off, so DE goes on too\r\n");
    } else {
        printf("[D1 ] PD4 DOES NOT REACH U6 - still receiving with /RE driven high\r\n");
    }
}

/* Phase D2: the driver held on across a whole burst, so DE timing cannot be
 * why the host hears nothing. */
static void RS485_Phase_HoldAndSend(void)
{
    printf("[D2 ] PD4 held HIGH, sending %d frames back to back\r\n", RS485_D2_FRAMES);
    RS485_DriveEnable(1);
    for (int i = 0; i < RS485_D2_FRAMES; i++) {
        char line[48];
        int n = snprintf(line, sizeof(line), "RS485 D2 BURST %d\r\n", i);
        HAL_StatusTypeDef st = HAL_UART_Transmit(&huart_rs485, (uint8_t *)line,
                                                 (uint16_t)n, 200);
        if (st != HAL_OK) {
            printf("[D2 ] HAL_UART_Transmit returned %d on frame %d\r\n", (int)st, i);
        }
        HAL_Delay(100);
    }
    RS485_DriveEnable(0);
    printf("[D2 ] done - check the adapter\r\n");
}

#endif /* RS485_DIAG_PHASES */

#if RS485_D3_METER

/* Phase D3: a square on the bus itself, slow enough for a DC meter. DI is
 * driven as plain GPIO, so a DI net that is open shows up as a line that will
 * not go to space. The adapter must be unplugged or its driver masks this. */
static void RS485_Phase_MeterSquare(void)
{
    GPIO_InitTypeDef gpio = {0};
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Pin   = RS485_TX_PIN;
    HAL_GPIO_Init(RS485_DIR_PORT, &gpio);

    printf("\r\n[D3 ] meter A and B against GND (J11-1), adapter UNPLUGGED\r\n"
           "[D3 ] steps have different lengths: mark %lu ms, space %lu ms, off %lu ms\r\n",
           (unsigned long)RS485_D3_MARK_MS, (unsigned long)RS485_D3_SPACE_MS,
           (unsigned long)RS485_D3_OFF_MS);

    for (;;) {
        RS485_DriveEnable(1);
        HAL_GPIO_WritePin(RS485_DIR_PORT, RS485_TX_PIN, GPIO_PIN_SET);
        printf("[D3 ] mark  (long,  %lu ms)\r\n", (unsigned long)RS485_D3_MARK_MS);
        HAL_Delay(RS485_D3_MARK_MS);

        HAL_GPIO_WritePin(RS485_DIR_PORT, RS485_TX_PIN, GPIO_PIN_RESET);
        printf("[D3 ] space (mid,   %lu ms)\r\n", (unsigned long)RS485_D3_SPACE_MS);
        HAL_Delay(RS485_D3_SPACE_MS);

        RS485_DriveEnable(0);
        printf("[D3 ] off   (short, %lu ms)\r\n", (unsigned long)RS485_D3_OFF_MS);
        HAL_Delay(RS485_D3_OFF_MS);
    }
}

#endif /* RS485_D3_METER */

void RS485_Test_Run(void)
{
    printf("\r\n=== RS485 bring-up (USART2, SP3485EN) ===\r\n"
           "[R0 ] PD5=TX(AF7) PD6=RX(AF7) PD4=/RE+DE, %lu 8N1\r\n"
           "[R0 ] terminals: A = A10 (J11-3), B = A11 (J11-2)\r\n"
           "[R0 ] half duplex - the board cannot receive while it transmits\r\n",
           (unsigned long)RS485_BAUD);

    if (!RS485_Init()) {
        for (;;) { }
    }

#if RS485_DIAG_PHASES
    RS485_Phase_DirReach();
    RS485_Phase_HoldAndSend();
#endif
#if RS485_D3_METER
    RS485_Phase_MeterSquare();   /* never returns */
#endif

    printf("[R2 ] sending a banner every %lu ms (%d x 0x55 preamble first)\r\n"
           "[R4 ] anything received is echoed back and printed here\r\n\r\n",
           (unsigned long)RS485_BANNER_MS, RS485_SCOPE_PREAMBLE);

    uint8_t  rx[RS485_RX_MAX];
    uint16_t rx_len   = 0;
    uint32_t rx_last  = 0;
    uint32_t next_tx  = HAL_GetTick();
    uint32_t seq      = 0;

    for (;;) {
        uint32_t now = HAL_GetTick();

        uint8_t b;
        if (HAL_UART_Receive(&huart_rs485, &b, 1, 0) == HAL_OK) {
            if (rx_len < RS485_RX_MAX) {
                rx[rx_len++] = b;
            }
            rx_last = now;
        }

        /* A gap in the traffic marks the end of a frame - echo it whole rather
         * than one byte at a time, which would flip the driver on every byte. */
        if (rx_len > 0 && (now - rx_last) >= RS485_IDLE_GAP_MS) {
            printf("[R4 ] got %u bytes: ", (unsigned)rx_len);
            for (uint16_t i = 0; i < rx_len; i++) {
                printf("%c", (rx[i] >= 32 && rx[i] < 127) ? rx[i] : '.');
            }
            printf("\r\n");
            RS485_Send(rx, rx_len);
            rx_len = 0;
            continue;
        }

        if ((int32_t)(now - next_tx) >= 0) {
            next_tx = now + RS485_BANNER_MS;
            char line[80];
            int n = 0;
#if RS485_SCOPE_PREAMBLE
            memset(line, 0x55, RS485_SCOPE_PREAMBLE);
            n = RS485_SCOPE_PREAMBLE;
#endif
            n += snprintf(line + n, sizeof(line) - n, "RS485 HELLO %lu\r\n",
                          (unsigned long)seq++);
            RS485_Send((const uint8_t *)line, (uint16_t)n);
            printf("[R2 ] sent: RS485 HELLO %lu\r\n", (unsigned long)(seq - 1));
        }
    }
}
