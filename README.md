# Portable Menu — 可移植单片机菜单核心

C99 写的菜单引擎：**核心不依赖任何 MCU、屏幕或按键库**，只用到 `stdbool.h`、`stddef.h`、`stdint.h`、`stdio.h`、`string.h`。
存储由调用方提供（静态对象），不用堆；放进超循环或 RTOS 任务里都可以。

> **v2 说明**：这一版与旧版 `portable-menu-minimal`（`menu_task()` / `menu_set_status()` 那套 API）
> **不兼容**，属于重写。旧版仍可通过标签
> [v1](https://github.com/jx116-330/portable-menu-minimal/tree/v1) 查看。

## 能做什么

- **参数编辑**：开关、整数（i8/u8/i16/u16/i32/u32）、浮点、枚举；每个参数可以有自己独立的多档步长（旋钮切换粗调/细调）
- **只读与绑定项**：值可来自回调，读写都走业务接口（例如后台提交硬件写参、带忙碌/失败状态）
- **实时数值行**：`MENU_LIVE` 系列，一行宏搞定——只读，值一变核心自动重画那一行，**不需要写任何回调**
- **实时页面**：上半屏自绘（进度条、波形、图像、状态条），下半屏照常调参；
  `enter` / `tick` / `draw` / `update` / `is_dirty` / `event` / `can_leave` / `leave` 全部按需实现
- **按键语义化**：页面可优先接管按键，其余交给核心的导航与编辑逻辑

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `menu/menu.h` | 公开接口：类型、配置宏、页面与钩子定义 |
| `menu/menu.c` | 核心实现：页面栈、数值编辑、绘制调度、实时数值行 |
| `menu/menu_app.c`、`menu/menu_app.h` | TC264（逐飞库 + IPS200）**参考接入** + 四个示例页；编译它需要逐飞 TC264 工程 |
| `menu/README.md` | 完整使用说明：操作、增删参数/页面、实时数值、实时页心智模型与从零教程 |
| `tests/` | 主机回归测试：gcc 秒级跑完，含核心行为断言与“README 示例 ↔ 示例文件”一致性检查 |

## 30 秒接入

```c
/* 1. 实现显示适配层：按设备能力填，NULL 表示不支持该操作 */
static const MenuDisplayOps g_display = {
    clear, clear_region, text, rect, fill_rect, line, image,
    NULL /* begin */, NULL /* end */, row, status
};

/* 2. 定义页面：一行就是一个菜单项 */
MENU_PAGE_BEGIN(page_main, "Main")
    MENU_BOOL("Enabled", &g_enabled),
    MENU_U16_STEPS("Target", &g_target, 0, 1000, steps, 3U),
    MENU_LIVE_VAR("Speed", MENU_VALUE_FLOAT, &g_speed, 1U, "rpm"),
    MENU_ACTION("Save", action_save),
    MENU_SUBMENU("More", &page_more)
MENU_PAGE_END(page_main, "Main", NULL);

/* 3. 初始化一次 */
static Menu g_menu;
menu_init(&g_menu, &page_main, &g_display, NULL, NULL);

/* 4. 把板级输入翻译成语义事件 */
MenuEvent event = { MENU_KEY_UP, MENU_KEY_SHORT };
menu_handle_event(&g_menu, &event);

/* 5. 周期任务里推进（示例工程用 10 ms 一次） */
menu_tick(&g_menu, now_ms);
```

更完整的接入步骤、配置宏清单与页面写法见 [`menu/README.md`](menu/README.md)。

## 跑测试

在仓库根目录执行：

```text
pwsh -File tests/run.ps1
```

也可以只用 gcc（需要 gcc 在 PATH）：

```text
gcc -std=c99 -Wall -Wextra -pedantic -Imenu menu/menu.c tests/menu_core_test.c -o tests/menu_core_test.exe
tests/menu_core_test.exe
```

测试覆盖：实时数值行的脏检测与局部重画、读取失败显示 `?`、枚举标签、跨子页缓存失效、
实时页 `draw`/`update`/`is_dirty` 的调用时机、编辑其它行时实时行继续刷新等。

## 依赖与许可

- `menu/menu.h`、`menu/menu.c` 与文档是本仓库自己的代码，**不含任何第三方代码**，也不引入任何硬件头文件。
- `menu/menu_app.c` 是 TC264 参考接入，编译它需要逐飞 TC264 开源库（GPL3）和 Infineon iLLD；
  这些库**不在本仓库内**，请从各自来源获取并遵守各自的许可。
- 本仓库自身的授权方式尚未确定（当前没有 LICENSE 文件）。
