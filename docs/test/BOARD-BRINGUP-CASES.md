# 板级测试用例

**`TestCase/` 下一个端口一个目录，共 10 项，全部通过（2026-09-03）。**

入口由 `Core/Src/main.c` 顶部那组宏选，同时只能开一个（`main.c` 里有 `#error` 守着，每个 `*_Test_Run()` 都不返回）。没有宏的那几项，在 `main.c` 里直接调它的入口函数。

日志一律走 UART4：**RS232 TxD**（端子 C05，UpperDeck J10-4）、**RS232 RxD**（端子 C06，J10-3），115200 8N1，适配器必须是 RS232 电平。

| 用例 | 接线 | 判据 | 结果 |
|---|---|---|---|
| **Digital In**<br>`TestCase/DIN/din_test.c`<br>`BRINGUP_TEST_ENABLE` | 不接。八个脚是高阻输入 | 每秒打印 `PB5 PC6 PB6 PB7 PH10 PH11 PI5 PI6` 八个脚的电平，与外部实际驱动一致 | ✅ |
| **模拟输入**<br>`TestCase/ADC/adc_test.c`<br>`BRINGUP_TEST_ENABLE` | 可调电压源接 **Analog In 1**（UpperDeck J3-4，PC3_C）或 **Analog In 2**（J4-1，PA6），地接 **Analog GND**（J4-4 / J4-5） | 打印原始计数、引脚电压、按档位折算的端子电压；读数随输入电压变化 | ✅ |
| **板载温度**<br>`TestCase/ADC/adc_test.c`<br>`BRINGUP_TEST_ENABLE` | 不接。PA0 = T-PS，PA3 = T-HS | 打印 ADC 电压和 `T = (mV − 500) / 10`，两路读数与室温相符 | ✅ |
| **模拟输出**<br>`TestCase/DAC/dac_test.c`<br>`BRINGUP_TEST_ENABLE` | 电流表串在回路里：**Analog Out 1**（UpperDeck J4-2，PA4）或 **Analog Out 2**（J4-3，PA5）→ 表 → **Analog GND** | 设 500 mV / 1500 mV，电流表读到 **4.883 mA / 14.648 mA**（`Iout = Vin × 10 / 1024`） | ✅ |
| **RS232**<br>`TestCase/RS232/rs232_test.c`<br>`main.c` 里调 `RS232_Test_Run()` | USB-RS232 适配器接端子 C05 / C06，GND 接 C02 / C11 / C12 | 开机看得到打印 = 发送通；敲键每个字符原样回显 = 接收通 | ✅ |
| **RS485**<br>`TestCase/RS485/rs485_test.c`<br>`RS485_TEST_ENABLE` | USB-RS485 适配器 A 接端子 **A10**（UpperDeck J11-3），B 接 **A11**（J11-2）。必须有第二台设备：PD4 同时驱动 `/RE` 与 `DE`，板子听不见自己 | 三条：PD4/PD5 当 GPIO 推 0/1 读回一致；适配器每 **3 s** 收到一帧 `RS485 HELLO <n>`；主机发的探针原样回来，同时在日志口打出 ASCII + hex 两列 | ✅ |
| **CAN**<br>`TestCase/CAN/can_test.c`<br>`CAN_TEST_ENABLE`（另有 `CAN_SOAK_` / `CAN_SCOPE_` / `CAN_ECHO_` 三个变体） | **CAN H** = 端子 C08（UpperDeck J10-1），**CAN L** = C07（J10-2），**CAN_GND** = A09（J11-4）。500 kbit/s，Classic CAN，标准帧 | 四段：P0 打印外设与配置；P1 内部回环收到自己发的帧；P2 外部回环同样收到；P3 监听模式收到对端的帧；P4 正常模式与第二个节点收发成功 | ✅ |
| **KNX**<br>`TestCase/KNX/knx_test.c`<br>`KNX_TEST_ENABLE` | KNX 总线接 UpperDeck 的 KNX 端子。TP1 位时序由 MCU 自己产生，104 µs/bit | 收：总线安静 `KNX_RX_FLUSH_MS` 后整串打印，raw 与 bit-inverted 两种读法都按 KNX 服务解析。发：`KNX_TX_ENABLE` 置 1，发出的脉冲经收发器回到 RX 被自己收到 | ✅ |
| **PWM**<br>`TestCase/PWM/pwm_test.c`<br>`main.c` 里调 `PWM_Test_Run()` | LED + 限流电阻接 **Digital Out 6**（端子 A08，PA9 = TIM1_CH2） | 占空比 0% → 100% → 0% 缓慢来回，LED 呼吸式亮灭 | ✅ |
| **SD 卡**<br>`TestCase/SD/sd_test.c`<br>`main.c` 里调 `SD_Test_Info()` 或 `SD_Test_FileIntegrity()` | microSD 插进 Bridge 板 J6。1-bit 总线（只接了 DAT0） | `SD_Test_Info()`：打印卡类型、容量、块大小、速度等级，拔插卡时 2 秒内跟着变；`SD_Test_FileIntegrity()`：FatFs 写 4 KiB 的 `0:/PLCTEST.BIN` 再读回，逐字节相同且 CRC32 一致 | ✅ |
| **SDRAM**<br>`TestCase/SDRAM/sdram_test.c`<br>`main.c` 里调三个入口之一 | 不接。板内 U6 = AS4C32M16SB-7BIN，64 MiB，映射在 `0xC0000000` | `SDRAM_Test_Capacity()` 容量与地址回绕正确；`SDRAM_Test_Retention()` 长时间反复写读不出错；`SDRAM_Test_CubeProgrammerVerify()` 用 STM32CubeProgrammer 写进去的字节，板子算出的 CRC32 与 PC 侧一致 | ✅ |

## 五项一次烧录同时跑

`BRINGUP_TEST_ENABLE` 走 `TestCase/common/bringup_test.c`，把 Digital In、继电器、模拟输入、模拟输出、板载温度五项一起跑。每项都是非阻塞 tick。

串口按键（端子 C05 / C06，115200 8N1）：`1` `2` `3` `4` `b` 单独开关某一项（`b` = 板载温度），`a` 全开，`?` 看帮助。

## 代码放哪

一个端口一个子目录，都在 `TestCase/` 下。`TestCase/common/` 装公用件：`testcase_hal_guard.h`、`bringup_test.c`、vendored FatFs，以及工程里唯一自带 HAL 副本的地方（ADC / DAC / SD / SDMMC / FDCAN）。`TestCase/common` 必须在 include path 上（`.cproject` 两个 build config 各一条 `../TestCase/common`）。
