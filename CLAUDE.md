# 开工入口 —— bootloader（STM32CubeIDE 工程）

**这份文件是给 AI 会话看的。** 它只回答三件事：这个仓库是什么、源码在哪、改它有哪些硬规矩。

> **产品文档在 `OpenPLC_Docs`**（`$PROD`）—— 全部文档和待决的问题，入口它的 `README.md`（本机位置见 `DOCS_REPO`）。
>
> 东西产出来该往哪放，读它的 `WHERE-THINGS-LIVE.md`。
>
> 这个仓库 2026-08-24 之前是「文档主仓」；**2026-09-16 起它一份文档都不留**，只剩这份入口和面向客户的 `RELEASE-NOTES.md`。

## 一、这个仓库是什么

STM32H743IIKx（Cortex-M7，2 MB Flash / 1 MB RAM）的 **bootloader**。带签名校验的 IAP：USB CDC / 以太网 / BOOT0 / app 请求四条入口，镜像先在外部 SDRAM 暂存并验签，**通过了才擦 app 区**。

构建是 STM32CubeIDE 的经典 Managed Build（`.cproject` + `Debug/`）。

## 二、源码在哪

| 目录 / 文件 | 装什么 |
|---|---|
| `Core/Inc` `Core/Src` | CubeMX 生成的外设初始化，**加几个手写的**：`fmc.c`（外部 SDRAM 上电时序 + 自检）、`relay.c`（继电器驱动，只有工装和 `TestCase/RELAY` 用；开机不动继电器，决策 71）、`usbd_cdc_flash.c`（flash 擦写 + 扇区地址表）、`crc.c`、`rtc.c`、`usart.c` |
| `Core/Inc/IAP_config.h` | **唯一的编译期配置点。** 固件版本号（必须和 core 的 `boards.txt` 一致，**没有机制强制**，靠用例 P1 查）、服务端口、`UDP_SERVER_NAME`（bootloader 报 `BOOTLD`、app 报 `CUSAPP`，工具靠这一个字段区分两者）、SDRAM 暂存区基址和大小（**故意不在链接脚本的 MEMORY 块里**）、接收缓冲大小、`IAP_Method` 枚举 |
| `Core/Startup/` | 复位向量 |
| `IAPServer/` | **本工程的产品代码，CubeMX 不碰这里。** 命令状态机与启动决策（`IAP_server.c`）、flash 上的 metadata 与校准值区（`bootloader_state.c`）、ECDSA 验签（`fw_verify.c` + `uecc/` 里 vendored 的 micro-ecc）、签名挑战认证（`iap_auth.c`）、证书验签（`iap_cert.c`）、本机 machine ID（`iap_keyderive.c`，**只剩这一件事，不再派生任何密钥**）、SHA-256（`sha256.c`）、owner 记录区（`owner_slot.c`）、SRAM4 交接记录（`IAP_boot_handoff.c`）、lwIP raw TCP/UDP（`tcp_server.c` / `udp_server.c`）、`keys/`（见 `$PROD/docs/modules/M2-ownership.md`）。另有 `SECURITY.md`，配图版是 `$PROD/docs/modules/M2-ownership.md`。<br>⚠️ **找不到合适的 CubeMX USER CODE 块时，代码挪到这里，不要改生成区** |
| `LWIP/` | `App/lwip.c` + `Target/{ethernetif.c,lwipopts.h}`。配置要点见 `$PROD/docs/build/BOOTLOADER-PROJECT-LAYOUT.md` |
| `USB_DEVICE/` | CDC 通道，含 1200 bps touch 触发 |
| `TestCase/` | 板级测试入口，**一个端口一个子目录**：`ADC/` `CAN/` `DAC/` `DIN/` `KNX/` `PWM/` `RELAY/` `RS232/` `RS485/` `SD/` `SDRAM/`，每个一份 `<x>_test.c/.h`。<br>⚠️ **两种入口形状并存**：老的是 `<X>_Test_Run()`，自己死循环、不返回；Digital In / 继电器 / 模拟输入 / 模拟输出 / 板载温度五项改成了 `<X>_Test_Init()` + `<X>_Test_Tick()` 的非阻塞对，由 `common/bringup_test.c` 的 `BringUp_Test_Run()` 统一调度，**一次烧录五项同时跑**（为什么能同时跑、共享了什么，写在 `common/bringup_test.h` 的头注释里）。<br>`common/` 装公用件：`testcase_hal_guard.h`、`bringup_test.c`、vendored FatFs、以及 ⚠️ **工程里唯一自带 HAL 副本的地方**（ADC / **DAC** / SD / SDMMC / FDCAN，靠那个守卫头与工程的 `stm32h7xx_hal_conf.h` 隔离）。它不是产品路径，改 HAL 行为之前先读守卫头。<br>⚠️ `common/` 必须在 include path 上（`.cproject` 里那一条 `../TestCase/common`；工程只有一个 build config，叫 `Debug`）—— `stm32h7xx_hal_conf.h` 找那些副本头文件走的是 include path，不是相对路径 |
| `Drivers/` `Middlewares/` | ST HAL + CMSIS + USB device library，CubeMX 拉进来的，不动 |
| `STM32H743IIKX_FLASH.ld` | bootloader 的链接脚本。⚠️ **FLASH LENGTH 是 120K 不是 128K。** 规矩和后果在 `$PROD/docs/build/CUBEMX-RULES.md` |
| `STM32H743IIKX_FLASH_PORTTOOL.ld` | **工装镜像专用**，FLASH 拿整片 2048K（ST-Link 整片烧，不走 IAP，没有 app 和 owner 记录要避让）。⚠️ **用它编出来的东西不能当 bootloader 烧。** 哪次构建用哪份由 `PLC_LD_SCRIPT` 决定 —— 见 `$PROD/docs/build/CUBEMX-RULES.md` |
| `*.ioc` `.mxproject` | CubeMX 工程定义。**它才是外设配置的单一事实源** |
| `cmake/` `CMakeLists.txt` | CubeMX 生成的 CMake 工程，⚠️ **当前构建不用它**，而且它的 GLOB 不覆盖 `TestCase/` 和 `IAPServer/` |
| `Debug/` | gitignored 构建产物：`.elf` `.bin` `.hex` `.map` `.list` |

## 三、不开 CubeIDE 也能挡住低级错误

用 core 包里的 `arm-none-eabi-gcc` 做单文件语法检查：

```bash
arm-none-eabi-gcc -fsyntax-only -mcpu=cortex-m7 -mthumb \
  -DSTM32H743xx -DUSE_HAL_DRIVER <本工程的 -I 列表> <改过的那个 .c>
```

完整的 `-I` 列表和 core 包里 gcc 的位置见 `$PROD/docs/build/BUILD-AND-TEST.md`。这是**不打开 CubeIDE 就能检查一处改动的唯一办法**。

## 四、改这个工程的硬规矩

1. **CubeMX 生成区不能手工改**，重新生成后有两项必查 —— `$PROD/docs/build/CUBEMX-RULES.md`
2. **`docs/` 下的笔记是本仓库事实的唯一出处。** 发现和现状不符就地改掉，不要另起一份 —— 约定见 `$PROD/docs/repo/CONVENTIONS.md`
3. ⚠️ **`TestCase/` 里的代码是已经测试通过的，写任何测试用例都必须先参考它。**（用户 2026-09-11：「testcase 是测试通过的代码，你写测试用例都要必须参考」）做一个端口的新用例、或者怀疑工装某个端口有问题时，**先读 `TestCase/<PORT>/<port>_test.c`** —— 那份在真板子上跑通过。两边行为不一致时，**差异本身就是线索**：2026-09-11 的 RS485 overrun 就是这么找到的（独立测试的紧循环不丢字节，工装的超循环丢，根因是 USART FIFO 没开）。
4. **测试脚本不放这个仓库。** 它们在 `$TOOL/TestCase/tools/` 或 `host/<主题>/`，判据在 `$PROD/docs/engineering/HOW-TO-RUN-TESTS.md`。这个仓库**没有 `tools/` 目录**，也不该有

## 五、这个仓库的四份入口文件

| 文件 | 装什么 |
|---|---|
| [README.md](README.md) | 英文，对外：烧 bootloader、Arduino 侧怎么装 |
| [RELEASE-NOTES.md](RELEASE-NOTES.md) | 英文，对外：升级规则、known issues、未验证项、发版检查单。**升级风险只靠它兜着** |
| `$PROD/docs/build/BOOTLOADER-PROJECT-LAYOUT.md` | 工程结构：flash 分区、尺寸预算、模块清单、lwIP 配置、构建配置 |
