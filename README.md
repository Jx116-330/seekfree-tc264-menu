# TC264 可移植菜单（含完整 ADS 工程）

一套跑在 TC264 上的菜单系统：逐飞开源库 + IPS200 屏幕 + 旋转编码器，
**克隆下来用 AURIX Development Studio 直接就能编译**。

菜单核心是硬件无关的 C99 代码，只依赖 5 个标准头（`stdbool/stddef/stdint/stdio/string`）；
硬件相关部分集中在 `code/menu/menu_app.c`，与核心之间只通过回调表交互。

> 仓库内含逐飞 TC264 开源库（GPL-3.0）与 Infineon iLLD（BSL-1.0），
> 因此**整个仓库按 GPL-3.0 发布**，见 [`LICENSE`](LICENSE)。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `firmware/` | **ADS 工程根**，导入这个目录即可（含 `.cproject` / `.project` / `.settings` / 链接脚本） |
| `firmware/code/menu/` | 菜单核心 `menu.h`、`menu.c` + TC264 适配与示例页 `menu_app.c` + 完整使用说明 `README.md` |
| `firmware/user/` | `cpu0_main.c`（调用 `menu_app_init()`，主循环调用 `menu_app_task()`）、`isr.c` 等 |
| `firmware/libraries/` | 逐飞 TC264 开源库（GPL-3.0）+ Infineon iLLD（BSL-1.0）+ 逐飞预编译 `zf_device_config.a` |
| `tests/` | 主机侧回归测试（gcc 秒级跑完）；**刻意放在 ADS 工程树之外**，见下文说明 |

## 用 ADS 编译（克隆后三步）

1. `git clone https://github.com/Jx116-330/portable-menu-minimal.git`
2. ADS → `File → Import → General → Existing Projects into Workspace` →
   选择仓库里的 **`firmware`** 目录 → Finish
3. 选中工程 → `Project → Build Project`（首次会自动生成 `firmware/Debug/`）

几个必须知道的点：

- 工程名由 `.project` 决定，导入后显示为 `Seekfree_TC264_Opensource_Library`。
- **`.settings/` 必须一起存在**：里面是 `DEVICE-ID=TC26B`、`aurixDevice=TC26xD_B-Step`。
  缺了它，ADS 会判定“没有配置 AURIX 器件”，把 `makefile` 换成拦截桩，
  最终报成 `amk F124: ["makefile" 5/0] missing separator`（真实原因被这层语法错误盖住）。
- 本机 TASKING 许可**禁止命令行编译**（`ctc F104: License does not support running as
  standalone`），构建只能在 ADS IDE 里做。
- 器件：TC264D（TC26xD_B-Step，BGA292）。引脚、屏幕方向、布局都在
  `firmware/code/menu/menu_app.c` 顶部的配置区。

## 菜单能做什么

- **参数编辑**：开关、整数（i8/u8/i16/u16/i32/u32）、浮点、枚举；每个参数可有多档步长（粗调/细调）
- **只读与绑定项**：值来自回调，读写走业务接口（例如后台提交摄像头写参并显示 WAIT/ERR）
- **实时数值行**：`MENU_LIVE` / `MENU_LIVE_VAR` / `MENU_LIVE_ENUM`，一行一个实时值，
  值一变核心自动重画那一行，**不需要写任何回调**
- **实时页面**：上半屏自绘（进度条、图像、状态条），下半屏照常调参；
  `enter/tick/draw/update/is_dirty/event/can_leave/leave` 按需实现
- **按键语义化**：编码器旋转 + 短按/长按（另有 `event` 钩子可让页面接管按键）

示例页有四个：`Basic Examples`、`Live State`（进度条）、`Live Values`（实时数值）、
`Camera Test`（总钻风实时图像 + 曝光/增益在线调整）。

## 主机回归测试

```text
pwsh -File tests/run.ps1
```

或者只用 gcc（需要 gcc 在 PATH）：

```text
gcc -std=c99 -Wall -Wextra -pedantic -Ifirmware/code/menu firmware/code/menu/menu.c tests/menu_core_test.c -o tests/menu_core_test.exe
tests/menu_core_test.exe
```

测试覆盖实时数值行的脏检测与局部重画、读取失败显示 `?`、枚举标签、跨子页缓存失效、
实时页 `draw`/`update`/`is_dirty` 的调用时机、编辑其它行时实时行继续刷新等，
并会校验 `menu/README.md` 里的教程示例与 `tests/realtime_page_example.c` 逐行一致。

> 为什么 `tests/` 放在工程树外面：ADS 会把工程目录下**所有 `.c`** 都编进固件
> （每个含 `.c` 的文件夹都会生成 `subdir.mk`），而主机测试自带 `main()` 且重复了
> `menu_*` 符号，放进去会让固件链接失败。

## 只想拿菜单核心？

只要 `firmware/code/menu/menu.h` + `menu.c` 两个文件：它们不引用任何 MCU、屏幕或按键头文件，
移植方式（实现 `MenuDisplayOps`、把输入翻成 `MenuEvent`、周期调用 `menu_tick`）见
[`firmware/code/menu/README.md`](firmware/code/menu/README.md)。

## 许可

- **仓库整体：GPL-3.0**（[`LICENSE`](LICENSE)），因为包含逐飞 TC264 开源库（GPL-3.0）。
- `firmware/libraries/infineon_libraries/`：Infineon iLLD，**Boost Software License 1.0**（宽松许可），见各文件头。
- `firmware/libraries/zf_device/zf_device_config.a`：逐飞提供的**预编译库**（摄像头配置数据与相关函数），
  随逐飞官方开源库一同分发；GPL-3.0 下再分发二进制建议能提供对应源码，如需更保守可自行从逐飞官方库获取该文件。
- 逐飞的许可声明见 `firmware/libraries/doc/GPL3_permission_statement.txt` 与各源文件头。
