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

四个 fault handler 已经做到重新生成安全：在 PD 块里把 CubeMX 生成的版本改名让路，在 USER CODE 1 里定义真正的 naked 版本。
