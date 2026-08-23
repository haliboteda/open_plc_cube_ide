# OpenPLC Bootloader —— 工程结构概览

**这份只讲"工程长什么样"**：怎么构建、有哪些文件、编出来多大、lwIP 怎么配的。

⚠️ **行为、安全模型、验证状态一律不写在这里，看 [docs/INDEX.md](docs/INDEX.md)。** 这份文档 2026-08-12 建立时把行为也写了一遍，四个月不到就错了四处（见文末），原因很简单：**同一件事写两遍，第二遍必然漂移**。2026-08-17 同步时把那些内容删掉改成指路。

| 想知道 | 去哪 |
|---|---|
| 启动怎么决策、签名怎么验、journal 怎么记 | [docs/design/JOURNAL.md](docs/design/JOURNAL.md)、[docs/design/OWNERSHIP.md](docs/design/OWNERSHIP.md) |
| 哪条路径验过、实测数字 | [docs/test/MEASUREMENTS.md](docs/test/MEASUREMENTS.md) |
| 三个仓库在哪、哪些代码是跨仓镜像 | [docs/design/ARCHITECTURE.md](docs/design/ARCHITECTURE.md) |
| 引脚、串口、启动模式的实测事实 | [docs/design/HARDWARE-FACTS.md](docs/design/HARDWARE-FACTS.md) |
| 需求清单和测试矩阵 | [docs/STATUS.md](docs/STATUS.md)、[docs/STATUS.md](docs/STATUS.md) |

## 1. 项目定位

- 目标芯片：STM32H743IIKx（Cortex-M7，2 MB Flash / 1 MB RAM）
- 本仓库是 **Bootloader**，和 Application 分开的工程（app 侧是 Arduino core，见 ARCHITECTURE.md）
- 构建：STM32CubeIDE 经典 Managed Build（`.cproject` + `Debug/`）。仓库里那份 `CMakeLists.txt` 是 CubeMX 生成的，**当前构建不用它**

## 2. Flash 分区

`Core/Inc/usbd_cdc_flash.h`：

```c
#define ADDRESS_VECTOR        0x20000                              // 128K 偏移
#define IAP_APP_ADDRESS       (FLASH_BASE_ADDR | ADDRESS_VECTOR)   // = 0x08020000
#define RESERVED_TAIL_SECTORS 1                                    // 尾部预留给 bootloader 状态
#define IAP_STATE_SECTOR_ADDR ADDR_FLASH_SECTOR_7_BANK2            // = 0x081E0000
#define IAP_APP_MAX_SIZE      (IAP_STATE_SECTOR_ADDR - IAP_APP_ADDRESS)
```

| 区 | 地址 | 用途 |
|---|---|---|
| bootloader | `0x08000000`–`0x0801FFFF`（sector 0，128K） | 本工程 |
| application | `0x08020000` 起，上限 `0x081E0000` | 1,835,008 B |
| bootloader 状态 | `0x081E0000`（bank2 sector 7，128K） | metadata + 事件 journal |

⚠️ `RESERVED_TAIL_SECTORS` 这个常量被当成两个意思用，改它会静默弄坏 reclaim。**当前值 1 是对的，别动** —— 完整分析见 [docs/work/ISSUES.md](docs/work/ISSUES.md) 的 C1。

## 3. 编译产物

**必须装进单个 128K 扇区的前 120K**（122,880 B —— 尾部 8K 给 owner 记录）。这是需求 **E3**，构建时的尺寸门禁。

**当前大小和余量在 `docs/test/MEASUREMENTS.md`**（唯一出处）。⚠️ 别在这里拄一份数字 —— 08-17 那个值就在这里挂到了 08-22。

> 本节数字会随每次构建变，**别把它当承诺**。要当前值就自己看 `Debug/` 下那个 `.bin` 的大小。

## 4. 功能模块清单

| 文件 | 功能 |
|---|---|
| `Core/Src/main.c` | 时钟/MPU/外设初始化，主循环 `IAP_task()` + `MX_LWIP_Process()` |
| `IAPServer/IAP_server.c` | 命令状态机 + 启动决策（`server_decide()`）+ 交权（`server_jump_to_app()`） |
| `IAPServer/bootloader_state.c` | flash 上的 metadata 与事件 journal，含 reclaim |
| `IAPServer/fw_verify.c` | ECDSA P-256 验签（micro-ecc） |
| `IAPServer/iap_auth.c` | 挑战应答认证；nonce 计数器在 RTC 备份寄存器 `DR1`，VBAT 见证在 `DR3` |
| `IAPServer/iap_keyderive.c` | 每设备密钥 = `HMAC-SHA256(固定密码, UID)` |
| `IAPServer/sha256.c` | SHA-256 / HMAC-SHA-256 |
| `IAPServer/IAP_boot_handoff.c` | **SRAM4 里的交接记录**，app 用它请求进上传模式 |
| `IAPServer/tcp_server.c` | lwIP raw-TCP，端口 56865，固件传输通道 |
| `IAPServer/udp_server.c` | lwIP UDP 设备发现，含全设备发现限流 |
| `Core/Src/usbd_cdc_flash.c` | flash 擦除/编程，含扇区地址表 |
| `Core/Src/fmc.c` | 外部 SDRAM 初始化 + 上电时序 + 自检。**SDRAM staging 的基础** |
| `USB_DEVICE/*` | USB CDC 通道，含 1200bps touch 触发 |
| `Core/Src/relay.c` | 上电时 6 路继电器自检时序 |
| `Core/Src/crc.c` | 硬件 CRC32 |
| `Core/Src/rtc.c` | RTC 初始化。⚠️ **它不再存"为什么进 bootloader"** —— 那个搬到 SRAM4 了 |
| `Core/Src/md5.c` | ⚠️ **死代码**：`IAP_server.c:12` 只 `#include "md5.h"`，没有任何调用。校验走 CRC32 + SHA-256 |

## 5. lwIP 配置要点（`LWIP/Target/lwipopts.h`）

- `NO_SYS=1`、`WITH_RTOS=0`（裸机轮询，无 RTOS）
- `LWIP_DHCP=1`
- `LWIP_NETCONN=0`、`LWIP_SOCKET=0`（只用 raw API）
- `CHECKSUM_BY_HARDWARE=1`

## 6. 构建配置

只保留一个 Debug 配置（`-O0`）；Debug 图标和"直接烧录"图标都用它的产物，不再需要 Release。

---

## 2026-08-17 这次同步改了什么

原文四处已经不成立，照着它排查会从一开始就找错方向：

| 原文 | 实际 |
|---|---|
| `.bin` = 140,100 字节（超 128K 预算） | 远比那个小，当前值见 `docs/test/MEASUREMENTS.md` |
| "CRC32 校验后复位跳转 App" | CRC32 **+ SHA-256 + ECDSA 验签**，且每次启动都重验 |
| 停留依据是 "RTC 备份寄存器 magic flag" | 现在是 **SRAM4 的 `boot_handoff_t`**；RTC 备份寄存器改用于 nonce 计数器和 VBAT 见证 |
| `IAP_CDC_reboot_trigger()` 当前未被调用 | 1200bps touch 路径**已实测通过** |

一处经核实**仍然成立**：`md5.c` 确实还在、确实没人调用。

**同时把"行为怎么运作"整节删了**，改成上面那张指路表 —— 那些内容在 `docs/` 下有唯一出处，这里再写一遍就是又一份会漂的副本。
