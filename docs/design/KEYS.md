# IAP 的两个秘密 —— `IAPServer/keys/`

本文件描述 bootloader 仓库里的一个目录。产品级的三仓布局、跨仓镜像清单和 RTC
备份寄存器分配表在 `$PROD/docs/design/ARCHITECTURE.md`。

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
