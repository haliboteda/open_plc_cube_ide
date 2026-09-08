# 改这个 CubeIDE 工程的硬规矩

只对 bootloader 仓库成立。产品级的协作约定在 `$PROD/docs/CONVENTIONS.md`，
跨项目通用的那些在 `~/.claude/rules/`（源在 AI-Skills 的 `_shared/rules/`）。

## CubeMX 生成区不能手工改

代码必须写在 `/* USER CODE BEGIN *** */` … `/* USER CODE END *** */` 之间，否则从 `.ioc` 重新生成时会被 CubeMX 删掉。

> 真实发生过：一次重新生成删掉了 `main()` 里的 `SystemClock_Config();`，并把 `stm32h7xx_it.c` 的四个 naked fault handler 换回空 `while(1)`。

**USER CODE 块之外的东西，不要自己改 —— 报给用户，由他去改 `.ioc`。** 手工改生成区只是"这次能用"，下次重新生成全废；**只有 `.ioc` 才是单一事实源**。发现需要改动时，说清楚"哪个文件、哪一项、应该是什么值"。

找不到合适的 USER CODE 块，就把代码挪到 CubeMX 不生成的文件（如 `IAPServer/`）。CubeMX 自己会生成的调用（`MX_*_Init()`、`SCB_EnableICache()`）留在原位不要搬。

### 已经通过 `.ioc` 固化的

- `USBD_LPM_ENABLED` —— `.ioc` 里 `USB_DEVICE.USBD_LPM_ENABLED-CDC_FS=0`（2026-08-15）。此前每次重新生成都会变回 `1U`，那会让设备对主机声称支持 USB 2.0 LPM，而 `USBD_LL_Init()` 里 `lpm_enable = DISABLE` 根本没开这个功能 —— 声称了一个自己不会响应的能力。

### 重新生成后仍须复查（不在 USER CODE 块内）

- `.ioc` 里 PG9 的信号类型（见 [../design/HARDWARE-FACTS.md](../design/HARDWARE-FACTS.md)）
- `STM32H743IIKX_FLASH.ld` 的 `FLASH LENGTH` 必须是 **120K，不是 128K** —— 尾部 8K 是 owner 记录区（需求 C10）。⚠️ **看到 128K 不要“改回去”** ：那会让链接器把代码放进那 8K，把已经写在里面的所有权记录盖掉 —— 而那是静默的，板子会惄无声息地退回出厂根。理由见 [../design/OWNERSHIP.md](../design/OWNERSHIP.md)
- **`.cproject` 里链接脚本那个选项的值必须是 `${workspace_loc:/${ProjName}/${PLC_LD_SCRIPT}}`，不是某个写死的 `.ld` 文件名** —— 见下一节

## 两份链接脚本，选哪份由环境变量决定

2026-09-08 加的。**bootloader 和工装镜像的 FLASH 区大小不一样**，因为约束不一样：

| 镜像 | 脚本 | FLASH | 为什么 |
|---|---|---|---|
| bootloader | `STM32H743IIKX_FLASH.ld` | **120K** | 上面 0x08020000 起是 app，尾部 8K 是 owner 记录，都不能被盖 |
| 工装 | `STM32H743IIKX_FLASH_PORTTOOL.ld` | **2048K** | ST-Link 整片烧、不走 IAP，上面那两件东西一件都不存在 |

**机制**：`.cproject` 里那个选项的值是 `${workspace_loc:/${ProjName}/${PLC_LD_SCRIPT}}`，变量的**工程默认值是 bootloader 那份**，写在 `.settings/org.eclipse.cdt.core.prefs` 里。工装构建靠 headless 的 `-E PLC_LD_SCRIPT=…` 临时换掉：

```
stm32cubeidec.exe … -D PORTTOOL_ENABLE=1 -E PLC_LD_SCRIPT=STM32H743IIKX_FLASH_PORTTOOL.ld -build …
```

**和 `PORTTOOL_ENABLE` 同一个原则**（[DECISIONS.md 第 14 条](DECISIONS.md)）：**只活在要它的那一次构建里，不写进工程文件**。所以随手编一次、或者在 IDE 里点一下构建，出来的都还是 bootloader。

⚠️ **重新生成后必查这两处**：

1. 那个选项的值有没有被 CubeMX 改回写死的 `STM32H743IIKX_FLASH.ld`。⚠️ **`.cproject` 里 ST 自己那个 `||` 分隔的大字符串（`com.st.stm32cube.ide.common.services.build.inputs…`）里仍然写着 bootloader 那份文件名** —— 那是 ST 用来回写的记录，所以这一处**被改回去的概率不低**。改回去的表现不是报错，是**工装镜像忽然又受 120K 限制**。
2. `.settings/org.eclipse.cdt.core.prefs` 还在不在、默认值还是不是 bootloader 那份。**这个文件没了，`${PLC_LD_SCRIPT}` 会展开成空**，链接器拿不到 `-T`。

**两处都由 `build_image.py` 自动兜着**：它在构建日志里核对链接器实际拿到的是哪一份，拿错了当场判失败 —— 因为拿错是静默的（工装拿到 bootloader 那份只是「装不下」，而 bootloader 拿到工装那份会**烧穿 app 区**）。

四个 fault handler 已经做到重新生成安全：在 PD 块里把 CubeMX 生成的版本改名让路，在 USER CODE 1 里定义真正的 naked 版本。
