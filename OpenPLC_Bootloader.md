# OpenPLC Bootloader - STM32H743 项目结构与功能概述

## 1. 项目定位

- 目标芯片：STM32H743IIKx（Cortex-M7，2 MB Flash / 1 MB RAM）
- 本项目是 **Bootloader**（与 Application 分开工程）
- 构建方式：STM32CubeIDE 经典 Managed Build（`.cproject` + `Debug/` 目录）；仓库里另有一份 `CMakeLists.txt`（CubeMX 生成，当前实际构建不使用这套 CMake 工程）

## 2. Flash 分区

`Core/Inc/usbd_cdc_flash.h`：

```c
#define ADDRESS_VECTOR   0x20000        // 128KB 偏移
#define RESERVED_SECTORS 1              // 预留 1 个 sector（128K）
#define IAP_APP_ADDRESS  (FLASH_BASE_ADDR | ADDRESS_VECTOR)  // = 0x08020000
```

即当前代码里 Bootloader 预留 1 个 sector（`0x08000000` ~ `0x0801FFFF`，128K），Application 从 `0x08020000`（sector 1）开始。

## 3. 当前编译结果（Debug 配置）

- `Debug/open_plc_cube_ide.bin` = 140,100 字节 (~136.8 KB)
- 段分布：`.isr_vector` 0x298 + `.text` 0x1E854 (125,524B) + `.rodata` 0x36A8 (13,992B) + `.data` 0x198 (408B)

### Flash 占用主要构成（`.text`，按目标文件汇总，从大到小）

| 模块 | 大小(约) |
|---|---|
| lwIP TCP (`tcp_in/tcp_out/tcp.c`) | ~24 KB |
| lwIP DHCP (`dhcp.c`) | ~7.5 KB |
| HAL RCC/RCC_EX（时钟配置） | ~12 KB |
| HAL ETH / lan8742 PHY driver | ~6 KB |
| USB Device（`hal_pcd` + `ll_usb` + USBD Core/CDC/ctlreq + `usbd_conf`） | ~23 KB |
| HAL UART | ~4.3 KB |
| lwIP ARP/IP4/pbuf/udp/mem 等 | ~13 KB |
| 应用层（`IAP_server.c`/`tcp_server.c`/`udp_server.c`） | ~2.6 KB |
| libc_nano（printf/scanf 族） | ~6.4 KB |

## 4. 功能模块清单

| 文件 | 功能 |
|---|---|
| `main.c` | 系统时钟/MPU/外设初始化，主循环调用 `IAP_task()` + `MX_LWIP_Process()` |
| `IAP_server.c` | 核心状态机：解析 `ping/info/run/flash <size> <crc32>` 命令，接收固件数据写 Flash，CRC32 校验后复位跳转 App；同时决定"是否停留在 Bootloader"（BOOT0 按键 / App 无效 / RTC 备份寄存器 magic flag） |
| `tcp_server.c` | lwIP raw-TCP server（端口 `OPENPLC_SERVER_PORT=56865`），固件传输通道 |
| `udp_server.c` | lwIP UDP，做设备发现（收到 `DISCOVER/ping` 等回复 `设备名_UID_BOOTLD_版本`） |
| `usbd_cdc_flash.c` | Flash 擦除/编程（`HAL_FLASH_Program`），供 IAP 使用；含 Flash sector 地址表 |
| USB CDC (`USB_DEVICE/*`) | 第二条固件升级通道（USB虚拟串口）；`IAP_config.h` 中定义了 "1200bps magic touch" 触发进 Bootloader 的机制（`IAP_CDC_reboot_trigger()` 当前未被 `usbd_cdc_if.c` 的回调调用） |
| `relay.c` | 上电时给 6 路 HSFET 继电器做一次开→延时→关 的自检时序 |
| `rtc.c` | 用 RTC 备份寄存器 (`MAGIC_BKP_REG`) 存"为什么进入 Bootloader"的标志（CDC/ETH/APP） |
| `crc.c` | 硬件 CRC32，用于固件校验 |
| `md5.c` | 已包含但未被任何代码调用（`IAP_server.c` 只 `#include "md5.h"`，实际校验用的是 CRC32） |

## 5. lwIP 配置要点（`LWIP/Target/lwipopts.h`）

- `NO_SYS=1`，`WITH_RTOS=0`（裸机轮询，不用 FreeRTOS/线程模型）
- `LWIP_DHCP=1`
- `LWIP_TCP` 默认开启（未在 lwipopts 中覆盖）
- `LWIP_NETCONN=0`，`LWIP_SOCKET=0`（只用 raw API，不用 socket/netconn）
- `CHECKSUM_BY_HARDWARE=1`（硬件校验和）

## 6. 构建配置

- `.cproject` 中曾经存在的两组重复 Debug/Release 配置已清理，现在只保留一个 Debug 配置（`-O0`），Debug 图标和"直接烧录"图标都固定用这一个配置的产物，不再需要 Release
- `Debug/open_plc_cube_ide.bin` 即为第 3 节所述的 137KB 产物
