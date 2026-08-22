# 三仓布局

这个产品是**三个独立的代码库**，没有共享构建系统。一个功能通常要同时改动其中两到三个。

## 路径变量

全套文档用下面这些变量指代路径。**换机器只改这一张表**，别的地方不写绝对路径。

> **要 clone 的地址、要装的工具、要配的东西，在 [../../CLAUDE.md](../../CLAUDE.md)。** 那是新机器的入口，这里不抄第二份 —— 下面这张表记的是**变量含义和本机当前值**，不是安装步骤。

| 变量 | 本机当前值（2026-08-16） |
|---|---|
| `$BOOT` | `E:\WorkSpace\Schaeffer-AG\open_plc_cube_ide` |
| `$CORE_LIVE` | `C:\Users\WhoamIamwhO\AppData\Local\Arduino15\packages\OpenPLC_Alpha\hardware\stm32\0.1.3-pre` |
| `$CORE_REPO` | `E:\WorkSpace\Schaeffer-AG\open_plc_arduino` |
| `$TOOL` | `E:\WorkSpace\Schaeffer-AG\IAPTranfer_Tool` |
| `$HW` | `E:\WorkSpace\Schaeffer-AG\Hardware` |
| `$REF` | `E:\WorkSpace\Schaeffer-AG\ref\Hello_World_OpenPLC` |
| `$IDE` | `D:\Soft\arduino-2` |
| `$A15` | `C:\Users\WhoamIamwhO\AppData\Local\Arduino15` |
| `$PKGIDX` | `E:\WorkSpace\Schaeffer-AG\package_index_json` |

⚠️ **这张表是给读文档的人看的，脚本不读它。** 脚本只认 `$TOOL/TestTool/config/machine.ps1` 和 `machine.py`（都 gitignored，都由 `tools/init_machine.py` 探测生成）。两处的值应当一致，但**没有任何机制强制**——selfcheck 的 **ENV** 一步会把脚本实际解析到的路径全部打出来，对不上时以它为准。

`$CORE_LIVE` 末尾的 `0.1.3-pre` 是板卡包版本号，**发版会变**；`$A15\packages\OpenPLC_Alpha\tools\STM32Tools\0.1.2` 里的 `0.1.2` 是随包分发的 IAPTool 版本，两个号互相独立。

## 三个仓库

| | 路径 | 内容 |
|---|---|---|
| **App 侧** | `$CORE_LIVE`（干活的地方）<br>`$CORE_REPO`（git 仓库） | 定制板卡包。编译用户应用；同时拥有 `cores/arduino/main.cpp`、变体头文件 `variants/STM32H7xx/H743/variant_PLC_H743.h`、引脚与外设映射、`libraries/OpenPLC_IAP`、`libraries/OpenPLC_Net`、`tools/discovery/` |
| **Bootloader 侧** | `$BOOT` | STM32CubeIDE 工程（`IAPServer/`、`Core/`、`LWIP/`） |
| **PC 工具侧** | `$TOOL` | Go，产出 `IAPTool.exe` 和 `TestTool.exe` |

⚠️ **Arduino 板卡包是要分发给其他工程师的**，所以 core 层的改动是共享基础设施，不是本地小修小补。

### App 侧有两份，方向是单向的

`$CORE_LIVE` 是 **Arduino IDE 真正加载的那份**（插件安装目录）。改动先落在这里，在这里编译、烧板、验证。

**只有验证通过的代码才拷进 `$CORE_REPO` 提交。**

- 反过来做没有意义 —— IDE 根本不看 `$CORE_REPO`，改那边不生效。
- ⚠️ **`$CORE_LIVE` 不在版本控制下。** 验证通过后忘了同步，那段代码就只存在于这一台机器上，重装一次 IDE 就没了。
- 同步目前**手动**做，是否自动化未定（2026-08-16）。

核对两边是否已同步：

```powershell
$live = "<$CORE_LIVE>"; $repo = "<$CORE_REPO>"
$skip = '^(installed\.json|tools\\discovery\\bin\\)|~$'
Get-ChildItem $live -Recurse -File | ForEach-Object {
  $rel = $_.FullName.Substring($live.Length + 1)
  if ($rel -match $skip) { return }
  $r = Join-Path $repo $rel
  if (-not (Test-Path $r)) { "ONLY-LIVE  $rel" }
  elseif ((Get-FileHash $_.FullName).Hash -ne (Get-FileHash $r).Hash) { "DIFF       $rel" }
}
```

排除的三类是**故意的**：`installed.json` 是 IDE 自己的安装元数据；`tools/discovery/bin/` 是编译产物（Go 每次构建出的二进制都不一样，比源码就够）；`*~` 是编辑器备份。

> 2026-08-16 跑过一次，除这三类外零差异。

## 其他位置

| | 路径 |
|---|---|
| **参考示例** | `$REF` —— 发明新写法之前先看这里是怎么做的 |
| **硬件文档** | `$HW` —— 概览 `.txt` + `Production/UpperDeck/Schematics/OpenPLC_UpperDeck_R3.pdf` |
| **测试与验收总入口** | `$TOOL\TestTool` —— 用例、主机侧单元测试、板上 sketch、自动化脚本、验收单全在这里。本机路径只写在 `TestTool\config\machine.ps1` |
| **板卡包索引** | `$PKGIDX` —— Arduino IDE 拉板卡包用的 package index json |

**文档打架时以 KiCad 原理图为准**（确实打过架，见 [HARDWARE-FACTS.md](HARDWARE-FACTS.md)）。

## 跨仓镜像的代码

因为没有共享构建，下面这些东西在多个仓库里各有一份拷贝，**只能靠注释交叉引用约束，机制上无法强制同步**。改一处必须改另一处，否则会静默分叉 —— 不会编译报错，只会在运行时表现成别的症状。

下表的 "core" 指 `$CORE_LIVE` 和 `$CORE_REPO` 两份（先改前者，验证过再同步到后者）。

| 内容 | 在哪几份 |
|---|---|
| MAC 从 UID 派生的算法 | bootloader `LWIP/Target/ethernetif.c`（USER CODE MACADDRESS 块）、core `libraries/OpenPLC_Net/src/ethernetif.c` |
| 发现回复限流 `discovery_reply_allowed()` | bootloader `IAPServer/udp_server.c`、core `libraries/OpenPLC_IAP/src/udp_server.c` |
| 身份字符串格式 `name_uid_role_version` | bootloader `IAPServer/IAP_server.c` 的 `iap_identity_string()`、core `libraries/OpenPLC_IAP/src/udp_server.c`、Go 侧解析 |
| SRAM4 交接记录 `boot_handoff_t` | bootloader `IAPServer/IAP_boot_handoff.{c,h}`、core `cores/arduino/stm32/IAP_boot_handoff.{c,h}` |
| 上传锁的文件名和过期时间 | `$TOOL/uploadlock.go`、core `tools/discovery/network_discovery.go` |
| 设备密钥派生 | 三边的 `iap_keyderive.c` / `.go` |
| **RTC 备份寄存器的分配** | 见下表 —— **认领任何一个之前先看这里** |

### RTC 备份寄存器分配表

备份寄存器是**跨 bootloader / app 持久存在**的共享资源，而两个镜像分别编译、没有任何机制阻止它们抢同一个。

| 寄存器 | bootloader | app / core | |
|---|---|---|---|
| DR0 | — | — | 空 |
| DR1 | **nonce 计数器**（`IAPServer/iap_auth.c`） | `RTC_BKP_INDEX`（`cores/arduino/stm32/backup.h:34` 定义，**当前无人写**） | ⚠️ 潜在冲突：谁引入 STM32RTC 库谁就会踩 |
| DR2 | — | **nonce 计数器**（`libraries/OpenPLC_IAP/src/iap_auth.c:21`） | |
| DR3 | **VBAT witness** | — | |
| DR4 | — | `HID_MAGIC_NUMBER_BKP_INDEX` | |
| DR5–DR9 | — | — | 空 |
| DR10 | — | `HID_OLD_MAGIC_NUMBER_BKP_INDEX` | |

> ⚠️ **踩过一次（2026-08-17 定位）**：bootloader 的 VBAT witness 曾经放在 DR2，和 app 的 nonce 计数器**撞车**。
>
> app 侧的注释当时写的理由是"两个镜像不同时运行，所以没有冲突风险" —— **这个推理是错的**。备份寄存器存在的意义就是跨这次交接保存状态，**先后访问同一份持久状态就是冲突**。
>
> 后果是双向的：app 的计数器覆盖 witness，导致 bootloader 每次启动都误报"备份域丢失"（表象）；而 witness 的写入把 app 的计数器重置成固定值，**导致 app 每次经过 bootloader 之后重复发放同一批 nonce 编号**（真正的缺陷 —— 重放保护只剩 tick 在撑）。
>
> 现已把 witness 挪到 DR3。**加新用途时更新这张表。**

## IAP 的两个秘密：`$BOOT\IAPServer\keys\`

IAP 的占位密码曾经在三处硬编码、逐字节相同，极易失同步。现在全部集中在这一个目录：

| 文件 | 作用 | 提交？ |
|---|---|---|
| `iap_fixed_password.txt` | HMAC 占位密码。决定**谁可以发起**升级 | 是（占位值） |
| `fw_pubkey.inc` | 固件签名**公钥**。决定**哪个镜像可以被执行** | 是（本来就公开） |
| `fw_signing_key.TEST_ONLY.pem` | 占位签名私钥 | 是 —— 因此等同公开 |
| `fw_signing_key.pem` | 真实签名私钥 | 否（gitignore） |
| `rotate_keys.sh` | 换这两个秘密的唯一入口 | 是 |

**没有生成器，也没有生成文件。** 密码文件被 `#include` 成 C 字符串字面量（`iap_keyderive.c`），公钥同理（`fw_pubkey.c`）—— 改文件、重编译，机制就这些。Go 侧更省事：`iapcrypto` **运行时读**同名文件，换密码连 IAPTool 都不用重编（所以 `$TOOL` 源码里没有密码副本，副本在随包分发的 exe 旁边）。

换秘密跑 `./rotate_keys.sh`（先 `--dry-run` 看一眼）。细节看 `$BOOT\IAPServer\keys\README.md`，那份是准的。

两个坑：

- ⚠️ 脚本自动找到的 core 是 **`$CORE_LIVE`**（它扫 `$A15\packages\...`），**不是 `$CORE_REPO`**。轮换后仓库里那份密码还是旧的，要跟着上面的 live → repo 流程一起同步提交。
- ⚠️ **换完必须用 ST-Link 或 DFU 重烧 bootloader** —— 密码和公钥是编译进去的，而 IAP 只写 app 区，永远更新不了持有秘密的 bootloader。没重烧的板子还认旧密码，直接不理 IAPTool。

> 等 [DEFERRED-DESIGNS.md](DEFERRED-DESIGNS.md) 里的产线单板密钥方案落地，固定密码这整套就不需要了。
