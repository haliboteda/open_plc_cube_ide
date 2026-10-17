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
| `Core/Inc` `Core/Src` | CubeMX 生成的外设初始化，**加几个手写的**：`fmc.c`（外部 SDRAM 上电时序 + 自检）、`relay.c`（继电器驱动，只有工装用；开机不动继电器，决策 71）、`usbd_cdc_flash.c`（flash 擦写 + 扇区地址表）、`crc.c`、`rtc.c`、`usart.c` |
| `Core/Inc/IAP_config.h` | **唯一的编译期配置点。** 固件版本号（必须和 core 的 `boards.txt` 一致，**没有机制强制**，靠用例 P1 查）、服务端口、`UDP_SERVER_NAME`（bootloader 报 `BOOTLD`、app 报 `CUSAPP`，工具靠这一个字段区分两者）、SDRAM 暂存区基址和大小（**故意不在链接脚本的 MEMORY 块里**）、接收缓冲大小、`IAP_Method` 枚举 |
| `Core/Startup/` | 复位向量 |
| `IAPServer/` | **本工程的产品代码，CubeMX 不碰这里。** 命令状态机与启动决策（`IAP_server.c`）、flash 上的 metadata 与校准值区（`bootloader_state.c`）、ECDSA 验签（`fw_verify.c` + `uecc/` 里 vendored 的 micro-ecc）、签名挑战认证（`iap_auth.c`）、证书验签（`iap_cert.c`）、本机 machine ID（`iap_keyderive.c`，**只剩这一件事，不再派生任何密钥**）、SHA-256（`sha256.c`）、根区（`owner_slot.c`，在扇区 15）、回收时的备份 SRAM 暂存（`bkp_stash.c`）、SRAM4 交接记录（`IAP_boot_handoff.c`）、lwIP raw TCP/UDP（`tcp_server.c` / `udp_server.c`）。另有 `SECURITY.md`，配图版是 `$PROD/docs/modules/M2-ownership.md`。<br>⚠️ **找不到合适的 CubeMX USER CODE 块时，代码挪到这里，不要改生成区** |
| `LWIP/` | `App/lwip.c` + `Target/{ethernetif.c,lwipopts.h}`。配置要点见 `$PROD/docs/build/BOOTLOADER-PROJECT-LAYOUT.md` |
| `USB_DEVICE/` | CDC 通道，含 1200 bps touch 触发 |
| `TestCase/` | **工装固件**（`porttool/`，`PORTTOOL_ENABLE=1` 时编进去）和它调用的测量函数：`ETH/` `KNX/` `SD/` `SDRAM/` 各一份 `<x>_test.c/.h`，全部是调一次就返回的函数。原来一次烧一个、不返回的旧端口测试已删（决策 87）。<br>`common/` 装公用件：各端口的硬件层 `port_*.c`、`testcase_hal_guard.h`、vendored FatFs、以及 ⚠️ **工程里唯一自带 HAL 副本的地方**（ADC / **DAC** / SD / SDMMC / FDCAN，靠那个守卫头与工程的 `stm32h7xx_hal_conf.h` 隔离）。它不是产品路径，改 HAL 行为之前先读守卫头。<br>⚠️ `common/` 必须在 include path 上（`.cproject` 里那一条 `../TestCase/common`；工程只有一个 build config，叫 `Debug`）—— `stm32h7xx_hal_conf.h` 找那些副本头文件走的是 include path，不是相对路径 |
| `Drivers/` `Middlewares/` | ST HAL + CMSIS + USB device library，CubeMX 拉进来的，不动 |
| `STM32H743IIKX_FLASH.ld` | bootloader 的链接脚本。FLASH LENGTH 是整个扇区 0 的 128K（根区在扇区 15）。规矩在 `$PROD/docs/build/CUBEMX-RULES.md` |
| `STM32H743IIKX_FLASH_PORTTOOL.ld` | **工装镜像专用**，FLASH 拿整片 2048K（ST-Link 整片烧，不走 IAP，没有 app 和 owner 记录要避让）。⚠️ **用它编出来的东西不能当 bootloader 烧。** 哪次构建用哪份由 `PLC_LD_SCRIPT` 决定 —— 见 `$PROD/docs/build/CUBEMX-RULES.md` |
| `*.ioc` `.mxproject` | CubeMX 工程定义。**它才是外设配置的单一事实源** |
| `cmake/` `CMakeLists.txt` | 顶层 `CMakeLists.txt` 由 `CMakeLists_template.txt` 填出来（文件头这么写；是哪个工具填的没核实）；`cmake/stm32cubemx/` 是 CubeMX 生成的（文件头这么写）。`.ioc` 的工具链是 STM32CubeIDE，CubeMX 现在不再生成它们。⚠️ **当前构建不用它们**，GLOB 也不覆盖 `TestCase/` 和 `IAPServer/`；顶层那份写死 `arm-none-eabi-gcc`，不能把 `tests/` 挂进去 |
| `tests/` | **主机测试**：`IAPServer/` 的真源码 + 桩，用本机 gcc 编，CTest 跑；外加 P16 / P17 两项源码检查。独立的 CMake 工程，**不在 `.cproject` 的编译范围里**（⚠️ 所以不能放进 `TestCase/`，那里会被编进固件）。怎么跑见第六节 |
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
3. ⚠️ **`TestCase/` 里的代码是已经测试通过的，写任何测试用例都必须先参考它。**（用户 2026-09-11：「testcase 是测试通过的代码，你写测试用例都要必须参考」）做一个端口的新用例、或者怀疑工装某个端口有问题时，**先读 `TestCase/common/port_<port>.c` 和 `TestCase/porttool/porttool_<port>.c`** —— 端口代码只剩工装这一套（决策 87）。原来一次烧一个的旧端口测试已删，要对照时去 git 历史里找。
4. **这个仓库只放测 bootloader 自己的测试**（`tests/`，决策 78）。要用到别的仓、或者要上真板子的测试在 `$TEST`（`OpenPLC_Test`）。判据在 `$PROD/docs/engineering/HOW-TO-RUN-TESTS.md`

## 五、这个仓库的四份入口文件

| 文件 | 装什么 |
|---|---|
| [README.md](README.md) | 英文，对外：烧 bootloader、Arduino 侧怎么装 |
| [RELEASE-NOTES.md](RELEASE-NOTES.md) | 英文，对外：升级规则、known issues、未验证项、发版检查单。**升级风险只靠它兜着** |
| `$PROD/docs/build/BOOTLOADER-PROJECT-LAYOUT.md` | 工程结构：flash 分区、尺寸预算、模块清单、lwIP 配置、构建配置 |

## 六、主机测试怎么跑

```
cd tests
cmake --preset local          # 第一次；local 预设在 gitignored 的 CMakeUserPresets.json 里，写法见 tests/README.md
cmake --build --preset local
ctest --preset local          # 35 项：T1-16、owner 区 7 组、T2-34 的 24 个断电场景、P16、P17、T1-36
```

只要 gcc、cmake、ninja，不读任何本机配置文件（决策 78）。本机这三样在 `D:\Soft\mingw64\bin`，不在 PATH 上，由 `CMakeUserPresets.json` 指过去。

