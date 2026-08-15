# 三仓布局

这个产品是**三个独立的代码库**，没有共享构建系统。一个功能通常要同时改动其中两到三个。

| | 路径 | 内容 |
|---|---|---|
| **App 侧** | `C:\Users\<用户>\AppData\Local\Arduino15\packages\OpenPLC_Alpha\hardware\stm32\0.1.3-pre` | 定制板卡包。编译用户应用；同时拥有 `cores/arduino/main.cpp`、变体头文件 `variants/STM32H7xx/H743/variant_PLC_H743.h`、引脚与外设映射、`libraries/OpenPLC_IAP`、`libraries/OpenPLC_Net`、`tools/discovery/` |
| **Bootloader 侧** | `E:\WorkSpace\Schaeffer-AG\open_plc_cube_ide` | STM32CubeIDE 工程（`IAPServer/`、`Core/`、`LWIP/`） |
| **PC 工具侧** | `E:\WorkSpace\Schaeffer-AG\IAPTranfer_Tool` | Go，产出 `IAPTool.exe` 和 `TestTool.exe` |

⚠️ **Arduino 板卡包是要分发给其他工程师的**，所以 core 层的改动是共享基础设施，不是本地小修小补。

## 其他位置

| | 路径 |
|---|---|
| **参考示例** | `E:\WorkSpace\Schaeffer-AG\ref\Hello_World_OpenPLC` —— 发明新写法之前先看这里是怎么做的 |
| **硬件文档** | `E:\WorkSpace\Schaeffer-AG\Hardware` —— 概览 `.txt` + `Production/UpperDeck/Schematics/OpenPLC_UpperDeck_R3.pdf` |
| **测试 sketch** | `E:\WorkSpace\Schaeffer-AG\Hardware\TestCase\SerialPort` |

**文档打架时以 KiCad 原理图为准**（确实打过架，见 [HARDWARE-FACTS.md](HARDWARE-FACTS.md)）。

## 跨仓镜像的代码

因为没有共享构建，下面这些东西在多个仓库里各有一份拷贝，**只能靠注释交叉引用约束，机制上无法强制同步**。改一处必须改另一处，否则会静默分叉 —— 不会编译报错，只会在运行时表现成别的症状。

| 内容 | 在哪几份 |
|---|---|
| MAC 从 UID 派生的算法 | bootloader `LWIP/Target/ethernetif.c`（USER CODE MACADDRESS 块）、core `libraries/OpenPLC_Net/src/ethernetif.c` |
| 发现回复限流 `discovery_reply_allowed()` | bootloader `IAPServer/udp_server.c`、core `libraries/OpenPLC_IAP/src/udp_server.c` |
| 身份字符串格式 `name_uid_role_version` | bootloader `IAPServer/IAP_server.c` 的 `iap_identity_string()`、core `libraries/OpenPLC_IAP/src/udp_server.c`、Go 侧解析 |
| SRAM4 交接记录 `boot_handoff_t` | bootloader `IAPServer/IAP_boot_handoff.{c,h}`、core `cores/arduino/stm32/IAP_boot_handoff.{c,h}` |
| 上传锁的文件名和过期时间 | `IAPTranfer_Tool/uploadlock.go`、core `tools/discovery/network_discovery.go` |
| 设备密钥派生 | 三边的 `iap_keyderive.c` / `.go` |

## IAP 固定密码：单一来源

IAP 的 HMAC 占位密码曾经在三处硬编码、逐字节相同，极易失同步。

现在的单一事实源是 **`open_plc_cube_ide/IAPServer/iap_fixed_password.txt`**。

改密码的流程：编辑该文件 → 在同目录跑 `generate_fixed_password.sh`（或 `.ps1`）→ 它会重新生成：

- `open_plc_cube_ide/IAPServer/iap_fixed_password_generated.h`
- Arduino core 的 `libraries/OpenPLC_IAP/src/iap_fixed_password_generated.h`
- `IAPTranfer_Tool/iapcrypto/fixed_password_generated.go`

这三个生成文件**必须在各自的仓库里分别提交**（仓库之间没有共享构建，没法在构建时从同一来源生成）。密码每次变更都要重新生成并三边重新提交。

> 等 [DEFERRED-DESIGNS.md](DEFERRED-DESIGNS.md) 里的产线单板密钥方案落地，整套机制就不需要了。
