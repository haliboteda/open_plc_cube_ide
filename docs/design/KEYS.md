# IAP 的密钥 —— `IAPServer/keys/`

本文件描述 bootloader 仓库里的一个目录。产品级的三仓布局、跨仓镜像清单和 RTC
备份寄存器分配表在 `$PROD/docs/design/ARCHITECTURE.md`。

**板子上没有任何共享秘密。** 会话认证走证书（`IAPServer/iap_cert.h`）：板子发 nonce，工具用叶子私钥签，板子用证书里的叶公钥验，而那张证书由 owner 槽里的根签发。编进固件的只有一把根公钥，公开无妨。

| 文件 | 作用 | 提交？ |
|---|---|---|
| `fw_pubkey.inc` | 固件签名**公钥**，即出厂内置根。决定**哪个镜像可以被执行** | 是（本来就公开） |
| `fw_signing_key.TEST_ONLY.pem` | 占位签名私钥 | 是 —— 因此等同公开 |
| `fw_signing_key.pem` | 真实签名私钥 | 否（gitignore） |
| `*.pem.certserial` | 该私钥的证书发号计数器 | 是（不是秘密） |
| `rotate_keys.sh` | 换签名密钥的唯一入口 | 是 |

**没有生成器，也没有生成文件。** 公钥被 `#include` 成 C 数组（`fw_pubkey.c`）—— 改文件、重编译，机制就这些。

换密钥跑 `./rotate_keys.sh`（先 `--dry-run` 看一眼）。细节看 `$BOOT\IAPServer\keys\README.md`，那份是准的。

两个坑：

- ⚠️ **换完必须用 ST-Link 或 DFU 重烧 bootloader** —— 公钥是编译进去的，而 IAP 只写 app 区，永远更新不了持有根的 bootloader。没重烧的板子还认旧根，直接不认新签名。
- ⚠️ **已认领的板子不跟着这把密钥走** —— 它认自己 flash 里 owner 记录指定的根。要换这种板子的主人用 `IAPTool setowner`，不是轮换这里。见 [OWNERSHIP.md](OWNERSHIP.md)。
