# 菜单最小可移植版使用说明

这份菜单是从你当前 TC387 工程里的菜单、按键、编码器、IPS200 显示逻辑中抽出来的**最小整合版**。

它的目标很明确：
- 只保留菜单本体
- 保留 IPS200 显示
- 保留 4 键驱动
- 保留机械编码器驱动
- 去掉 GPS / PID / Flash / Path 等业务耦合

---

# 1. 文件说明

本文件夹只有三个文件：

- `menu.h`
- `menu.c`
- `README.md`

其中：

## `menu.h`
负责：
- 菜单结构体定义
- 按键与编码器引脚宏定义
- 菜单对外接口声明

## `menu.c`
负责：
- 4 键扫描
- 编码器解码
- IPS200 菜单显示
- 菜单切换逻辑

## `README.md`
负责：
- 告诉你怎么接入
- 告诉你怎么定义菜单
- 告诉你怎么写动作函数

---

# 2. 这份菜单当前依赖什么

它不是完全脱离逐飞库的“纯 C 标准版”，它仍然依赖你工程里的这些底层库：

- `zf_common_typedef.h`
- `zf_common_debug.h`
- `zf_driver_gpio.h`
- `zf_driver_delay.h`
- `zf_device_ips200.h`

也就是说，这份菜单最适合直接放进你的 **TC387 + 逐飞库工程** 中用。

---

# 3. 默认硬件引脚

## 按键
在 `menu.h` 中定义：

```c
#define MENU_KEY_LIST {P20_2, P20_8, P20_6, P20_7}
```

对应：
- `MENU_KEY_1`
- `MENU_KEY_2`
- `MENU_KEY_3`
- `MENU_KEY_4`

当前菜单实际只用到了：
- `KEY1 短按`：进入/确认
- `KEY1 长按`：返回

---

## 编码器
在 `menu.h` 中定义：

```c
#define MENU_ENCODER_A_PIN P20_3
#define MENU_ENCODER_B_PIN P20_0
```

作用：
- 编码器转动控制菜单上下移动

---

## 屏幕
这份菜单默认使用：

```c
ips200_init(IPS200_TYPE_SPI);
```

也就是 **IPS200 SPI 屏幕**。

如果你后面改成并口屏，可以把 `menu.c` 中：

```c
ips200_init(IPS200_TYPE_SPI);
```

改成：

```c
ips200_init(IPS200_TYPE_PARALLEL8);
```

或者把 `MENU_USE_IPS200_SPI` 宏改掉。

---

# 4. 菜单操作方式

当前交互规则：

- **编码器旋转**：上下切换菜单项
- **KEY1 短按**：进入子菜单 / 执行动作
- **KEY1 长按**：返回上一级

---

# 5. 如何接入到你的工程

## 第一步：把文件拷进工程
建议你放到类似位置：

```text
code/menu_lite/
    menu.c
    menu.h
```

然后加入 ADS 工程编译。

---

## 第二步：在主程序里包含头文件
例如在 `cpu0_main.c` 里：

```c
#include "menu.h"
```

---

## 第三步：定义你的菜单树
你需要自己在某个 `.c` 文件里定义 `MenuItem` 和 `MenuPage`。

例如最简单示例：

```c
#include "menu.h"

static void action_test_1(void)
{
    menu_set_status("Action 1 OK");
}

static void action_test_2(void)
{
    menu_set_status("Action 2 OK");
}

MenuItem pid_items[] = {
    {"1. Edit Kp", action_test_1, 0},
    {"2. Edit Ki", action_test_2, 0},
};

MenuPage pid_menu = {
    "PID",
    pid_items,
    sizeof(pid_items) / sizeof(pid_items[0]),
    0
};

MenuItem main_items[] = {
    {"1. PID", 0, &pid_menu},
};

MenuPage main_menu = {
    "Main",
    main_items,
    sizeof(main_items) / sizeof(main_items[0]),
    0
};
```

---

## 第四步：初始化菜单
在 `core0_main()` 初始化区调用：

```c
menu_init(&main_menu);
```

注意：
- `main_menu` 必须是你定义好的根菜单页

---

## 第五步：主循环调用菜单任务
在主循环中调用：

```c
while (TRUE)
{
    menu_task();
}
```

如果你主循环里还有别的任务，可以这样：

```c
while (TRUE)
{
    other_task();
    menu_task();
}
```

---

# 6. 菜单项的三种写法

## 写法 1：执行动作

```c
{"Save", action_save, 0}
```

表示：
- 进入这个菜单项时，不跳子菜单
- 直接执行 `action_save()`

---

## 写法 2：进入子菜单

```c
{"PID", 0, &pid_menu}
```

表示：
- 这个菜单项没有动作函数
- 短按进入 `pid_menu`

---

## 写法 3：空菜单项
不建议这样写，但技术上可以：

```c
{"Reserved", 0, 0}
```

它会显示出来，但短按无效果。

---

# 7. 如何写动作函数

动作函数固定形式：

```c
static void action_xxx(void)
{
    // 你的代码
}
```

例如：

## 示例 1：显示状态提示
```c
static void action_beep(void)
{
    menu_set_status("Beep trigger");
}
```

## 示例 2：切换某个标志位
```c
static uint8 led_enable = 0;

static void action_led_toggle(void)
{
    led_enable = !led_enable;
    if (led_enable)
        menu_set_status("LED ON");
    else
        menu_set_status("LED OFF");
}
```

## 示例 3：调用你原有业务函数
```c
static void action_start_gps(void)
{
    gnss_init(TAU1201);
    menu_set_status("GPS Init Done");
}
```

---

# 8. `menu_set_status()` 是干什么的

这是给你专门留的一个简易状态显示接口。

例如：

```c
menu_set_status("Path saved");
```

屏幕底部状态行会显示这句文字。

适合用来提示：
- 执行成功
- 执行失败
- 参数变化
- 当前模式

---

# 9. 如果你想把你现在的 GPS/PID 菜单迁回去，怎么做

思路很简单：

## 先迁菜单树
把你原来：
- `gps_items[]`
- `pid_items[]`
- `main_items[]`

迁过来。

## 再迁动作函数
把你原来：
- `gps_action_xxx()`
- `pid_action_xxx()`

按需一个一个加回来。

## 最后再迁特殊页面
这份最小版目前**没有集成 GPS 特殊数据页、PID 动态预览页**这种“菜单外页面模式”。

如果你后面要恢复：
- 动态数据显示页
- 地图页
- PID Preview 页

建议做法是：
- 先保留这份菜单做菜单骨架
- 再额外加一个 `app_mode` 状态机
- 在 `menu_task()` 之外切换到专用页面逻辑

也就是说：
**这份最小版适合作为基础菜单，不负责复杂业务页接管。**

---

# 10. 当前版本的边界

这份最小版已经包含：
- 屏幕驱动调用
- 按键驱动
- 编码器驱动
- 菜单绘制
- 页面跳转
- 动作执行

但它**没有包含**：
- GPS 页面
- PID 编辑页
- Flash 存档
- 动态区域绘图
- 图像预览

这是故意的，因为你这次要的是**最小可移植版**。

---

# 11. 推荐接入模板

你可以按下面这个模板开始用：

```c
#include "myhead.h"
#include "menu.h"

static void action_test(void)
{
    menu_set_status("Test OK");
}

MenuItem sub_items[] = {
    {"1. Test", action_test, 0},
};

MenuPage sub_menu = {
    "SubMenu",
    sub_items,
    sizeof(sub_items) / sizeof(sub_items[0]),
    0
};

MenuItem main_items[] = {
    {"1. Enter", 0, &sub_menu},
};

MenuPage main_menu = {
    "Main",
    main_items,
    sizeof(main_items) / sizeof(main_items[0]),
    0
};

int core0_main(void)
{
    clock_init();
    debug_init();

    menu_init(&main_menu);

    while (TRUE)
    {
        menu_task();
    }
}
```

---

# 12. 你后面如果继续要优化，优先改哪几个方向

如果你后面想把它继续变强，我建议按这个顺序扩展：

## 第一优先级
- 增加 `KEY2/KEY3/KEY4` 功能
- 增加更多状态提示

## 第二优先级
- 增加参数编辑项
- 增加数值增减界面

## 第三优先级
- 增加“菜单模式 / 页面模式”切换
- 支持 GPS 数据页、波形页、图像页

## 第四优先级
- 增加配置保存
- 增加通用参数绑定机制

---

# 13. 一句话总结

这份 `menu.c + menu.h`：

**适合你现在快速迁移、快速复用、快速起菜单。**

如果你后面要，我还可以继续帮你做第二版：
- 加参数编辑
- 加专用页面
- 加保存机制
- 加更完整的通用菜单框架
