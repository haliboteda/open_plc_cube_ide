# M1 · owner 槽与信任根 ✅ 六步全部完成（2026-08-18）

**完整生命周期已实测**：认领（G1）→ 换 owner（G2，现任签名）→ 恢复出厂（cleared）→ 重新认领。每一步都有反向用例，包括**夺取攻击**（无签名的高 generation 记录夺不走已认领的板子）。

**覆盖需求：[C10](../STATUS.md)** —— 板子有办法脱离"出厂公开根"，且不需要 ST-Link。

> ⚠️ **完整设计在 [../design/OWNERSHIP.md](../design/OWNERSHIP.md)，不在这里。** 这份只写**落地计划**：分几步、每步验什么、还有哪几个口子没堵。设计推理不重复一遍。

## 是什么

flash 里一片只追加、永不擦除的记录区，记着"这块板现在认哪把公钥当根"。空的时候回落到编译进去的默认根。

## 为什么必须做

出厂板的默认根**私钥必然是公开的** —— 客户要签自己的 sketch，私钥就得在客户机器上，项目开源，所以那把私钥在仓库里。这是产品定位的直接推论，不是妥协。

于是"客户想加固时有没有一条不用 ST-Link 的路"就是产品问题，不是安全洁癖。没有 owner 槽，答案是"没有"。

## 落地已经定死的几件事

这些在 [../design/OWNERSHIP.md](../design/OWNERSHIP.md) 里已经想清楚，**照抄，不要重新讨论**：

| | 决定 |
|---|---|
| 位置 | bootloader 扇区尾部，寄生在已占用但没用满的扇区里。**净增 0 扇区** |
| 大小 | 建议留 **8K** —— `FLASH LENGTH` 128K → 120K，owner 区起点 `0x0801E000`，可存 51 条 |
| 记录格式 | 160 字节 = 5 × 32 字节 flash word，字段表在 OWNERSHIP.md。`format_ver` 从第一版就要有 |
| 告警条件 | **不是"槽空"，是"当前根 == 那把公开根"**。存一份公开根的 SHA-256 比对。自己编译的客户不会被误报 |
| 三个操作 | 认领（TOFU，要按 BOOT0）／换 owner（必须被现任签名）／恢复出厂（追加 cleared 记录，要物理动作） |
| 术语 | 直接借 UEFI Secure Boot 的 Setup Mode / User Mode，读者有现成心智模型 |
| core 要不要改 | **不用。一行都不用。** 认领要求 BOOT0 物理按键，那时 app 根本没在跑 |

## 分步计划

每一步都有可验证的中间状态，不要合并。

| 步 | 做什么 | 验什么 |
|---|---|---|
| 1 | ✅ **2026-08-17 已做**：`FLASH LENGTH` 128K → 120K，注释写清预留区和理由 | 见下 |
| 2 | ✅ **2026-08-17 已做**：`IAPServer/owner_slot.{h,c}`，`_Static_assert` 锁死 160 字节，只读扫描 | 见下 |
| 3 | ✅ **2026-08-18 已做**：`owner_slot_root_is_public()` + 启动告警 | 见下 |
| 4 | ✅ **2026-08-18 已做**：`owner_slot_claim()` + `takeown` 命令，BOOT0 门控 | 见下 |
| 5 | ✅ **2026-08-18 已做**：`owner_slot_set_owner()` + `setowner` 命令，链上逐环验签 | 见下 |
| 6 | ✅ **2026-08-18 已做**：`owner_slot_factory_reset()`，接在 10 秒手势上 | 见下 |

### 三个操作各自靠什么门控（2026-08-18 定）

**不能用同一套** —— 它们的性质不一样：

| 操作 | 门控 | 为什么不能是别的 |
|---|---|---|
| **首次认领**（TOFU） | **只能靠物理动作** | 此时还没有 owner，没有任何签名可验。本质是先到先得 |
| **换 owner** | **现任 owner 的签名，不碰按键** | 签名本身就是授权；远程换 owner 是要支持的场景 |
| **恢复出厂** | **只能靠物理动作** | 现任 owner 可能私钥丢了/人不在了。要求签名的话，客户丢了根就永久变砖 |

所以"按住等待 owner 授权"这个说法要拆开：**需要"等授权"的换 owner 其实不需要按键，需要按键的认领和恢复出厂反而没有授权可等。**

### 动作设计

| 动作 | 触发 | 状态 |
|---|---|---|
| 留在 bootloader | 复位后，**t=1.5s 那个采样点**手按着 | 不变 |
| 恢复出厂 | 同一次按压**继续按到 10 秒**，继电器改快速连响提示，松手执行 | 要写 |
| 首次认领 | 已在 bootloader，IAPTool 下发 `takeown` | 第 4 步 |
| 换 owner | 纯协议，验现任签名 | 第 5 步 |

只新增**一个**手势，而且是现有手势的自然延长，用户不用记第二种操作。

### 手势识别（2026-08-18）

`boot_window_relay()` 现在返回 `boot0_gesture_t`（`NONE` / `UPLOAD` / `FACTORY_RESET`）。

⚠️ **留在 bootloader 的判据一个字没改**：继电器响 3×500ms **纯等待**，然后 `boot0_is_pressed()` **只读一次**，"在 t=1.5s 那一瞬手是不是按着"。轮询只在**那之后**才开始，它**改不了留不留的决定**，只能追加恢复出厂。

实测（两个手势各一次）：

| 手势 | 日志 |
|---|---|
| 按住过 1.5s，10s 前松手 | `UPLOAD Mod ... (BOOT0 held)`，**没有 ARMED** |
| 按住超过 10s | 三下快咔哒 + `Factory reset ARMED - release BOOT0 to run it, or reset the board to cancel` → 松手后 `FACTORY RESET REQUESTED` |

**反馈用继电器咔哒不用 LED**：板上没有通用 LED，而且板子通常在柜子里，看不见 LED —— 启动窗口本来就是靠咔哒声宣告的。

**卡键保护**：按住超过 30 秒当作按键卡死/网短路，报一行然后按上传模式继续。否则板子会在轮询循环里干等，**看起来和 bootloader 死了一模一样**。

⚠️ **恢复出厂现在只识别不执行，而且明说 `not implemented yet`。** 做了手势的人绝不能被留在"我以为已经重置了"的状态里 —— 那比功能不存在更糟。

⚠️ **第 6 步的清除动作必须放在 `server_decide()` 之前。** `owner_slot_report()` 在它里面打日志；清除放在后面的话，**这一次启动的日志会先报旧的所有权、然后才把它清掉** —— 而那恰恰是唯一一次肯定有人在读日志的启动。接缝已经挪到正确位置（[../../Core/Src/main.c](../../Core/Src/main.c) 里那个 `if (gesture == BOOT0_GESTURE_FACTORY_RESET)` 块），第 6 步填进去即可。

⚠️ **复位那一刻不能按住** —— PG9 就是 BOOT0 网，复位瞬间是高电平的话 MCU 从系统存储器启动（ROM DFU），根本到不了我们的固件。所以长按必须是**复位之后才开始按**。banner 现在写的 "while clicking" 就是这个意思。

### 前置：PG9 已改成输入（2026-08-18 完成）

按 10 秒之前必须先做，否则**就是 3V3 对地短路 10 秒**。已做完并用受控实验验证，完整记录在 [../design/HARDWARE-FACTS.md](../design/HARDWARE-FACTS.md) —— 顺带否定了"改 input 会让 RESET 失效"那条挂了很久的怀疑（6 次物理复位全正常，BOOT0 也读得到）。

⚠️ **第 1 步是唯一一个改错了会当场变砖的**（链接器把代码放进预留区）。单独做、单独验、单独提交。

### 第 1 步的实测结果（2026-08-17）

取 **8K**（本文档「还没定的」里的建议值），预留区 `0x0801E000`–`0x0801FFFF`。

| 验的 | 结果 |
|---|---|
| 构建 | 0 errors 0 warnings |
| `.bin` 大小 | **96,944 B**，和改之前**一模一样** —— 预留不产生任何代码 |
| 余量 | 122,880 − 96,944 = **25,936 B（21%）** |
| map 里最高的 flash 地址 | **`0x08017AB0`**，离预留区起点还差 26,960 B |
| 预留区内容（ST-Link 读回） | **全 `0xFF`**，烧写后和实际跑过一轮之后都查过 |
| 板子行为 | app 正常启动、SDRAM 验收 19/19 仍全过；进 bootloader 侧 SDRAM 自检 OK、以太网起来、尺寸检查正常拒绝 |

**为什么 `.bin` 大小一个字节都没变**：预留是把 `MEMORY` 区的上界往下压，不是插入数据。链接器本来就没往那一片放东西，所以产物完全相同 —— 变的是**以后也不可能往那里放**，这正是要的保证。

### 第 2 步的实测结果（2026-08-17）

`IAPServer/owner_slot.{h,c}`，**只读**，一个字节都不写 flash。挂在 `server_decide()` 里 `bootloader_state_init()` 之后，所以**每次启动都打**，不只是停在 bootloader 的那些。

| 场景 | 日志 |
|---|---|
| 空区 | `Owner slot: empty, using the built-in root key` |
| 一条合法记录（generation 7） | `Owner slot: 1 record(s), latest generation 7` |
| `format_ver` 不认识 | `Owner slot: 1 record(s) ignored - wrong format or corrupt` |
| **只要有记录，额外一行** | `** Owner records present but NOT in effect: signature checking is not implemented yet. **` |

镜像 97,580 B（涨 636 B），占 122,880 的 79.4%。

⚠️ **`owner_slot_root()` 现在永远返回编译进去的默认根，即使有记录。** 这是刻意的：验 `prev_sig` 是第 5 步，还不存在。**采信一条没验签的记录，等于任何能写这 8K 的代码都能给自己发信任根 —— 比没有这个功能更糟。** 所以开关等到让它安全的那个检查落地再打开，在那之前大声报出来。

### 第 3 步的实测结果（2026-08-18）

告警：

告警文字就是第 3 步实测记的那两行（见下）。判据是**当前生效的根的 SHA-256 == 公开根的 SHA-256**，常量在 `IAPServer/owner_slot.c` 的 `k_published_root_sha256`。

**两条验收都做了，反向那条才是重点：**

| 场景 | 结果 |
|---|---|
| 出厂板（公开根） | **每次启动都告警** ✅ |
| 临时换一把根密钥重编 | 槽仍然是空的，但**没有告警** ✅ |

第二条证明了告警**不是**挂在"槽空"上。用槽空当判据的话，自己编译换了根的客户会被永远误报 —— [../design/OWNERSHIP.md](../design/OWNERSHIP.md) 花了一整段讲这件事：**一个所有人都学会忽略的告警，等于没有告警。**

⚠️ **指纹是常量，不是构建时从 `fw_public_key` 算出来的。** 那样算的话比较永远成立，客户板子也会被告警 —— 正好是要避免的那个失效模式。

⚠️ **代价是：项目自己轮换默认密钥时，这个常量必须一起更新**，否则出厂板会静默地不再告警。新增用例 **P6**（`tools/check_public_root.py`，selfcheck 会跑它）比对两者，漂了就报错，负向对照验过会响。

### 第 4 步的实测结果（2026-08-18）

`takeown <128hex>` 命令 + `owner_slot_claim()`。跑法：`tools/run-takeown.ps1`。

| 验的 | 结果 |
|---|---|
| **BOOT0 没按** 时 `takeown` | `Refused`，`getpubkey` 返回的密钥一字节没变 ✅ |
| BOOT0 按住时 `takeown` | `OK`，`getpubkey` 立刻返回新密钥 ✅ |
| 复位后 | `Owner slot: claimed at generation 1 - firmware must be signed by that owner`，**公开根告警消失** ✅ |
| flash 里的记录 | `4F 05 01 00 01 00 00 00 00 00 00 00 E9 59 12 FF…` —— type/slots/format_ver/generation/flags 全对 ✅ |

**两个落地决定：**

| 决定 | 理由 |
|---|---|
| **记录体先写、头最后写** | 五个 flash word 中途掉电，绝不能留下一条读起来合法的记录。头最后写，最坏情况是 `type` still `0xFF` —— 被扫描器判为"撕裂写"跳过。反过来会留下一条自称合法、密钥却写了一半的记录 |
| **走链，不是"generation 最大的赢"** | 后者是个洞：已被 G1 认领的板子，任何能追加记录的人写一条更高 generation 的无签名记录就能夺走。**权威必须来自链，不是来自"最后一条"**。首条可以无签名（TOFU），cleared 可以无签名（物理门控，R3），其余必须被现任根签名；**某一环验不过就停下**，不跳过 —— 跳过等于让攻击者作废一环、让后面别人写的记录静默生效 |

### 第 5 步的实测结果（2026-08-18）

`setowner <generation> <newkey_hex> <sig_hex>` + `getowner`（返回当前 generation）。跑法：`tools/run-setowner.ps1`。

**签的是新记录的前 76 字节**：`type | slots | format_ver | generation | flags | 新公钥`（1+1+2+4+4+64）。

⚠️ **generation 在签名覆盖范围内是刻意的** —— 否则一条被截获的记录可以被重放到后面的槽位，把之后的一次交接抹掉。命令里带 generation，板子校验它**恰好等于当前 +1**，这样双方对"签的是哪些字节"逐位一致。

⚠️ **换 owner 不需要按 BOOT0。** 现任签名本身就是授权，远程交接是设计要支持的场景。物理门只管那些**没有签名可验**的操作（首次认领、恢复出厂）。

| 场景 | 结果 |
|---|---|
| A 签名把板子交给 B | `OK`，generation 1→2，`getpubkey` 返回 B ✅ |
| 签名改坏一位 | `Refused`，generation 和根**都没动** ✅ |
| 复位后 | `2 record(s), latest generation 2` + `claimed at generation 2`，无任何 "NOT in effect" ✅ |
| **夺取攻击**：合法 G1 + 无签名 G9 | 扫描器看见 `latest generation 9`，但**链停在 generation 1**，`getpubkey` 仍返回 A ✅ |

**最后一条是这一步真正的判据。** 它证明权威来自链而不是"最后一条记录"——如果按"generation 最大的赢"，任何能追加记录的人写一条更高 generation 的无签名记录就能夺走一块已认领的板子。

**验签在写入之前做，不是之后。** 扫描器下次启动本来也会拒，但那条坏记录会永久占掉 51 个槽位之一，而这些槽位不擦掉 bootloader 就回收不了。拒绝不花任何代价，接受是永久的。

**顺带修了一句过期日志**：`not implemented yet (M1 step 5)` —— 第 5 步做完它就不再成立了。

### 第 6 步的实测结果（2026-08-18）

手势（按住 BOOT0 超过 10 秒 → 三下咔哒 → 松手）接上 `owner_slot_factory_reset()`。板子先被认领到 generation 1，然后做手势：

```
** BOOT0 held - keep holding for 10 s to arm a factory reset, or let go now for upload mode **
** Factory reset ARMED - release BOOT0 to run it, or reset the board to cancel **
** FACTORY RESET DONE at generation 2. Back to the built-in root; the board can be claimed again. **
Owner slot: 2 record(s), latest generation 2 (cleared)
Owner slot: last record is a factory reset - back to the built-in root
** This board trusts the PUBLISHED root key: anyone can sign firmware it will run. **
```

⚠️ **`FACTORY RESET DONE` 打在 `Owner slot:` 之前** —— 因为清除动作放在 `server_decide()` 之前。顺序反了的话，这次启动的日志会先报旧的所有权、然后才把它清掉，而那恰恰是唯一一次肯定有人在读日志的启动。

**完整生命周期**（接着上面继续）：重新认领 → `3 record(s), latest generation 3` + `claimed at generation 3`，全链验证通过、无任何 "NOT in effect"。

### ★ 差点做出一块"恢复出厂 = 永久变砖"的板子

第 6 步暴露了链规则的一个缺陷：**恢复出厂之后再也无法重新认领**。

新的认领记录既不是第一条、也不是 cleared、又没有签名 —— 按原规则直接判为未授权。结果就是：恢复出厂把板子打回公开根，**然后永远卡在那里**，谁也认领不了。**比不做恢复出厂更糟。**

补的第三种允许情形：**紧跟在 cleared 之后的无签名记录 = 回到 TOFU**。同时 `takeown` 的门从"必须没有记录"改成"没有记录**或**最后一条是 cleared"，generation 也从写死的 1 改成接着往下排（否则重新认领会和旧记录撞号）。

> 三种允许无签名的情形，各有各的理由，**不能合并成一条**：
> **首条** —— 还没有 owner，没有签名可验；
> **cleared** —— 要求现任签名的话，丢了私钥的客户永久变砖（R3）；
> **cleared 之后** —— 板子已经回到 TOFU，等价于首条。

### ★★ 验收抓到一个严重缺陷：认领曾经是安全剧场

认领之后，**用旧的公开密钥签的 app 照样启动**。

根因：`fw_verify_signature()` 写死了 `fw_public_key`，**没走 `owner_slot_root()`**。于是认领只改变了"板子报告的根"和告警文字，**没改变它实际拿来验签的密钥**。板子一边打印 `firmware must be signed by that owner`，一边照跑任何用公开密钥签的固件。

**除了这条用例，没有任何东西会发现** —— 其他每个症状看起来都完全正确。

修法：`fw_verify_signature()` 改用 `owner_slot_root()`；同时加了 `fw_verify_signature_with_key()` 供第 5 步验链用。未认领的板子上 `owner_slot_root()` 就是 `fw_public_key`，**行为不变**。

修复后实测：认领状态下，旧 app → `** UPLOAD Mod ... (no valid application)`，**被拒**。

> **教训**：一个"改变信任根"的功能，必须验的是**什么代码能真的跑起来**，不是日志说了什么。C10 的措辞是"板子有办法脱离出厂公开根"，不是"板子有办法宣称自己脱离了"。

### ★ 换密钥时的一个静默陷阱（当场踩到）

把 `fw_pubkey.inc` 换回来之后重编，**编进去的还是旧密钥**。

原因：`Copy-Item`（以及任何普通复制/还原）**保留源文件的时间戳**，还原回来的 `.inc` 时间比 `.o` 旧，make 判定不需要重编。**依赖本身是声明对的**（`.d` 里正确列了 `.inc`），是时间戳骗了 make。

后果很难看：**固件构建正常、启动正常、看起来一切健康，但带着错的信任根。** 这次能发现，只是因为 app 的签名恰好验不过了 —— 如果轮换的两把密钥都是我们自己的，**什么异常都不会有**。

P6 因此还会检查 **`Debug/*.bin` 里到底有没有 `fw_pubkey.inc` 那把密钥**，把这种静默失败变成响的。

### ⚠️ 不能用编程器直接往 owner 区写

**踩过了，板子当场没输出。**

```powershell
STM32_Programmer_CLI -c port=SWD -w record.bin 0x0801E000   # ← 会擦掉 bootloader
```

owner 区在 **bootloader 自己那个扇区**的尾部，而编程器写之前会擦整个扇区。结果是 `0x08000000` 全变 `FF`，板子什么都不打，只能重烧。

正确做法：**把 bootloader 和记录拼成一个镜像一次写进 `0x08000000`** —— 一次擦除，两者都活下来。已做成 `$TOOL/TestCase/tools/inject-owner-record.ps1`：

```powershell
.\inject-owner-record.ps1 -Generation 7   # 一条合法记录
.\inject-owner-record.ps1 -Corrupt        # 格式版本不认识，必须被忽略
.\inject-owner-record.ps1 -Cleared        # 恢复出厂记录
.\inject-owner-record.ps1 -Restore        # 放回干净的 bootloader
```

第 4 步之后 bootloader 自己会追加记录，但这个脚本仍然有用 —— **它能造出固件按设计造不出来的记录**（未来格式版本、签名验不过的记录），那正是负向用例需要的。

⚠️ **`flash-bootloader.ps1` 原来把余量按 131,072 报**，现在会多报 8K 并且掩盖真正的失败点。已改成从链接脚本里读 `LENGTH`，两边不会再各说各话。

## 还没定的（做之前要拍板）

| | 影响 | 现在能不能定 |
|---|---|---|
| 保留 4K / 8K / 16K | — | ✅ **定了 8K**，2026-08-17 已落地。实测余量 21%，够 M2 的证书验证代码再长一截 |
| 认领时要不要绑 UID | 防止把一块板的记录整段搬到另一块板 | 能。建议绑，代价很小 |
| 恢复出厂怎么触发 | — | ✅ **2026-08-18 已定并实现**：同一次按压继续按到 10 秒，三下咔哒后松手执行。前置的 PG9 改输入也做完了 |
| 要不要上 WRP | 唯一能真正禁止 app 写这片区域的手段 | 可以推后，不阻塞 |

## 诚实的上限

> **owner 槽的安全上限 = "这块板子上能不能跑任意代码"。**

出厂状态：**能**（默认根私钥公开 → 谁都能签 sketch → 谁都能写 owner 区）。R1 的 BOOT0 只保护受支持的那条路（为什么，见 [../design/OWNERSHIP.md](../design/OWNERSHIP.md) 的 R1）。

已认领之后：**不能**。从那一刻起才是真保证。

**所以文档里要写死：板子到手第一件事是认领。**

## 顺带会踩到的地雷

**`RESERVED_TAIL_SECTORS` 被当成两个意思用**（`Core/Inc/usbd_cdc_flash.h:65`）。走 bootloader 扇区尾部这条路**恰好绕开它**（根本不加尾部扇区），但地雷还在。详见 [ISSUES.md](ISSUES.md) 的 `ISS-C1`，那里有完整的三处对照表。

⚠️ **重烧 bootloader = 所有权重置** —— owner 记录和 bootloader 同扇区。语义上是对的，但要和"bootloader 与 app 必须捆绑升级"那条风险**写在一起**进发布说明：换 bootloader 的人要同时重传 app **和**重新认领。

## 验收 ✅ 2026-08-18 全部完成

用例是 **OW1 / OW2 / OW3 / P6**，状态和结果在 [../STATUS.md](../STATUS.md) 的 C10，实测数字在 [../test/MEASUREMENTS.md](../test/MEASUREMENTS.md)。

原计划要求的五条，逐条落到了哪：

| 要验的 | 落在哪 |
|---|---|
| 空槽 → 用默认根 → 告警出现 | 第 3 步实测（上面） |
| 认领 → 不告警 → 断电后仍认新根 | **OW1** |
| 伪造一条记录（不带有效签名）→ 被忽略 | **OW2** 的夺取攻击那条 |
| 恢复出厂 → 回落默认根 → 告警恢复 | **OW3** |
| **反向用例**：自己编译换过根的板子，槽空但**不**告警 | 第 3 步实测的第二行 + **P6** |

最后一条最容易漏，而它恰恰是"告警条件为什么不能用槽空"的全部理由。⚠️ **它当时确实差点被漏掉** —— 见第 3 步。

## 已否决

| 路 | 为什么砍 |
|---|---|
| 记录放现有 state 扇区 | `journal_reclaim()` 整扇区擦。搬运再写回的话掉电窗口 = 擦除耗时，丢了就**静默**退回默认根 |
| 新开一个专用扇区 | 白付 128K，还要绕开 `RESERVED_TAIL_SECTORS` 那个陷阱 |
| 撤销状态放 RTC 备份域 | 备份域是三仓共享、无人统一分配的资源，**已经撞过一次车**（DR2，2026-08-17）。见 [../design/ARCHITECTURE.md](../design/ARCHITECTURE.md) 的分配表 |
| 学 Android"追加一把根"（OEM 根 + 用户根并存） | Android 的 OEM 根私钥保密，多信任一把不掉安全性。**我们的默认根私钥是公开的，只能覆盖不能追加** |
| 学 ESP32 的 aggressive revoke | 会永久变砖，理由见 [../design/OWNERSHIP.md](../design/OWNERSHIP.md) 的 R4 |
