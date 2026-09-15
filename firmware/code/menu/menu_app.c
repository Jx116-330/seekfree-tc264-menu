#include "menu.h"
#include "menu_app.h"
#include "zf_common_headfile.h"
#include "IfxStm.h"

#include <stdio.h>
#include <string.h>

/* 原 system_getval_ms() 先截取 32 位 STM，约 43 秒回绕；
 * 菜单使用完整 STM 后再转毫秒，避免通信/断流超时被短回绕误触发。 */
/*********************************************************************************************************************
 * @brief  : 获取菜单使用的单调毫秒时间戳
 * @param  : 无
 * @return : 当前毫秒时间戳
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static uint32_t tc264_menu_now_ms(void)
{
    static uint32_t ticks_per_ms;
    uint32_t irq_state;
    uint64_t ticks;
    if (0U == ticks_per_ms)
        ticks_per_ms = (uint32_t)(IfxStm_getFrequency(&MODULE_STM0) / 1000.0f);
    /* TIM0/CAP 必须成对读取；关中断仅覆盖读寄存器，除法在恢复后执行。 */
    irq_state = interrupt_global_disable();
    ticks = IfxStm_get(&MODULE_STM0);
    interrupt_global_enable(irq_state);
    return (uint32_t)(ticks / ticks_per_ms);
}

/* ============================================================================
 * 菜单应用配置区
 *
 * 后续要换引脚、屏幕方向、字体、菜单布局或参数默认值，优先只改这里。
 * 下面的驱动实现和菜单核心调用不需要跟着改。
 * ========================================================================== */

/* 1) 旋转编码器和蜂鸣器引脚：按原理图修改这四个宏即可。 */
#define MENU_ENCODER_A_PIN              (P02_6) /* 编码器 A 相 */
#define MENU_ENCODER_B_PIN              (P02_7) /* 编码器 B 相 */
#define MENU_ENCODER_SW_PIN             (P02_5) /* 编码器按键 E */
#define MENU_BEEP_PIN                   (P33_10) /* 蜂鸣器，初始化为关闭 */

/* 2) 编码器手感参数：每个机械卡点通常产生 4 个边沿。 */
#define MENU_ENCODER_COUNTS_PER_STEP    (4)
#define MENU_ENCODER_PENDING_LIMIT      (8)
#define MENU_ENCODER_DIR_SIGN           (1)
#define MENU_ENCODER_SW_DEBOUNCE_MS     (20U)
#define MENU_ENCODER_SW_LONG_MS         (250U)
#define MENU_TICK_PERIOD_MS             (10U)

/* 3) IPS200 显示配置：当前工程使用硬件 SPI、竖屏、8x16 字体。 */
#define MENU_DISPLAY_DIR                (IPS200_PORTAIT)
#define MENU_DISPLAY_FONT               (IPS200_8X16_FONT)
#define MENU_DISPLAY_TEST_ONLY          (0U)

/* 4) 菜单页面布局：width/height 是屏幕分辨率，单位为像素。 */
static const MenuLayout g_menu_layout = {
    240U, /* width:  屏幕宽度 */
    320U, /* height: 屏幕高度 */
    40U,  /* title_height: 标题区域高度 */
    20U,  /* row_height:   普通菜单行高度 */
    20U,  /* footer_height:底部状态栏高度 */
    1U    /* wrap_navigation: 到边界后是否循环 */
};

/* 5) 字体和绘图缓冲区参数：需与上面的屏幕布局匹配。 */
#define TC264_ITEM_TEXT_WIDTH           (8U)
#define TC264_ITEM_TEXT_HEIGHT          (16U)
#define TC264_TITLE_TEXT_WIDTH          (8U)
#define TC264_TITLE_TEXT_HEIGHT         (16U)
#define TC264_DRAW_BUFFER_WIDTH         (240U)
#define TC264_DRAW_BUFFER_HEIGHT        (24U)
#define TC264_ROW_CACHE_COUNT           (16U)
#define CAMERA_VIEW_WIDTH               (240U) /* IPS200 全宽 */
#define CAMERA_VIEW_HEIGHT              (153U) /* 按 188:120 等比例放大 */
#define CAMERA_PANEL_HEIGHT             (180U) /* 153 像素图像 + 独立状态条；下方留四行参数 */
/* 采集 FPS 在 zf_device_mt9v03x.h 固定为 200，不跟随预览刷新率变化。
 * 0x35 是 MT9V032/034 的模拟增益寄存器，16~64 对应约 1~4 倍。
 * 参考 OpenMV drivers/sensors/mt9v0xx.h；保持默认手动曝光/增益模式。 */
#define CAMERA_ANALOG_GAIN_REG          (0x35U)
#define CAMERA_WRITE_INTERVAL_MS        (100U) /* 连续旋转合并提交，最多每秒 10 次 */
#define CAMERA_FRAME_TIMEOUT_MS         (250U) /* 完整帧停止更新后显示 LOST */
#define CAMERA_WRITE_EXPOSURE           (1U)
#define CAMERA_WRITE_GAIN               (2U)

/* 6) 示例菜单的业务变量：真实项目可在这里替换成自己的参数。 */
static bool g_demo_enabled;
static uint32_t g_demo_u32 = 25U;
static float g_demo_gain = 1.25f;
static uint32_t g_demo_mode;
static uint32_t g_demo_actions;
static uint32_t g_demo_ticks;
static uint8_t g_demo_running;
static uint8_t g_demo_progress;
static uint8_t g_demo_view_last_running;
static uint8_t g_demo_view_last_progress;
static uint8_t g_demo_view_state_valid;
static char g_demo_view_last_text[48];
static uint8_t g_demo_saved;
static uint16_t g_camera_exposure = MT9V03X_EXP_TIME_DEF;
static uint16_t g_camera_gain = MT9V03X_GAIN_DEF;
static bool g_camera_binary;              /* OFF 灰度；ON 按阈值显示黑白图，不改采集原图 */
static uint8_t g_camera_threshold = 128U;  /* 灰度小于阈值显示黑，范围 1~255 */
static uint16_t g_camera_preview_fps = 50U; /* 只控制 LCD 预览，范围 5~60 */
static bool g_camera_freeze;              /* 只冻结 LCD 图像，摄像头继续采集 */
static uint8_t g_camera_write_error;      /* 每个参数一位；失败目标保留，ERR 不代表生效 */
static uint8_t g_camera_write_pending;    /* 最新目标待提交，不排队保存中间值 */
static uint8_t g_camera_write_active;     /* 当前事务对应的参数位 */
static uint8_t g_camera_write_last = CAMERA_WRITE_GAIN; /* 两项交替，避免饥饿 */
static uint32_t g_camera_last_write_ms;
static uint32_t g_camera_monitor_frame;
static uint32_t g_camera_last_frame_ms;
static bool g_camera_stream_lost;
static uint8_t g_camera_image_pending;
static uint32_t g_camera_last_preview_ms;
static uint32_t g_camera_preview_frames;
static uint32_t g_camera_preview_measured_fps;
static char g_camera_last_status[30];
static uint32_t g_camera_frame_count;
static uint32_t g_camera_rendered_frame;
static uint32_t g_camera_fps_window_start_ms;
static uint32_t g_camera_fps_window_frames;
static uint32_t g_camera_display_fps; /* DMA 完整帧计数得到的采集 FPS，不是 LCD FPS */
static uint8_t g_camera_initialized;

/* 数组区：MENU_ENUM 用 values / labels，MENU_xxx_STEPS 用步长数组。
 * 一个参数一套档位；数组有几项，页面里最后就写几。 */
static const uint32_t g_demo_enum_values[] = { 0U, 2U, 7U, 42U };
static const char *const g_demo_enum_labels[] = { "OFF", "LOW", "HIGH", "AUTO" };
static const double g_demo_u32_steps[] = { 25.0, 100.0, 250.0 };
static const double g_demo_steps[] = { 0.01, 0.1, 1.0, 10.0 };
static const double g_camera_exposure_steps[] = { 1.0, 10.0, 100.0 };
static const double g_camera_gain_steps[] = { 1.0, 4.0, 8.0 };
static const double g_camera_threshold_steps[] = { 1.0, 5.0, 20.0 };

/* 7) 实时数值演示页的业务变量（“Live Values” 页）：由 live_demo_tick 按时间
 * 派生，只做示范用。真实项目把这些换成自己的快照变量即可。 */
static float g_live_gyro_z;               /* 浮点 + 小数位数 + 单位：陀螺仪、PID 输出这类值 */
static uint8_t g_live_level;              /* 0~100，带 % 单位 */
static uint32_t g_live_state;             /* 枚举：显示标签，不是原始数字 */
static uint16_t g_live_step = 10U;        /* 可编辑参数：编辑它时观察上面的实时行是否继续刷新 */

static const uint32_t g_live_state_values[] = { 0U, 1U, 2U };
static const char *const g_live_state_labels[] = { "IDLE", "RUN", "FAULT" };

/*
 * 下面这些是“页面扩展功能”的函数声明。
 * 新人先记住：普通参数菜单不需要改这里；只有做实时页面、自定义项、
 * 参数读写回调或实时数值的 read 回调时才会用到。声明只是提前告诉编译器
 * 函数长什么样，真正的函数代码在后面，不能把这里当成菜单配置来改。
 */
static void demo_action_count(void *user);
static void demo_action_save(void *user);
static void demo_action_start(void *user);
static void demo_action_cancel(void *user);
static void demo_view_enter(void *user);
static void demo_view_tick(void *user, uint32_t now_ms);
static MenuEventResult demo_view_event(void *user, const MenuEvent *event);
static bool demo_view_can_leave(void *user);
static void demo_view_leave(void *user);
static bool demo_view_is_dirty(void *user);
static void demo_view_draw(void *user, MenuCanvas *canvas);
static void demo_view_update(void *user, MenuCanvas *canvas);
static bool demo_read_uptime(void *user, MenuValue *value);
static void live_demo_tick(void *user, uint32_t now_ms);
static const MenuPageHooks g_live_values_hooks;
static bool camera_read_exposure(void *user, MenuValue *value);
static bool camera_write_exposure(void *user, const MenuValue *value);
static bool camera_read_gain(void *user, MenuValue *value);
static bool camera_write_gain(void *user, const MenuValue *value);
static void camera_view_enter(void *user);
static void camera_view_tick(void *user, uint32_t now_ms);
static void camera_view_draw(void *user, MenuCanvas *canvas);
static void camera_view_update(void *user, MenuCanvas *canvas);
static bool camera_view_is_dirty(void *user);
static const MenuPageHooks g_camera_view_hooks;

/* 实时页面的钩子表定义在后面的实现代码里，这里仅保留声明。 */
static const MenuPageHooks g_demo_view_hooks;

/*
 * 菜单页面定义区（最常修改的区域；把这个菜单拿去用，基本只改这里）。
 * ============================================================================
 * 三条规则先记住：
 *   1. MENU_PAGE_BEGIN / MENU_PAGE_END 包住一页，中间“一行就是一个菜单项”。
 *   2. 每行第一个字符串是屏幕上的名字，后面跟着它绑定的变量或函数。
 *   3. BEGIN 的第二个参数是页面标题，显示在屏幕最上面。
 *
 * 加一个能旋转修改的参数，照抄三步（下面 g_target 只是示例名字，自己起名，
 * 不要和已有的变量重名）：
 *   第一步：在“6) 示例菜单的业务变量”里加变量
 *               static uint16_t g_target = 100U;
 *   第二步：在变量区下面的数组区加它的档位（旋钮在这些档位之间切换）
 *               static const double g_target_steps[] = { 1.0, 10.0, 50.0 };
 *   第三步：在页面里加一行（整项写成一行；除最后一项外，行尾要有逗号）
 *               MENU_U16_STEPS("Target", &g_target, 0, 1000, g_target_steps, 3U),
 *           依次是：屏幕名字、变量地址、最小值、最大值、档位数组、档位数
 *           （数组有几项，档位数就写几）
 *
 * 一行宏怎么选，按用途分三类：
 *
 *   可调参数：短按进入编辑，旋转改值，长按退出编辑
 *     MENU_BOOL         开关量，显示 ON / OFF
 *     MENU_U8/U16/U32   无符号整数；MENU_I8/I16/I32 是有符号整数
 *     MENU_FLOAT        浮点数（固定 2 位小数），后面是最小值、最大值、步长
 *     MENU_xxx_STEPS    上面几种的多档步长版，可切换粗调 / 细调
 *     MENU_ENUM         枚举：实际值来自 values，显示文字来自 labels
 *
 *   实时数值：只显示不让改，值一变核心自动重画这一行，不用写任何回调
 *     MENU_LIVE_VAR     直接绑定变量，陀螺仪、PID 输出这类快照最常用
 *     MENU_LIVE         值由 read 回调算出来（需要换算时用）
 *     MENU_LIVE_ENUM    实时枚举，显示 labels 里匹配的文字
 *     浮点实时行倒数第二个参数是小数位数，最后一个是单位文本（不写就填 NULL）
 *
 *   动作和跳转
 *     MENU_ACTION_CTX   短按执行一个函数，例如保存、启动、取消
 *     MENU_SUBMENU      短按进入另一个页面
 *
 * MENU_PAGE_END 的第三个参数（页面的实时能力）：
 *   NULL                 普通页，上面三类就够用
 *   &某个MenuPageHooks   实时页，需要自己画图或接管按键时才用，参考摄像头页
 *
 * 每个 MENU_ 开头的宏在 menu.h 里都有一句中文说明；更完整的教程和踩坑点见
 * 同目录 README.md（“增加一个参数，只需三步”“显示一个实时数值怎么理解”）。
 */
static const MenuPage g_demo_items_page;
static const MenuPage g_demo_live_page;
static const MenuPage g_demo_live_values_page;
static const MenuPage g_camera_page;
static const MenuPage g_demo_root_page;

MENU_PAGE_BEGIN(g_demo_items_page, "Basic Examples")
    /* 最基础：开关量，短按进入编辑，旋转切换 ON/OFF。 */
    MENU_BOOL("Enabled", &g_demo_enabled),
    /* 多档步长整数：每个参数都可以绑定自己独立的 steps 数组。 */
    MENU_U32_STEPS("Count", &g_demo_u32, 0, 1000, g_demo_u32_steps, 3U),
    /* 多档步长浮点数：短按编码器可切换 0.01/0.1/1.0/10.0。 */
    MENU_FLOAT_STEPS("Gain", &g_demo_gain, 0.0, 20.0, g_demo_steps, 4U),
    /* 枚举：显示文字来自 g_demo_enum_labels，实际值来自 values。 */
    MENU_ENUM("Mode", &g_demo_mode, g_demo_enum_values, g_demo_enum_labels, 4U),
    /* 动作：短按执行回调函数。 */
    MENU_ACTION_CTX("Save", demo_action_save, NULL),
    MENU_ACTION_CTX("Count Action", demo_action_count, &g_demo_actions)
MENU_PAGE_END(g_demo_items_page, "Basic Examples", NULL);

MENU_PAGE_BEGIN(g_demo_live_page, "Live State")
    MENU_ACTION_CTX("Start", demo_action_start, NULL),
    MENU_ACTION_CTX("Cancel", demo_action_cancel, NULL),
    MENU_ACTION_CTX("Save State", demo_action_save, NULL),
    MENU_ACTION_CTX("Count Action", demo_action_count, &g_demo_actions),
    /* 实时数值行：不用写页面钩子，核心每 tick 比较值并只重画变化的那一行。 */
    MENU_LIVE_VAR("Ticks", MENU_VALUE_U32, &g_demo_ticks, 0U, NULL),
    MENU_LIVE_VAR("Progress", MENU_VALUE_U8, &g_demo_progress, 0U, "%")
MENU_PAGE_END(g_demo_live_page, "Live State", &g_demo_view_hooks);

/*
 * 实时数值演示页：一页看懂实时数值行的代表性写法。
 *
 *   GyroZ  浮点 + 小数位数 + 单位：陀螺仪、PID 输出这类值的标准写法
 *   Level  u8 整数 + % 单位
 *   State  枚举：显示 labels 里的文字，不是原始数字
 *   Step   可编辑参数：编辑它时，上面几行应当继续刷新
 *
 * 上面几行都用 MENU_LIVE_VAR 直接绑定变量；用 read 回调取值的写法见首页的
 * Uptime 行（MENU_LIVE），枚举标签的写法见 State 行（MENU_LIVE_ENUM）。
 */
MENU_PAGE_BEGIN(g_demo_live_values_page, "Live Values")
    MENU_LIVE_VAR("GyroZ", MENU_VALUE_FLOAT, &g_live_gyro_z, 1U, "dps"),
    MENU_LIVE_VAR("Level", MENU_VALUE_U8, &g_live_level, 0U, "%"),
    MENU_LIVE_ENUM("State", &g_live_state, g_live_state_values, g_live_state_labels, 3U),
    MENU_U16("Step", &g_live_step, 0, 1000, 10)
MENU_PAGE_END(g_demo_live_values_page, "Live Values", &g_live_values_hooks);

MENU_PAGE_BEGIN(g_camera_page, "Camera Test")
    /* 总钻风实时曝光：图像在上方，旋钮编辑项在下方。 */
    MENU_BIND_STEPS("Exposure", MENU_VALUE_U16, camera_read_exposure, camera_write_exposure, &g_camera_exposure, 1, 4095, g_camera_exposure_steps, 3U),
    /* 后台写入传感器：WAIT 表示待完成，ERR 表示目标尚未确认生效。 */
    MENU_BIND_STEPS("Analog Gain", MENU_VALUE_U16, camera_read_gain, camera_write_gain, &g_camera_gain, 16, 64, g_camera_gain_steps, 3U),
    /* 以下都是预览参数，不改变摄像头 188x120 的原始灰度数据。 */
    MENU_BOOL("Binary", &g_camera_binary),
    MENU_U8_STEPS("Threshold", &g_camera_threshold, 1, 255, g_camera_threshold_steps, 3U),
    MENU_U16("LCD FPS", &g_camera_preview_fps, 5, 60, 5),
    MENU_BOOL("Freeze", &g_camera_freeze)
MENU_PAGE_END(g_camera_page, "Camera Test", &g_camera_view_hooks);

MENU_PAGE_BEGIN(g_demo_root_page, "Portable Menu Test")
    MENU_SUBMENU("Basic Examples", &g_demo_items_page),
    MENU_SUBMENU("Live State", &g_demo_live_page),
    MENU_SUBMENU("Live Values", &g_demo_live_values_page),
    MENU_SUBMENU("Camera Test", &g_camera_page),
    MENU_ACTION_CTX("Save", demo_action_save, NULL),
    MENU_ACTION_CTX("Count Action", demo_action_count, &g_demo_actions),
    /* 普通页（hooks 为 NULL）同样能实时刷新：运行时间每 0.1 秒变化一次。 */
    MENU_LIVE("Uptime", MENU_VALUE_FLOAT, demo_read_uptime, NULL, 1U, "s")
MENU_PAGE_END(g_demo_root_page, "Portable Menu Test", NULL);

/*********************************************************************************************************************
 * @brief  : 执行计数演示动作
 * @param  : user  业务上下文地址
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void demo_action_count(void *user)
{
    uint32_t *counter = (uint32_t *)user;
    if (NULL != counter) ++(*counter);
}

/*********************************************************************************************************************
 * @brief  : 执行保存标志演示动作
 * @param  : user  业务上下文地址
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void demo_action_save(void *user)
{
    (void)user;
    g_demo_saved = 1U;
}

/*********************************************************************************************************************
 * @brief  : 启动实时状态演示任务
 * @param  : user  业务上下文地址
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void demo_action_start(void *user)
{
    (void)user;
    g_demo_running = 1U;
    g_demo_progress = 0U;
}

/*********************************************************************************************************************
 * @brief  : 取消实时状态演示任务
 * @param  : user  业务上下文地址
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void demo_action_cancel(void *user)
{
    (void)user;
    g_demo_running = 0U;
}

/*********************************************************************************************************************
 * @brief  : 读取上电运行时间，单位为秒
 * @param  : user  业务上下文地址，本示例未使用
 * @param  : value  菜单值容器
 * @return : true 表示读取成功，false 表示参数地址无效
 * @date   : 2026年9月15日
 * @author : Jx116
 *********************************************************************************************************************/
static bool demo_read_uptime(void *user, MenuValue *value)
{
    uint32_t now_ms;
    (void)user;
    if (NULL == value) return false;
    now_ms = tc264_menu_now_ms();
    /* 先量化到显示分辨率（0.1 秒）再返回。实时数值行的刷新由“原始值变化”
     * 触发，直接返回毫秒会让这一行每个 tick 都重画，而屏幕上看不出区别。 */
    value->type = MENU_VALUE_FLOAT;
    value->as.f32 = (float)(now_ms / 100U) / 10.0f;
    return true;
}

/*********************************************************************************************************************
 * @brief  : 按时间派生实时数值演示页的演示值
 * @param  : user  业务上下文地址，本示例未使用
 * @param  : now_ms  当前毫秒时间戳
 * @return : 无
 * @date   : 2026年9月15日
 * @author : Jx116
 *********************************************************************************************************************/
static void live_demo_tick(void *user, uint32_t now_ms)
{
    (void)user;
    /* 每个值都先量化到“屏幕上看得出差别”的粒度：实时行的刷新由原始值变化
     * 触发，不量化就会为了看不见的变化反复重画。 */
    g_live_gyro_z = (float)((int32_t)((now_ms / 100U) % 4000U) - 2000) / 10.0f; /* -200.0 ~ 199.9 */
    g_live_level = (uint8_t)((now_ms / 500U) % 101U);                          /* 0 ~ 100 */
    g_live_state = (now_ms / 3000U) % 3U;                                      /* IDLE/RUN/FAULT */
}

/*
 * 实时数值演示页的钩子表（一般不需要修改）。
 *
 * 本页只需要一个 tick 来推进演示值：view_height 为 0 表示不申请实时视图区，
 * draw/update/is_dirty 全部留空；列表行的实时刷新由 MENU_LIVE 内核完成。
 */
static const MenuPageHooks g_live_values_hooks = {
    NULL,              /* 1: 进入页面 */
    live_demo_tick,    /* 2: 周期更新：推进演示值 */
    NULL,              /* 3: 首次完整绘制：没有实时视图区 */
    NULL,              /* 4: 页面按键事件：交给默认处理 */
    NULL,              /* 5: 退出许可检查：随时可退 */
    NULL,              /* 6: 离开页面 */
    NULL,              /* 7: 无额外业务上下文 */
    0U,                /* 8: 不申请实时视图区 */
    NULL,              /* 9: 局部刷新：由 MENU_LIVE 行自己处理 */
    NULL               /* 10: 脏检查：本页不使用视图刷新 */
};

/*********************************************************************************************************************
 * @brief  : 进入实时状态演示页面时初始化状态
 * @param  : user  业务上下文地址
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void demo_view_enter(void *user)
{
    (void)user;
    g_demo_running = 0U;
    g_demo_progress = 0U;
    g_demo_view_state_valid = 0U;
    g_demo_view_last_text[0] = '\0';
}

/*********************************************************************************************************************
 * @brief  : 推进实时状态演示进度
 * @param  : user  业务上下文地址
 * @param  : now_ms  当前毫秒时间戳
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void demo_view_tick(void *user, uint32_t now_ms)
{
    (void)user;
    (void)now_ms;
    ++g_demo_ticks;
    if (0U != g_demo_running)
    {
        if (g_demo_progress < 100U) ++g_demo_progress;
        if (g_demo_progress >= 100U) g_demo_running = 0U;
    }
}

/*********************************************************************************************************************
 * @brief  : 判断实时状态演示页面是否允许退出
 * @param  : user  业务上下文地址
 * @return : true 表示允许退出，false 表示禁止退出
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static bool demo_view_can_leave(void *user)
{
    (void)user;
    return (0U == g_demo_running);
}

/*********************************************************************************************************************
 * @brief  : 离开实时状态演示页面时清理状态
 * @param  : user  业务上下文地址
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void demo_view_leave(void *user)
{
    (void)user;
    g_demo_running = 0U;
}

/*********************************************************************************************************************
 * @brief  : 判断实时状态演示页面是否需要刷新
 * @param  : user  业务上下文地址
 * @return : true 表示需要刷新，false 表示无需刷新
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static bool demo_view_is_dirty(void *user)
{
    (void)user;
    return (0U == g_demo_view_state_valid) ||
           (g_demo_view_last_running != g_demo_running) ||
           (g_demo_view_last_progress != g_demo_progress);
}

/*********************************************************************************************************************
 * @brief  : 格式化实时状态演示文本
 * @param  : text  文本缓冲区或待绘制文本
 * @param  : text_size  文本缓冲区容量
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void demo_view_format_text(char *text, uint16_t text_size)
{
    if ((NULL == text) || (0U == text_size)) return;
    (void)snprintf(text, text_size, "RUN %s  %3u%%", (0U != g_demo_running) ? "YES" : "NO",
                   (unsigned)g_demo_progress);
}

/*********************************************************************************************************************
 * @brief  : 处理实时状态演示页面的专用按键
 * @param  : user  业务上下文地址
 * @param  : event  菜单语义按键事件
 * @return : 页面事件处理结果
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static MenuEventResult demo_view_event(void *user, const MenuEvent *event)
{
    (void)user;
    if ((NULL == event) || (MENU_KEY_SHORT != event->press)) return MENU_EVENT_IGNORED;
    if (MENU_KEY_AUX1 == event->key)
    {
        g_demo_running = 1U;
        g_demo_progress = 0U;
        return MENU_EVENT_IGNORED;
    }
    if (MENU_KEY_AUX2 == event->key)
    {
        g_demo_running = 0U;
        return MENU_EVENT_IGNORED;
    }
    return MENU_EVENT_IGNORED;
}

/*********************************************************************************************************************
 * @brief  : 完整绘制实时状态演示视图区
 * @param  : user  业务上下文地址
 * @param  : canvas  自定义视图画布
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void demo_view_draw(void *user, MenuCanvas *canvas)
{
    char text[48];
    uint16_t bar_width;
    (void)user;
    if (NULL == canvas) return;
    menu_canvas_rect(canvas, 0U, 0U, canvas->width - 1U, canvas->height - 1U);
    demo_view_format_text(text, sizeof(text));
    menu_canvas_text(canvas, 4U, 4U, text);
    (void)snprintf(g_demo_view_last_text, sizeof(g_demo_view_last_text), "%s", text);
    bar_width = (uint16_t)(((uint32_t)(canvas->width - 12U) * g_demo_progress) / 100U);
    menu_canvas_rect(canvas, 4U, 22U, (uint16_t)(canvas->width - 12U), 12U);
    menu_canvas_fill_rect(canvas, 6U, 24U, bar_width, 8U);
    menu_canvas_line(canvas, 4U, 44U, (uint16_t)(canvas->width - 8U), 44U);
    menu_canvas_text(canvas, 4U, 48U, "AUX1 start / AUX2 cancel");
    menu_canvas_image(canvas, 0U, 60U, 1U, 1U, NULL);
    g_demo_view_last_running = g_demo_running;
    g_demo_view_last_progress = g_demo_progress;
    g_demo_view_state_valid = 1U;
}

/*********************************************************************************************************************
 * @brief  : 局部更新实时状态演示视图区
 * @param  : user  业务上下文地址
 * @param  : canvas  自定义视图画布
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void demo_view_update(void *user, MenuCanvas *canvas)
{
    char text[48];
    uint16_t old_bar_width;
    uint16_t new_bar_width;
    uint16_t text_len = 0U;
    uint16_t old_text_len = 0U;
    uint16_t run_start;
    uint16_t run_end;
    char saved_char;
    (void)user;
    if ((NULL == canvas) || (NULL == canvas->display) || (NULL == canvas->display->clear_region)) return;

    if (!demo_view_is_dirty(user)) return;

    /* 状态文字按字符差分，进度变化时只更新真正变化的数字。 */
    demo_view_format_text(text, sizeof(text));
    while ('\0' != text[text_len]) ++text_len;
    while ('\0' != g_demo_view_last_text[old_text_len]) ++old_text_len;
    run_start = 0U;
    while (run_start < text_len)
    {
        if ((run_start < old_text_len) && (g_demo_view_last_text[run_start] == text[run_start]))
        {
            ++run_start;
            continue;
        }
        run_end = run_start;
        while ((run_end < text_len) && ((run_end >= old_text_len) ||
                (g_demo_view_last_text[run_end] != text[run_end])))
        {
            ++run_end;
        }
        canvas->display->clear_region(canvas->display_user, (uint16_t)(canvas->x + 4U + run_start * 8U),
            (uint16_t)(canvas->y + 4U), (uint16_t)((run_end - run_start) * 8U), 16U);
        saved_char = text[run_end];
        text[run_end] = '\0';
        menu_canvas_text(canvas, (uint16_t)(4U + run_start * 8U), 4U, &text[run_start]);
        text[run_end] = saved_char;
        run_start = run_end;
    }
    (void)snprintf(g_demo_view_last_text, sizeof(g_demo_view_last_text), "%s", text);
    old_bar_width = (uint16_t)(((uint32_t)(canvas->width - 12U) *
                                g_demo_view_last_progress) / 100U);
    new_bar_width = (uint16_t)(((uint32_t)(canvas->width - 12U) *
                                g_demo_progress) / 100U);
    if (new_bar_width > old_bar_width)
    {
        menu_canvas_fill_rect(canvas, (uint16_t)(6U + old_bar_width), 24U, (uint16_t)(new_bar_width - old_bar_width), 8U);
    }
    else if (old_bar_width > new_bar_width)
    {
        canvas->display->clear_region(canvas->display_user, (uint16_t)(canvas->x + 6U + new_bar_width),
                                      (uint16_t)(canvas->y + 24U),
                                      (uint16_t)(old_bar_width - new_bar_width), 8U);
    }
    g_demo_view_last_running = g_demo_running;
    g_demo_view_last_progress = g_demo_progress;
    g_demo_view_state_valid = 1U;
}

/*
 * 实时页面生命周期回调表（一般不需要修改）。
 * 每一行对应 MenuPageHooks 结构体中的一个字段：
 *   1. enter     进入实时页面时调用一次
 *   2. tick      菜单周期任务中反复调用，推进业务状态
 *   3. draw      首次进入页面时绘制完整实时区域
 *   4. event     实时页面优先处理按键事件
 *   5. can_leave 退出前检查是否允许离开
 *   6. leave     确认离开页面后调用一次
 *   7. user      业务上下文指针；本示例不需要，所以是 NULL
 *   8. view_height 实时绘图区高度，这里是 70 像素
 *   9. update    运行中只刷新发生变化的区域
 *  10. is_dirty  判断是否真的有变化，没有变化就不刷新
 */
static const MenuPageHooks g_demo_view_hooks = {
    demo_view_enter,       /* 1: 进入页面 */
    demo_view_tick,        /* 2: 周期更新 */
    demo_view_draw,        /* 3: 首次完整绘制 */
    demo_view_event,       /* 4: 页面按键事件 */
    demo_view_can_leave,   /* 5: 退出许可检查 */
    demo_view_leave,       /* 6: 离开页面 */
    NULL,                  /* 7: 无额外业务上下文 */
    70U,                   /* 8: 实时区域高度 */
    demo_view_update,      /* 9: 局部差分刷新 */
    demo_view_is_dirty     /* 10: 脏检查 */
};

/* 旋钮只更新目标值。后台串行提交，成功/失败通过状态条反馈。 */
/*********************************************************************************************************************
 * @brief  : 读取摄像头曝光目标值
 * @param  : user  业务上下文地址
 * @param  : value  菜单值容器
 * @return : true 表示读取成功，false 表示参数地址无效
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static bool camera_read_exposure(void *user, MenuValue *value)
{
    (void)user;
    if (NULL == value) return false;
    value->type = MENU_VALUE_U16;
    value->as.u16 = g_camera_exposure;
    return true;
}

/*********************************************************************************************************************
 * @brief  : 校验并提交摄像头曝光目标值
 * @param  : user  业务上下文地址
 * @param  : value  菜单值容器
 * @return : true 表示已接受，false 表示校验失败或摄像头不可用
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static bool camera_write_exposure(void *user, const MenuValue *value)
{
    uint16_t next;
    (void)user;
    if ((NULL == value) || (MENU_VALUE_U16 != value->type)) return false;
    next = value->as.u16;
    if ((0U == g_camera_initialized) || (next < 1U) || (next > 4095U))
    {
        g_camera_write_error |= CAMERA_WRITE_EXPOSURE;
        return false;
    }
    g_camera_exposure = next;
    g_camera_write_pending |= CAMERA_WRITE_EXPOSURE;
    return true;
}

/*********************************************************************************************************************
 * @brief  : 读取摄像头模拟增益目标值
 * @param  : user  业务上下文地址
 * @param  : value  菜单值容器
 * @return : true 表示读取成功，false 表示参数地址无效
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static bool camera_read_gain(void *user, MenuValue *value)
{
    (void)user;
    if (NULL == value) return false;
    value->type = MENU_VALUE_U16;
    value->as.u16 = g_camera_gain;
    return true;
}

/*********************************************************************************************************************
 * @brief  : 校验并提交摄像头模拟增益目标值
 * @param  : user  业务上下文地址
 * @param  : value  菜单值容器
 * @return : true 表示已接受，false 表示校验失败或摄像头不可用
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static bool camera_write_gain(void *user, const MenuValue *value)
{
    uint16_t next;
    (void)user;
    if ((NULL == value) || (MENU_VALUE_U16 != value->type)) return false;
    next = value->as.u16;
    if ((0U == g_camera_initialized) || (next < 16U) || (next > 64U))
    {
        g_camera_write_error |= CAMERA_WRITE_GAIN;
        return false;
    }
    g_camera_gain = next;
    g_camera_write_pending |= CAMERA_WRITE_GAIN;
    return true;
}

/* 独立于当前页运行：离页仍收尾，Freeze 不停止通信或断流检查。 */
/*********************************************************************************************************************
 * @brief  : 维护摄像头帧状态和后台参数写入事务
 * @param  : now_ms  当前毫秒时间戳
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void camera_service(uint32_t now_ms)
{
    uint32_t frame = mt9v03x_frame_count;
    uint8_t next;
    mt9v03x_write_status_enum result;
    if (!g_camera_initialized) return;

    if (frame != g_camera_monitor_frame)
    {
        g_camera_monitor_frame = frame;
        g_camera_last_frame_ms = now_ms;
    }
    g_camera_stream_lost =
        ((now_ms - g_camera_last_frame_ms) >= CAMERA_FRAME_TIMEOUT_MS);

    if (0U != g_camera_write_active)
    {
        result = mt9v03x_write_poll(now_ms);
        if (MT9V03X_WRITE_BUSY == result) return;
        if (MT9V03X_WRITE_SUCCESS == result)
            g_camera_write_error &= (uint8_t)~g_camera_write_active;
        else
        {
            g_camera_write_error |= g_camera_write_active;
            /* 出错后留出静默间隔丢弃迟到回复，不自动重试失败目标。 */
            g_camera_last_write_ms = now_ms;
        }
        /* 只完成已提交事务；期间新旋转产生的 pending 必须保留。 */
        g_camera_write_active = 0U;
        return;
    }
    if ((0U == g_camera_write_pending) ||
        ((now_ms - g_camera_last_write_ms) < CAMERA_WRITE_INTERVAL_MS)) return;

    next = (CAMERA_WRITE_EXPOSURE == g_camera_write_last) ?
           CAMERA_WRITE_GAIN : CAMERA_WRITE_EXPOSURE;
    if (0U == (g_camera_write_pending & next)) next = g_camera_write_last;
    if (0U != mt9v03x_write_start(
            (CAMERA_WRITE_GAIN == next) ? MT9V03X_WRITE_REGISTER : MT9V03X_WRITE_EXPOSURE,
            CAMERA_ANALOG_GAIN_REG, (CAMERA_WRITE_GAIN == next) ? g_camera_gain : g_camera_exposure, now_ms))
        return;
    g_camera_write_pending &= (uint8_t)~next;
    g_camera_write_active = next;
    g_camera_write_last = next;
    g_camera_last_write_ms = now_ms;
}

/* 状态条在图像外：CAM=DMA采集帧率，LCD=实际执行预览绘图的帧率。
 * LOST 覆盖 RUN/HOLD；写参错误独立保留，不被另一项成功写入清除。
 * 参数显示是目标值，WAIT/ERR 时不能视作生效；无寄存器实测回读。 */
/*********************************************************************************************************************
 * @brief  : 格式化摄像头页面状态栏文本
 * @param  : text  文本缓冲区或待绘制文本
 * @param  : size  输出缓冲区容量
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void camera_format_status(char *text, uint16_t size)
{
    (void)snprintf(text, size, "CAM %3lu LCD %2lu %-4s %-4s",
                   (unsigned long)(g_camera_stream_lost ? 0U : g_camera_display_fps),
                   (unsigned long)g_camera_preview_measured_fps,
                   g_camera_stream_lost ? "LOST" : (g_camera_freeze ? "HOLD" : "RUN"),
                   !g_camera_initialized ? "INIT" : (g_camera_write_error ? "ERR" :
                    ((g_camera_write_pending || g_camera_write_active) ? "WAIT" : "OK")));
}

/*********************************************************************************************************************
 * @brief  : 按字符差分绘制摄像头状态栏
 * @param  : canvas  自定义视图画布
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void camera_draw_status(MenuCanvas *canvas)
{
    char text[30];
    char segment[30];
    uint16_t i = 0U;
    uint16_t start;
    uint16_t count;
    uint16_t old_length = (uint16_t)strlen(g_camera_last_status);
    camera_format_status(text, sizeof(text));
    /* 定宽字段用空格覆盖缩短的数字，不先清底色，避免文字闪烁。 */
    while ('\0' != text[i])
    {
        if ((i < old_length) && (text[i] == g_camera_last_status[i]))
        {
            ++i;
            continue;
        }
        start = i;
        count = 0U;
        while (('\0' != text[i]) && ((i >= old_length) || (text[i] != g_camera_last_status[i])))
            segment[count++] = text[i++];
        segment[count] = '\0';
        menu_canvas_text(canvas, (uint16_t)(4U + start * TC264_ITEM_TEXT_WIDTH), (uint16_t)(CAMERA_VIEW_HEIGHT + 4U), segment);
    }
    (void)snprintf(g_camera_last_status, sizeof(g_camera_last_status), "%s", text);
}

/*********************************************************************************************************************
 * @brief  : 进入摄像头页面时初始化预览统计
 * @param  : user  业务上下文地址
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void camera_view_enter(void *user)
{
    (void)user;
    g_camera_frame_count = mt9v03x_frame_count;
    g_camera_rendered_frame = g_camera_frame_count;
    g_camera_fps_window_start_ms = tc264_menu_now_ms();
    g_camera_fps_window_frames = 0U;
    g_camera_display_fps = 0U;
    g_camera_preview_frames = 0U;
    g_camera_preview_measured_fps = 0U;
    g_camera_freeze = false; /* 重新进页恢复实时预览，曝光/增益等 RAM 参数保留 */
    g_camera_image_pending = 1U;
    g_camera_last_preview_ms = tc264_menu_now_ms();
    g_camera_last_status[0] = '\0';
}

/*********************************************************************************************************************
 * @brief  : 统计摄像头帧率并调度预览刷新
 * @param  : user  业务上下文地址
 * @param  : now_ms  当前毫秒时间戳
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void camera_view_tick(void *user, uint32_t now_ms)
{
    uint32_t source_frame_count;
    uint32_t new_frames;
    (void)user;
    source_frame_count = mt9v03x_frame_count;
    if (source_frame_count != g_camera_frame_count)
    {
        new_frames = source_frame_count - g_camera_frame_count;
        mt9v03x_finish_flag = 0U;
        g_camera_frame_count = source_frame_count;
        g_camera_fps_window_frames += new_frames;
    }
    if ((now_ms - g_camera_fps_window_start_ms) >= 1000U)
    {
        uint32_t elapsed_ms = now_ms - g_camera_fps_window_start_ms;
        /* 使用 DMA 完整帧增量，即使 LCD 跳过中间帧也不会漏计采集 FPS。 */
        g_camera_display_fps = (g_camera_fps_window_frames * 1000U) /
                               ((0U == elapsed_ms) ? 1U : elapsed_ms);
        g_camera_preview_measured_fps = (g_camera_preview_frames * 1000U) /
                                        ((0U == elapsed_ms) ? 1U : elapsed_ms);
        g_camera_preview_frames = 0U;
        g_camera_fps_window_start_ms = now_ms;
        g_camera_fps_window_frames = 0U;
    }
    /* 仅限速 LCD，不修改摄像头 FPS，也不关 DMA/VSYNC 中断。
     * Freeze 保留屏幕上最后一幅图；冻结时修改阈值须解除冻结后才显示。 */
    g_camera_image_pending = (uint8_t)(g_camera_initialized && !g_camera_freeze &&
        (g_camera_rendered_frame != g_camera_frame_count) &&
        ((now_ms - g_camera_last_preview_ms) >=
         ((1000U + g_camera_preview_fps - 1U) / g_camera_preview_fps)));
}

/*********************************************************************************************************************
 * @brief  : 将摄像头快照绘制到菜单视图区
 * @param  : canvas  自定义视图画布
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void camera_draw_image(MenuCanvas *canvas)
{
    g_camera_last_preview_ms = tc264_menu_now_ms();
    ips200_show_gray_image(canvas->x, canvas->y, mt9v03x_image_snapshot[mt9v03x_image_snapshot_index][0],
                           MT9V03X_W, MT9V03X_H, CAMERA_VIEW_WIDTH, CAMERA_VIEW_HEIGHT,
                           g_camera_binary ? g_camera_threshold : 0U);
    ++g_camera_preview_frames;
    g_camera_rendered_frame = g_camera_frame_count;
    g_camera_image_pending = 0U;
}

/*********************************************************************************************************************
 * @brief  : 完整绘制摄像头页面视图区
 * @param  : user  业务上下文地址
 * @param  : canvas  自定义视图画布
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void camera_view_draw(void *user, MenuCanvas *canvas)
{
    (void)user;
    if (NULL == canvas) return;
    if (g_camera_initialized) camera_draw_image(canvas);
    else menu_canvas_text(canvas, 4U, 20U, "Camera init failed");
    g_camera_last_status[0] = '\0';
    camera_draw_status(canvas);
}

/*********************************************************************************************************************
 * @brief  : 局部更新摄像头页面视图区
 * @param  : user  业务上下文地址
 * @param  : canvas  自定义视图画布
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void camera_view_update(void *user, MenuCanvas *canvas)
{
    (void)user;
    if (NULL == canvas) return;
    if (g_camera_image_pending && !g_camera_freeze && g_camera_initialized)
        camera_draw_image(canvas);
    camera_draw_status(canvas);
}

/*********************************************************************************************************************
 * @brief  : 判断摄像头页面是否需要刷新
 * @param  : user  业务上下文地址
 * @return : true 表示需要刷新，false 表示无需刷新
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static bool camera_view_is_dirty(void *user)
{
    char text[30];
    (void)user;
    camera_format_status(text, sizeof(text));
    return (0U != g_camera_image_pending) ||
           (0 != strcmp(text, g_camera_last_status));
}

/* 摄像头实时页：图像在上方，参数列表由菜单核心自动放在下方。 */
static const MenuPageHooks g_camera_view_hooks = {
    camera_view_enter,     /* 1: 进入页面 */
    camera_view_tick,      /* 2: 检查新帧 */
    camera_view_draw,      /* 3: 首次绘制图像 */
    NULL,                  /* 4: 使用菜单默认按键处理 */
    NULL,                  /* 5: 随时允许退出 */
    NULL,                  /* 6: 无额外退出清理 */
    NULL,                  /* 7: 无额外业务上下文 */
    CAMERA_PANEL_HEIGHT,   /* 8: 图像 + 独立状态条，下方参数自动滚动 */
    camera_view_update,    /* 9: 新帧时局部刷新图像 */
    camera_view_is_dirty   /* 10: 按帧判断是否需要刷新 */
};

/*********************************************************************************************************************
 * @brief  : 获取菜单示例根页面
 * @param  : 无
 * @return : 示例根页面地址
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
const MenuPage *menu_demo_root_page(void)
{
    return &g_demo_root_page;
}

/*********************************************************************************************************************
 * @brief  : 获取计数演示动作次数
 * @param  : 无
 * @return : 计数演示动作次数
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
uint32_t menu_demo_action_count(void)
{
    return g_demo_actions;
}

/*********************************************************************************************************************
 * @brief  : 获取实时状态演示运行标志
 * @param  : 无
 * @return : 1 表示运行中，0 表示已停止
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
uint8_t menu_demo_is_running(void)
{
    return g_demo_running;
}

/*********************************************************************************************************************
 * @brief  : 获取实时状态演示进度
 * @param  : 无
 * @return : 当前进度，范围 0~100
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
uint8_t menu_demo_progress(void)
{
    return g_demo_progress;
}

/*********************************************************************************************************************
 * @brief  : 获取保存演示标志
 * @param  : 无
 * @return : 1 表示保存动作已执行，0 表示未执行
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
uint8_t menu_demo_is_saved(void)
{
    return g_demo_saved;
}

/* ============================================================================
 * 运行时实现区：编码器采样、显示适配和菜单任务调度
 * ========================================================================== */

static const int8_t g_menu_encoder_transition[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
};

static volatile int g_menu_encoder_pending_steps;
static int g_menu_encoder_raw_accum;
static uint8_t g_menu_encoder_prev_state;
static uint8_t g_menu_encoder_sw_raw_pressed;
static uint8_t g_menu_encoder_sw_stable_pressed;
static uint8_t g_menu_encoder_sw_long_sent;
static uint32_t g_menu_encoder_sw_changed_ms;
static uint32_t g_menu_encoder_sw_pressed_ms;
static uint32_t g_menu_last_tick_ms;

/*********************************************************************************************************************
 * @brief  : 读取旋转编码器 A/B 相状态
 * @param  : 无
 * @return : A/B 相组合状态，范围 0~3
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static uint8_t tc264_encoder_read_state(void)
{
    uint8_t a = (uint8_t)gpio_get_level(MENU_ENCODER_A_PIN);
    uint8_t b = (uint8_t)gpio_get_level(MENU_ENCODER_B_PIN);
    return (uint8_t)((a << 1) | b);
}

/*********************************************************************************************************************
 * @brief  : 初始化旋转编码器输入和按键状态
 * @param  : 无
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void tc264_encoder_init(void)
{
    uint32_t now_ms = tc264_menu_now_ms();

    gpio_init(MENU_ENCODER_A_PIN, GPI, GPIO_HIGH, GPI_PULL_UP);
    gpio_init(MENU_ENCODER_B_PIN, GPI, GPIO_HIGH, GPI_PULL_UP);
    gpio_init(MENU_ENCODER_SW_PIN, GPI, GPIO_HIGH, GPI_PULL_UP);
    g_menu_encoder_pending_steps = 0;
    g_menu_encoder_raw_accum = 0;
    g_menu_encoder_prev_state = tc264_encoder_read_state();
    g_menu_encoder_sw_raw_pressed =
        (GPIO_LOW == gpio_get_level(MENU_ENCODER_SW_PIN)) ? 1U : 0U;
    g_menu_encoder_sw_stable_pressed = g_menu_encoder_sw_raw_pressed;
    g_menu_encoder_sw_long_sent = 0U;
    g_menu_encoder_sw_changed_ms = now_ms;
    g_menu_encoder_sw_pressed_ms = now_ms;
}

/*********************************************************************************************************************
 * @brief  : 在中断周期中采样旋转编码器
 * @param  : 无
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_app_encoder_sample(void)
{
    uint8_t current_state = tc264_encoder_read_state();
    uint8_t transition_index;
    int8_t delta;

    if (current_state == g_menu_encoder_prev_state) return;

    transition_index = (uint8_t)((g_menu_encoder_prev_state << 2) | current_state);
    delta = g_menu_encoder_transition[transition_index];
    g_menu_encoder_prev_state = current_state;
    if (0 == delta) return;

    g_menu_encoder_raw_accum += (int)(delta * MENU_ENCODER_DIR_SIGN);
    if (g_menu_encoder_raw_accum >= MENU_ENCODER_COUNTS_PER_STEP)
    {
        if (g_menu_encoder_pending_steps < MENU_ENCODER_PENDING_LIMIT)
            ++g_menu_encoder_pending_steps;
        g_menu_encoder_raw_accum = 0;
    }
    else if (g_menu_encoder_raw_accum <= -MENU_ENCODER_COUNTS_PER_STEP)
    {
        if (g_menu_encoder_pending_steps > -MENU_ENCODER_PENDING_LIMIT)
            --g_menu_encoder_pending_steps;
        g_menu_encoder_raw_accum = 0;
    }
}

/*********************************************************************************************************************
 * @brief  : 从编码器步进队列中取出一个步进
 * @param  : 无
 * @return : 一个步进值，1 表示正向，-1 表示反向，0 表示无步进
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static int tc264_encoder_take_step(void)
{
    uint32 irq_state = interrupt_global_disable();
    int step = 0;

    if (g_menu_encoder_pending_steps > 0)
    {
        --g_menu_encoder_pending_steps;
        step = 1;
    }
    else if (g_menu_encoder_pending_steps < 0)
    {
        ++g_menu_encoder_pending_steps;
        step = -1;
    }
    interrupt_global_enable(irq_state);
    return step;
}

/*********************************************************************************************************************
 * @brief  : 采样并解析编码器按键事件
 * @param  : now_ms  当前毫秒时间戳
 * @param  : menu_key  输出的菜单按键
 * @param  : press  输出的按键类型
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static bool tc264_encoder_button_event(uint32_t now_ms, MenuKey *menu_key, MenuKeyPress *press)
{
    uint8_t raw_pressed =
        (GPIO_LOW == gpio_get_level(MENU_ENCODER_SW_PIN)) ? 1U : 0U;

    if (raw_pressed != g_menu_encoder_sw_raw_pressed)
    {
        g_menu_encoder_sw_raw_pressed = raw_pressed;
        g_menu_encoder_sw_changed_ms = now_ms;
    }

    if ((g_menu_encoder_sw_stable_pressed != g_menu_encoder_sw_raw_pressed) &&
        ((now_ms - g_menu_encoder_sw_changed_ms) >= MENU_ENCODER_SW_DEBOUNCE_MS))
    {
        g_menu_encoder_sw_stable_pressed = g_menu_encoder_sw_raw_pressed;
        if (0U != g_menu_encoder_sw_stable_pressed)
        {
            g_menu_encoder_sw_pressed_ms = now_ms;
            g_menu_encoder_sw_long_sent = 0U;
        }
        else if (0U == g_menu_encoder_sw_long_sent)
        {
            *menu_key = MENU_KEY_OK;
            *press = MENU_KEY_SHORT;
            return true;
        }
    }

    if ((0U != g_menu_encoder_sw_stable_pressed) && (0U == g_menu_encoder_sw_long_sent) &&
        ((now_ms - g_menu_encoder_sw_pressed_ms) >= MENU_ENCODER_SW_LONG_MS))
    {
        g_menu_encoder_sw_long_sent = 1U;
        *menu_key = MENU_KEY_BACK;
        *press = MENU_KEY_LONG;
        return true;
    }
    return false;
}

static Menu g_tc264_menu;
static uint16_t g_tc264_draw_buffer[TC264_DRAW_BUFFER_WIDTH * TC264_DRAW_BUFFER_HEIGHT];
static char g_tc264_row_cache[TC264_ROW_CACHE_COUNT][MENU_MAX_TEXT * 2U];
static uint8_t g_tc264_row_cache_selected[TC264_ROW_CACHE_COUNT];
static uint8_t g_tc264_row_cache_valid[TC264_ROW_CACHE_COUNT];

/*********************************************************************************************************************
 * @brief  : 计算 ASCII 文本的像素宽度
 * @param  : text  文本缓冲区或待绘制文本
 * @param  : char_width  字符宽度
 * @return : 文本像素宽度
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static uint16_t tc264_text_width(const char *text, uint16_t char_width)
{
    uint16_t count = 0U;
    if (NULL == text) return 0U;
    while (('\0' != text[count]) && (count < (uint16_t)(TC264_DRAW_BUFFER_WIDTH / char_width)))
    {
        ++count;
    }
    return (uint16_t)(count * char_width);
}

/*********************************************************************************************************************
 * @brief  : 生成指定尺寸的 ASCII 文本像素缓冲
 * @param  : width  绘图区域宽度
 * @param  : height  绘图区域高度
 * @param  : text  文本缓冲区或待绘制文本
 * @param  : foreground  前景色
 * @param  : background  背景色
 * @param  : origin_x  文字起始横坐标
 * @param  : origin_y  文字起始纵坐标
 * @param  : char_width  字符宽度
 * @param  : char_height  字符高度
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void tc264_render_text_buffer(uint16_t width, uint16_t height, const char *text, uint16_t foreground,
                                     uint16_t background, uint16_t origin_x,
                                     uint16_t origin_y, uint16_t char_width,
                                     uint16_t char_height)
{
    uint16_t x;
    uint16_t y;
    uint16_t char_index;

    for (y = 0U; y < height; ++y)
    {
        for (x = 0U; x < width; ++x)
        {
            g_tc264_draw_buffer[(uint32_t)y * width + x] = background;
        }
    }
    if (NULL == text) return;

    for (char_index = 0U; ('\0' != text[char_index]) &&
         ((uint16_t)(origin_x + (char_index + 1U) * char_width) <= width);
         ++char_index)
    {
        uint8_t character = (uint8_t)text[char_index];
        const uint8_t *glyph;
        uint16_t glyph_x;
        uint16_t glyph_y;
        uint16_t glyph_width = char_width;
        uint16_t glyph_height = char_height;
        uint16_t glyph_origin_x = (uint16_t)(origin_x + char_index * glyph_width);

        if ((character < 32U) || (character > 126U)) character = (uint8_t)'?';
        glyph = ascii_font_8x16[character - 32U];
        for (glyph_y = 0U; glyph_y < glyph_height; ++glyph_y)
        {
            uint16_t source_y = (uint16_t)(glyph_y * 16U / glyph_height);
            uint8_t source_bit = (uint8_t)(1U << (source_y & 7U));
            for (glyph_x = 0U; glyph_x < glyph_width; ++glyph_x)
            {
                uint16_t destination_x = (uint16_t)(glyph_origin_x + glyph_x);
                uint16_t destination_y = (uint16_t)(origin_y + glyph_y);
                uint16_t source_x = (uint16_t)(glyph_x * 8U / glyph_width);
                uint8_t source_bits = glyph[source_x +
                                            ((source_y >= 8U) ? 8U : 0U)];
                if ((destination_x < width) && (destination_y < height) && (0U != (source_bits & source_bit)))
                {
                    g_tc264_draw_buffer[(uint32_t)destination_y * width + destination_x] =
                        foreground;
                }
            }
        }
    }
}

/*********************************************************************************************************************
 * @brief  : 清屏并使菜单行缓存失效
 * @param  : user  业务上下文地址
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void tc264_clear(void *user)
{
    uint16_t i;
    (void)user;
    for (i = 0U; i < TC264_ROW_CACHE_COUNT; ++i) g_tc264_row_cache_valid[i] = 0U;
    ips200_set_color(RGB565_BLACK, RGB565_WHITE);
    ips200_clear();
}
/*********************************************************************************************************************
 * @brief  : 清除显示器上的矩形区域
 * @param  : user  业务上下文地址
 * @param  : x  横坐标
 * @param  : y  纵坐标
 * @param  : w  宽度
 * @param  : h  高度
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void tc264_clear_region(void *user, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint16_t col;
    uint16_t i;
    (void)user;
    if ((0U == w) || (0U == h) || (w > TC264_DRAW_BUFFER_WIDTH) || (x + w > 240U) || (y + h > 320U))
    {
        return;
    }
    if (0U == y)
    {
        for (i = 0U; i < TC264_ROW_CACHE_COUNT; ++i) g_tc264_row_cache_valid[i] = 0U;
    }
    for (col = 0U; col < w; ++col)
    {
        g_tc264_draw_buffer[col] = RGB565_WHITE;
    }
    /* A one-row source is repeated by the IPS200 image scaler. */
    ips200_show_rgb565_image(x, y, g_tc264_draw_buffer, w, 1U, w, h, 0U);
}
/*********************************************************************************************************************
 * @brief  : 绘制菜单文字并处理标题分隔线
 * @param  : user  业务上下文地址
 * @param  : x  横坐标
 * @param  : y  纵坐标
 * @param  : text  文本缓冲区或待绘制文本
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void tc264_text(void *user, uint16_t x, uint16_t y, const char *text)
{
    uint16_t width;
    uint16_t height;
    uint16_t char_width;
    uint16_t char_height;
    uint16_t foreground;
    bool is_title = (10U == y);
    (void)user;
    if ((NULL == text) || (x >= TC264_DRAW_BUFFER_WIDTH) || (y >= 320U))
    {
        return;
    }
    char_width = is_title ? TC264_TITLE_TEXT_WIDTH : TC264_ITEM_TEXT_WIDTH;
    char_height = is_title ? TC264_TITLE_TEXT_HEIGHT : TC264_ITEM_TEXT_HEIGHT;
    foreground = is_title ? RGB565_BLUE : RGB565_BLACK;
    height = char_height;
    width = tc264_text_width(text, char_width);
    if (width > (uint16_t)(TC264_DRAW_BUFFER_WIDTH - x))
    {
        width = (uint16_t)(TC264_DRAW_BUFFER_WIDTH - x);
        width = (uint16_t)(width - (width % char_width));
    }
    if (0U == width) return;
    if (height > (uint16_t)(320U - y)) height = (uint16_t)(320U - y);
    if (0U == height) return;
    tc264_render_text_buffer(width, height, text, foreground, RGB565_WHITE, 0U, 0U, char_width, char_height);
    ips200_show_rgb565_image(x, y, g_tc264_draw_buffer, width, height, width, height, 0U);
    if (is_title && (height + y + 4U < 320U))
    {
        ips200_draw_line(0U, (uint16_t)(y + height + 4U), 239U, (uint16_t)(y + height + 4U), RGB565_GRAY);
    }
}
/*********************************************************************************************************************
 * @brief  : 绘制菜单矩形边框
 * @param  : user  业务上下文地址
 * @param  : x  横坐标
 * @param  : y  纵坐标
 * @param  : w  宽度
 * @param  : h  高度
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void tc264_rect(void *user, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    (void)user;
    ips200_draw_line(x, y, (uint16_t)(x + w), y, RGB565_BLACK);
    ips200_draw_line(x, y, x, (uint16_t)(y + h), RGB565_BLACK);
    ips200_draw_line((uint16_t)(x + w), y, (uint16_t)(x + w), (uint16_t)(y + h), RGB565_BLACK);
    ips200_draw_line(x, (uint16_t)(y + h), (uint16_t)(x + w), (uint16_t)(y + h), RGB565_BLACK);
}
/*********************************************************************************************************************
 * @brief  : 绘制菜单实心矩形
 * @param  : user  业务上下文地址
 * @param  : x  横坐标
 * @param  : y  纵坐标
 * @param  : w  宽度
 * @param  : h  高度
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void tc264_fill_rect(void *user, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint16_t row;
    uint16_t col;
    (void)user;
    if ((0U == w) || (0U == h) || (w > TC264_DRAW_BUFFER_WIDTH) ||
        (h > TC264_DRAW_BUFFER_HEIGHT) || (x + w > 240U) || (y + h > 320U))
    {
        return;
    }
    for (row = 0U; row < h; ++row)
    {
        for (col = 0U; col < w; ++col)
        {
            g_tc264_draw_buffer[(uint32_t)row * w + col] = RGB565_BLACK;
        }
    }
    ips200_show_rgb565_image(x, y, g_tc264_draw_buffer, w, h, w, h, 0U);
}
/*********************************************************************************************************************
 * @brief  : 按行缓存差分绘制菜单项
 * @param  : user  业务上下文地址
 * @param  : x  横坐标
 * @param  : y  纵坐标
 * @param  : w  宽度
 * @param  : h  高度
 * @param  : text  文本缓冲区或待绘制文本
 * @param  : selected  是否为当前选中行
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void tc264_row(void *user, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      const char *text, bool selected)
{
    /* 用绝对屏幕行作为缓存键；不能按固定视图区起点换算，
     * 否则不同 y 的菜单行会落到同一个槽位。 */
    uint16_t row_index = (uint16_t)(y / 20U);
    uint16_t old_len = 0U;
    uint16_t new_len = 0U;
    uint16_t start = 0U;
    uint16_t end;
    char segment[MENU_MAX_TEXT * 2U];
    uint16_t background = selected ? RGB565_YELLOW : RGB565_WHITE;
    bool full_line;
    (void)user;
    if (NULL == text) text = "";
    if ((0U == w) || (0U == h) || (w > TC264_DRAW_BUFFER_WIDTH) || (h > TC264_DRAW_BUFFER_HEIGHT)) return;
    if (row_index >= TC264_ROW_CACHE_COUNT) row_index = 0U;
    while (('\0' != text[new_len]) && (new_len + 1U < sizeof(segment)) &&
           ((new_len + 1U) * 8U <= w)) ++new_len;
    if (g_tc264_row_cache_valid[row_index])
        while ('\0' != g_tc264_row_cache[row_index][old_len]) ++old_len;
    if (g_tc264_row_cache_valid[row_index] && (g_tc264_row_cache_selected[row_index] == (uint8_t)selected) &&
        (old_len == new_len) && (0 == strcmp(g_tc264_row_cache[row_index], text)))
        return;
    full_line = !g_tc264_row_cache_valid[row_index] ||
                (g_tc264_row_cache_selected[row_index] != (uint8_t)selected);
    if (full_line)
    {
        start = 0U;
        end = (old_len > new_len) ? old_len : new_len;
    }
    else
    {
        while ((start < new_len) && (start < old_len) &&
               (g_tc264_row_cache[row_index][start] == text[start])) ++start;
        end = (old_len > new_len) ? old_len : new_len;
        while ((end > start) && (end - 1U < old_len) && (end - 1U < new_len) &&
               (g_tc264_row_cache[row_index][end - 1U] == text[end - 1U])) --end;
    }
    if (end > start)
    {
        uint16_t i;
        for (i = start; i < end; ++i)
            segment[i - start] = (i < new_len) ? text[i] : ' ';
        segment[end - start] = '\0';
        tc264_render_text_buffer(full_line ? w : (uint16_t)((end - start) * 8U), h,
                                 segment, RGB565_BLACK, background, full_line ? 2U : 0U, 2U,
                                 TC264_ITEM_TEXT_WIDTH, TC264_ITEM_TEXT_HEIGHT);
        ips200_show_rgb565_image((uint16_t)(x + (full_line ? 0U : start * 8U + 2U)), y, g_tc264_draw_buffer,
                                  full_line ? w : (uint16_t)((end - start) * 8U), h,
                                  full_line ? w : (uint16_t)((end - start) * 8U), h, 0U);
    }
    (void)snprintf(g_tc264_row_cache[row_index], sizeof(g_tc264_row_cache[row_index]), "%s", text);
    g_tc264_row_cache_selected[row_index] = (uint8_t)selected;
    g_tc264_row_cache_valid[row_index] = 1U;
}
/*********************************************************************************************************************
 * @brief  : 绘制菜单底部状态栏
 * @param  : user  业务上下文地址
 * @param  : x  横坐标
 * @param  : y  纵坐标
 * @param  : w  宽度
 * @param  : h  高度
 * @param  : text  文本缓冲区或待绘制文本
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void tc264_status(void *user, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const char *text)
{
    uint16_t row;
    uint16_t col;
    uint16_t text_width;
    (void)user;
    if ((NULL == text) || (0U == w) || (0U == h) ||
        (w > TC264_DRAW_BUFFER_WIDTH) || (h > TC264_DRAW_BUFFER_HEIGHT) ||
        (x + w > 240U) || (y + h > 320U)) return;
    for (row = 0U; row < h; ++row)
    {
        for (col = 0U; col < w; ++col)
            g_tc264_draw_buffer[(uint32_t)row * w + col] = RGB565_66CCFF;
    }
    ips200_show_rgb565_image(x, y, g_tc264_draw_buffer, w, h, w, h, 0U);
    text_width = tc264_text_width(text, TC264_ITEM_TEXT_WIDTH);
    if (text_width > (uint16_t)(w - 4U)) text_width = (uint16_t)(w - 4U);
    if ((0U != text_width) && (h >= TC264_ITEM_TEXT_HEIGHT))
    {
        tc264_render_text_buffer(text_width, TC264_ITEM_TEXT_HEIGHT, text,
                                 RGB565_BLACK, RGB565_66CCFF, 0U, 0U,
                                 TC264_ITEM_TEXT_WIDTH, TC264_ITEM_TEXT_HEIGHT);
        ips200_show_rgb565_image((uint16_t)(x + 2U), (uint16_t)(y + 1U), g_tc264_draw_buffer, text_width,
                                 TC264_ITEM_TEXT_HEIGHT, text_width, TC264_ITEM_TEXT_HEIGHT, 0U);
    }
}
/*********************************************************************************************************************
 * @brief  : 绘制菜单直线
 * @param  : user  业务上下文地址
 * @param  : x0  起点横坐标
 * @param  : y0  起点纵坐标
 * @param  : x1  终点横坐标
 * @param  : y1  终点纵坐标
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void tc264_line(void *user, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{ (void)user; ips200_draw_line(x0, y0, x1, y1, RGB565_BLACK); }
/*********************************************************************************************************************
 * @brief  : 绘制菜单图像
 * @param  : user  业务上下文地址
 * @param  : x  横坐标
 * @param  : y  纵坐标
 * @param  : w  宽度
 * @param  : h  高度
 * @param  : pixels  图像像素数据地址
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void tc264_image(void *user, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void *pixels)
{
    (void)user;
    if (NULL != pixels) ips200_show_rgb565_image(x, y, (const uint16 *)pixels, w, h, w, h, 1U);
}
static const MenuDisplayOps g_tc264_display = {
    tc264_clear, tc264_clear_region, tc264_text, tc264_rect, tc264_fill_rect,
    tc264_line, tc264_image, NULL, NULL, tc264_row, tc264_status
};
/*********************************************************************************************************************
 * @brief  : 初始化菜单应用和板级外设适配
 * @param  : 无
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_app_init(void)
{
    gpio_init(MENU_BEEP_PIN, GPO, GPIO_LOW, GPO_PUSH_PULL);
    /* IPS200 在 init 内发送 MADCTL，方向必须在 init 前设置。 */
    ips200_set_dir(MENU_DISPLAY_DIR);
    ips200_init(IPS200_TYPE_SPI);
    ips200_set_font(MENU_DISPLAY_FONT);
    ips200_set_color(RGB565_BLACK, RGB565_WHITE);
#if MENU_DISPLAY_TEST_ONLY
    /* 先发最小绘图事务，避免软件 SPI 清整屏掩盖初始化结果。 */
    ips200_show_string(8U, 8U, "IPS200 TEST");
    (void)tc264_encoder_take_step;
    (void)tc264_encoder_button_event;
    return;
#endif
    /* 总钻风初始化：失败时保留菜单运行，摄像头页会显示空图像区域。 */
    g_camera_initialized = (0U == mt9v03x_init()) ? 1U : 0U;
    g_camera_monitor_frame = mt9v03x_frame_count;
    g_camera_last_frame_ms = tc264_menu_now_ms();
    g_camera_last_write_ms = g_camera_last_frame_ms - CAMERA_WRITE_INTERVAL_MS;
    tc264_encoder_init();
    g_menu_last_tick_ms = tc264_menu_now_ms() - MENU_TICK_PERIOD_MS;
    menu_init(&g_tc264_menu, menu_demo_root_page(), &g_tc264_display, NULL, &g_menu_layout);
}
/*********************************************************************************************************************
 * @brief  : 执行一次菜单应用周期任务
 * @param  : 无
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_app_task(void)
{
#if MENU_DISPLAY_TEST_ONLY
    return;
#else
    MenuKey menu_key;
    MenuKeyPress press;
    int encoder_step;
    uint8_t steps_taken = 0U;
    uint32_t now_ms = tc264_menu_now_ms();

    /* 编码器由 CCU60_CH1 的 1 kHz ISR 采样；此处只消费累计步数。 */
    while ((steps_taken < MENU_ENCODER_PENDING_LIMIT) && (0 != (encoder_step = tc264_encoder_take_step())))
    {
        ++steps_taken;
        /* Board wiring calibration: positive decoder steps map to menu up. */
        MenuEvent event = {
            (encoder_step < 0) ? MENU_KEY_DOWN : MENU_KEY_UP,
            MENU_KEY_SHORT
        };
        menu_handle_event(&g_tc264_menu, &event);
    }

    if (tc264_encoder_button_event(now_ms, &menu_key, &press))
    {
        MenuEvent event = { menu_key, press };
        menu_handle_event(&g_tc264_menu, &event);
    }
    now_ms = tc264_menu_now_ms();
    camera_service(now_ms);
    now_ms = tc264_menu_now_ms();
    if ((now_ms - g_menu_last_tick_ms) >= MENU_TICK_PERIOD_MS)
    {
        g_menu_last_tick_ms = now_ms;
        menu_tick(&g_tc264_menu, now_ms);
    }
#endif
}
