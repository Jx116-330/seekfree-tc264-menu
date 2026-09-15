/*
 * portable-menu/menu.h
 *
 * 通用嵌入式菜单的公开接口。
 *
 * 这个文件只描述“菜单是什么”和“外部如何接入”，不包含任何具体
 * 单片机、显示屏或按键驱动。因此同一份 menu.c 可以放到 TC264、TC387
 * 或其它 C99 工程中复用。应用层只需要提供显示回调、输入事件和业务
 * 页面回调即可。
 *
 * 列表项除了可编辑参数，还可以是“实时数值行”（MENU_LIVE 系列宏）：只读，
 * 由菜单核心比较原始值并只重画发生变化的那一行，普通列表页不需要写任何
 * 页面钩子就能显示实时数据。
 */
#ifndef PORTABLE_MENU_H
#define PORTABLE_MENU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 页面栈的最大深度。深度越大，Menu 对象占用的 RAM 越多。 */
#ifndef MENU_MAX_STACK
#define MENU_MAX_STACK 8U
#endif

/* 菜单内部格式化文本的最大长度，包含结尾的 '\0'。 */
#ifndef MENU_MAX_TEXT
#define MENU_MAX_TEXT 64U
#endif

/*
 * 实时数值行（MENU_ITEM_LIVE）的自动刷新缓存行数。
 *
 * 只覆盖当前屏幕可见的行即可：菜单只会为可见行做值比较和局部重画。
 * 行数越多，Menu 对象占用的 RAM 越多（每行一个指针加一个 MenuValue）。
 * 该值同时决定脏行位图的宽度，不要超过 16。
 */
#ifndef MENU_MAX_LIVE_ROWS
#define MENU_MAX_LIVE_ROWS 16U
#endif

/* 前置声明：具体定义在本文件后面，应用只需持有指针或对象即可。 */
typedef struct Menu Menu;
typedef struct MenuPage MenuPage;
typedef struct MenuItem MenuItem;
typedef struct MenuCanvas MenuCanvas;

/*
 * 菜单项的外观/交互类别。
 *
 * I8/U8/I16/U16 必须单独列出：它们绑定的是不同宽度的真实变量，不能
 * 把 uint8_t 或 uint16_t 地址误当作 uint32_t 地址写入。
 *
 * READONLY 只在重画该行时重新取值；LIVE 额外由核心每 tick 比较原始值，
 * 值变化后只重画这一行，因此普通列表页也能显示实时数据。
 */
typedef enum
{
    MENU_ITEM_SUBMENU = 0,
    MENU_ITEM_ACTION,
    MENU_ITEM_BOOL,
    MENU_ITEM_ENUM,
    MENU_ITEM_I8,
    MENU_ITEM_U8,
    MENU_ITEM_I16,
    MENU_ITEM_U16,
    MENU_ITEM_I32,
    MENU_ITEM_U32,
    MENU_ITEM_FLOAT,
    MENU_ITEM_READONLY,
    MENU_ITEM_LIVE,
    MENU_ITEM_BINDING,
    MENU_ITEM_CUSTOM
} MenuItemType;

/* 数值回调和 MenuValue 联合体使用的实际数据类型。 */
typedef enum
{
    MENU_VALUE_BOOL = 0,
    MENU_VALUE_I8,
    MENU_VALUE_U8,
    MENU_VALUE_I16,
    MENU_VALUE_U16,
    MENU_VALUE_I32,
    MENU_VALUE_U32,
    MENU_VALUE_FLOAT,
    MENU_VALUE_ENUM,
    /*
     * 仅由菜单核心用于实时数值行的缓存：这一行上一次取值失败。
     * 应用层的 read 回调不需要返回这个值，返回 false 即可。
     */
    MENU_VALUE_UNAVAILABLE
} MenuValueType;

/* 一次读取或写入操作使用的类型安全数值容器。 */
typedef struct
{
    MenuValueType type;
    union
    {
        bool boolean;
        int8_t i8;
        uint8_t u8;
        int16_t i16;
        uint16_t u16;
        int32_t i32;
        uint32_t u32;
        float f32;
        uint32_t enumeration;
    } as;
} MenuValue;

/* 返回 false 表示业务层暂时无法提供值或拒绝本次写入。 */
typedef bool (*MenuValueReadFn)(void *user, MenuValue *value);
typedef bool (*MenuValueWriteFn)(void *user, const MenuValue *value);

/*
 * 动态数值项的完整描述。
 *
 * read/write 不为空时使用回调；为空时，user 被当作对应基础类型变量
 * 的地址。steps 用于“细步/粗步/快速调整”等多档步进，enum_values 可以
 * 表示 0、2、7、42 这类不连续的实际值。
 */
typedef struct
{
    MenuValueType type;
    MenuValueReadFn read;
    MenuValueWriteFn write;
    void *user;
    double minimum;
    double maximum;
    double step;
    const double *steps;
    uint8_t step_count;
    const uint32_t *enum_values;
    const char *const *enum_labels;
    uint8_t enum_count;
    const char *unit;
    uint8_t precision;
} MenuDynamicValue;

/* 仅保留枚举标签的轻量描述，供应用层组织配置数据。 */
typedef struct
{
    const char *const *labels;
    uint8_t count;
} MenuEnumList;

/* 动作项不返回结果；失败、忙碌或进度应通过页面状态/只读项显示。 */
typedef void (*MenuActionFn)(void *user);

/* 菜单引擎理解的语义按键，不直接绑定 KEY1/KEY2 或 GPIO。 */
typedef enum
{
    MENU_KEY_UP = 0,
    MENU_KEY_DOWN,
    MENU_KEY_LEFT,
    MENU_KEY_RIGHT,
    MENU_KEY_OK,
    MENU_KEY_BACK,
    MENU_KEY_AUX1,
    MENU_KEY_AUX2
} MenuKey;

/* 同一个语义按键可区分短按、长按和自动连发。 */
typedef enum
{
    MENU_KEY_SHORT = 0,
    MENU_KEY_LONG,
    MENU_KEY_REPEAT
} MenuKeyPress;

/* 输入适配层把物理按键转换成这个事件后交给 menu_handle_event()。 */
typedef struct
{
    MenuKey key;
    MenuKeyPress press;
} MenuEvent;

/* 页面/自定义行处理事件后的返回值。 */
typedef enum
{
    MENU_EVENT_IGNORED = 0,
    MENU_EVENT_HANDLED,
    MENU_EVENT_REQUEST_BACK,
    MENU_EVENT_REQUEST_REDRAW
} MenuEventResult;

/*
 * 显示适配表。
 *
 * 所有坐标均为显示适配层使用的像素坐标。某个设备不支持图像或填充
 * 矩形时可以将对应函数置为 NULL，核心会跳过该操作。
 */
typedef struct
{
    /* 清屏、文字、基本图元和可选图像输出；NULL 表示设备不支持该能力。 */
    void (*clear)(void *user);
    /* 清除一个矩形区域；动态页面刷新时避免清整屏。 */
    void (*clear_region)(void *user, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    void (*text)(void *user, uint16_t x, uint16_t y, const char *text);
    void (*rect)(void *user, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    void (*fill_rect)(void *user, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    void (*line)(void *user, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
    void (*image)(void *user, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void *pixels);
    void (*begin)(void *user);
    void (*end)(void *user);
    /* 可选的整行绘制回调；提供后，导航时可只刷新变化的行。 */
    void (*row)(void *user, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const char *text, bool selected);
    /* 可选的状态栏绘制回调；可使用独立底色和字体颜色。 */
    void (*status)(void *user, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const char *text);
} MenuDisplayOps;

/*
 * 自定义绘图收到的画布。
 * x/y 是当前视图的左上角，绘图辅助函数会自动把相对坐标转换为屏幕
 * 坐标，避免每个业务页面重复计算偏移。
 */
struct MenuCanvas
{
    const MenuDisplayOps *display;
    void *display_user;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
};

/*
 * 页面级扩展点。
 *
 * 普通列表页可以全部置 NULL；摄像头、校准、自检等实时页面通常使用
 * enter/tick/draw/event/can_leave/leave 的组合。这里的状态机属于业务
 * 回调，菜单核心只负责按固定时机调用它们。
 */
typedef struct
{
    /* 进入页面时调用一次，可在这里申请/接管业务资源。 */
    void (*enter)(void *user);
    /* 每次 menu_tick 调用一次；必须保持非阻塞，只推进一步状态。 */
    void (*tick)(void *user, uint32_t now_ms);
    /* 绘制实时区域；列表行仍由核心负责绘制。 */
    void (*draw)(void *user, MenuCanvas *canvas);
    /* 页面优先接收按键，可实现确认、取消、向导等专用交互。 */
    MenuEventResult (*event)(void *user, const MenuEvent *event);
    /* 返回 false 时拒绝离页，例如校准或电机测试仍在运行。 */
    bool (*can_leave)(void *user);
    /* 真正离页后调用一次，用于停止输出、恢复 ISR 或释放资源。 */
    void (*leave)(void *user);
    /* 页面业务上下文，不由核心解释。 */
    void *user;
    /* 实时区域申请的高度；核心会按屏幕大小自动限幅。 */
    uint16_t view_height;
    /* 可选的动态局部更新；提供后，tick 刷新时不再自动清空整个视图。 */
    void (*update)(void *user, MenuCanvas *canvas);
    /* 可选的动态脏检查；返回 false 时 tick 不触发任何视图刷新。 */
    bool (*is_dirty)(void *user);
} MenuPageHooks;

/* 列表中的数值项：可编辑参数、只读项（MENU_VALUE）和实时数值行
 * （MENU_LIVE 系列）共用这一份描述。 */
typedef struct
{
    const char *label;               /* 行名称。 */
    MenuValueType type;              /* 变量/回调返回的实际类型。 */
    MenuValueReadFn read;            /* 动态读取；NULL 表示直接读 user 指针。 */
    MenuValueWriteFn write;          /* 动态写入；NULL 表示直接写 user 指针。 */
    void *user;                      /* 变量地址或业务上下文。 */
    double minimum;                  /* 数值下限，编辑时限幅。 */
    double maximum;                  /* 数值上限，编辑时限幅。 */
    double step;                     /* 没有 steps 数组时使用的步长。 */
    const double *steps;             /* 可选的多档步长数组。 */
    uint8_t step_count;              /* steps 数组元素数。 */
    const uint32_t *enum_values;     /* 枚举的真实值数组。 */
    const char *const *enum_labels;  /* 与 enum_values 对应的显示文本。 */
    uint8_t enum_count;              /* 枚举元素数。 */
    const char *unit;                /* 显示在数值后的单位文本。 */
    uint8_t precision;               /* 浮点显示的小数位数。 */
} MenuValueItem;

/* 列表中的一次性动作，例如 Save、Reset 或 Start。 */
typedef struct
{
    MenuActionFn action;
    void *user;
} MenuActionItem;

/* 列表中需要自行绘制和处理事件的行。 */
typedef struct
{
    void (*draw)(void *user, MenuCanvas *canvas, bool selected);
    MenuEventResult (*event)(void *user, const MenuEvent *event);
    void *user;
} MenuCustomItem;

/*
 * 一行菜单的统一存储格式。
 * type 决定 data 联合体当前使用哪一个成员；配置宏负责生成正确的
 * 静态初始化器，创建页面时不需要手写这些联合体字段。
 */
struct MenuItem
{
    const char *label;
    MenuItemType type;
    union
    {
        const MenuPage *submenu;
        MenuActionItem action;
        MenuValueItem value;
        MenuCustomItem custom;
    } data;
};

/* 页面是只读的静态配置；运行时选择位置保存在 Menu 对象和页面栈中。 */
struct MenuPage
{
    /* 静态标题和条目数组，不在运行时修改。 */
    const char *title;
    const MenuItem *items;
    uint8_t item_count;
    const MenuPageHooks *hooks;
};

/* 屏幕尺寸和列表布局参数，可按 TFT/OLED 的实际分辨率调整。 */
typedef struct
{
    uint16_t width;
    uint16_t height;
    uint16_t title_height;
    uint16_t row_height;
    uint16_t footer_height;
    uint8_t wrap_navigation;
} MenuLayout;

/*
 * 实时数值行（MENU_ITEM_LIVE）的上次刷新记录。
 *
 * item 是缓存对应的菜单项地址，换页或滚动后指针不同即视为失效；value
 * 是上次参与刷新的原始值。用原始值而不是格式化文本做比较，可以避免
 * 每 tick 都执行一次浮点 snprintf。
 */
typedef struct
{
    const MenuItem *item;
    MenuValue value;
} MenuLiveRow;

/*
 * 菜单运行时对象。
 *
 * 应用应将它作为静态变量或上层对象成员保存，不要在周期任务中创建。
 * stack_* 保存父页的返回位置，所以进入子页后父页会话不会丢失。
 */
struct Menu
{
    /* 根页和当前页。 */
    const MenuPage *root;
    const MenuPage *page;
    const MenuDisplayOps *display;
    void *display_user;
    MenuLayout layout;
    /* 页面栈：保存每一级父页以及对应的高亮/滚动位置。 */
    const MenuPage *stack_pages[MENU_MAX_STACK];
    uint8_t stack_selection[MENU_MAX_STACK];
    uint8_t stack_first_visible[MENU_MAX_STACK];
    uint8_t depth;
    uint8_t selection;
    uint8_t first_visible;
    uint8_t editing;          /* 当前是否处于数值编辑状态。 */
    uint8_t edit_step_index;  /* 当前使用的多档步进下标。 */
    uint8_t redraw_requested; /* 下一次 tick 是否需要重画。 */
    uint8_t redraw_mode;      /* 0=无，1=局部行，2=整屏，3=动态视图。 */
    uint8_t view_redraw_requested; /* 局部行刷新同时需要重画动态视图。 */
    uint8_t footer_redraw_requested; /* 局部刷新同时需要更新页脚。 */
    uint8_t redraw_old_selection;
    uint8_t redraw_old_first_visible;
    const MenuPage *redraw_page;
    const MenuPage *rendered_page; /* 当前屏幕上已经完成首帧绘制的页面。 */
    uint8_t initialized;      /* menu_init 是否已经成功完成。 */
    uint32_t now_ms;          /* 最近一次 menu_tick 提供的时间戳。 */
    /* 实时数值行缓存和脏行位图；位图的第 row 位对应第 row 个可见行。 */
    MenuLiveRow live_rows[MENU_MAX_LIVE_ROWS];
    uint16_t live_dirty_mask;
};

/*********************************************************************************************************************
 * @brief  : 初始化菜单对象并调用根页 enter 回调
 * @param  : menu          菜单运行时对象
 * @param  : root          根页面地址
 * @param  : display       显示设备适配回调表
 * @param  : display_user  传给显示回调的用户上下文
 * @param  : layout        菜单布局参数，可传 NULL 使用默认值
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_init(Menu *menu, const MenuPage *root, const MenuDisplayOps *display, void *display_user,
               const MenuLayout *layout);

/*********************************************************************************************************************
 * @brief  : 推进当前页及父页的周期任务并按需刷新屏幕
 * @param  : menu     菜单运行时对象
 * @param  : now_ms   当前毫秒时间戳
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_tick(Menu *menu, uint32_t now_ms);

/*********************************************************************************************************************
 * @brief  : 处理一个已经完成消抖和分类的语义按键事件
 * @param  : menu   菜单运行时对象
 * @param  : event  菜单语义按键事件
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_handle_event(Menu *menu, const MenuEvent *event);

/*********************************************************************************************************************
 * @brief  : 按当前菜单状态立即重画一帧
 * @param  : menu   菜单运行时对象
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_render(Menu *menu);

/*********************************************************************************************************************
 * @brief  : 请求下一次 menu_tick 执行整屏重画
 * @param  : menu   菜单运行时对象
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_request_redraw(Menu *menu);

/*********************************************************************************************************************
 * @brief  : 查询当前是否处于数值编辑状态
 * @param  : menu   菜单运行时对象
 * @return : true 表示正在编辑，false 表示浏览状态
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
bool menu_is_editing(const Menu *menu);

/*********************************************************************************************************************
 * @brief  : 获取当前页面地址
 * @param  : menu   菜单运行时对象
 * @return : 当前页面地址，参数无效时返回 NULL
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
const MenuPage *menu_current_page(const Menu *menu);

/*********************************************************************************************************************
 * @brief  : 获取当前页面的选中项下标
 * @param  : menu   菜单运行时对象
 * @return : 当前选中项下标
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
uint8_t menu_current_selection(const Menu *menu);

/*********************************************************************************************************************
 * @brief  : 在自定义视图中绘制文字
 * @param  : canvas  自定义视图画布
 * @param  : x       相对画布横坐标
 * @param  : y       相对画布纵坐标
 * @param  : text    要绘制的 ASCII 文本
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_canvas_text(MenuCanvas *canvas, uint16_t x, uint16_t y, const char *text);

/*********************************************************************************************************************
 * @brief  : 在自定义视图中绘制矩形边框
 * @param  : canvas  自定义视图画布
 * @param  : x       相对画布横坐标
 * @param  : y       相对画布纵坐标
 * @param  : w       矩形宽度
 * @param  : h       矩形高度
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_canvas_rect(MenuCanvas *canvas, uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/*********************************************************************************************************************
 * @brief  : 在自定义视图中绘制实心矩形
 * @param  : canvas  自定义视图画布
 * @param  : x       相对画布横坐标
 * @param  : y       相对画布纵坐标
 * @param  : w       矩形宽度
 * @param  : h       矩形高度
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_canvas_fill_rect(MenuCanvas *canvas, uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/*********************************************************************************************************************
 * @brief  : 在自定义视图中绘制直线
 * @param  : canvas  自定义视图画布
 * @param  : x0      起点横坐标
 * @param  : y0      起点纵坐标
 * @param  : x1      终点横坐标
 * @param  : y1      终点纵坐标
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_canvas_line(MenuCanvas *canvas, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/*********************************************************************************************************************
 * @brief  : 在自定义视图中绘制图像
 * @param  : canvas  自定义视图画布
 * @param  : x       相对画布横坐标
 * @param  : y       相对画布纵坐标
 * @param  : w       图像宽度
 * @param  : h       图像高度
 * @param  : pixels   图像像素数据地址
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_canvas_image(MenuCanvas *canvas, uint16_t x, uint16_t y,
                       uint16_t w, uint16_t h, const void *pixels);

/*
 * 配置宏：每个宏展开为一个静态 MenuItem 初始化器。
 *
 * 给初学者的快速理解：
 *   - MENU_PAGE_BEGIN/END 包住一整页菜单；中间一行就是一个菜单项。
 *   - 第一个字符串是屏幕显示名称，&变量是菜单要读写的真实变量。
 *   - min/max 是数值范围，step 是单步大小；*_STEPS 允许每个参数拥有
 *     自己独立的多档步长数组，短按编码器可在这些档位之间切换。
 *   - MENU_SUBMENU 进入子页面，MENU_ACTION 执行动作，MENU_ENUM 显示枚举。
 *   - MENU_LIVE_VAR 直接绑定变量、MENU_LIVE 用 read 回调取值、
 *     MENU_LIVE_ENUM 显示枚举标签：三者都是实时数值行，只读，值变化时
 *     核心自动重画这一行，不需要写页面钩子，也不需要应用自己请求重绘。
 *   - MENU_PAGE_END 的 hooks 参数传 NULL 表示普通页；传入
 *     MenuPageHooks 地址表示实时页，可增加 tick、局部刷新和专用按键处理。
 *
 * 推荐的页面写法是“一页一块、一项一行”，例如：
 *
 * MENU_PAGE_BEGIN(settings_page, "Settings")
 *     MENU_U8_STEPS("Threshold", &threshold, 0, 255, threshold_steps, 3U),
 *     MENU_ACTION("Save", save_settings)
 * MENU_PAGE_END(settings_page, "Settings", NULL);
 */
#define MENU_SUBMENU(label_, page_) \
    { (label_), MENU_ITEM_SUBMENU, { .submenu = (page_) } }

#define MENU_ACTION(label_, fn_) \
    { (label_), MENU_ITEM_ACTION, { .action = { (fn_), NULL } } }

#define MENU_ACTION_CTX(label_, fn_, ctx_) \
    { (label_), MENU_ITEM_ACTION, { .action = { (fn_), (ctx_) } } }

/* 直接绑定 bool 变量，OK 进入编辑，旋钮旋转切换 ON/OFF。 */
#define MENU_BOOL(label_, ptr_) \
    { (label_), MENU_ITEM_BOOL, { .value = { (label_), MENU_VALUE_BOOL, NULL, NULL, \
      (ptr_), 0.0, 1.0, 1.0, NULL, 0U, NULL, NULL, 0U, NULL, 0U } } }

#define MENU_I32(label_, ptr_, min_, max_, step_) \
    { (label_), MENU_ITEM_I32, { .value = { (label_), MENU_VALUE_I32, NULL, NULL, \
      (ptr_), (min_), (max_), (step_), NULL, 0U, NULL, NULL, 0U, NULL, 0U } } }

#define MENU_U32(label_, ptr_, min_, max_, step_) \
    { (label_), MENU_ITEM_U32, { .value = { (label_), MENU_VALUE_U32, NULL, NULL, \
      (ptr_), (min_), (max_), (step_), NULL, 0U, NULL, NULL, 0U, NULL, 0U } } }

#define MENU_FLOAT(label_, ptr_, min_, max_, step_) \
    { (label_), MENU_ITEM_FLOAT, { .value = { (label_), MENU_VALUE_FLOAT, NULL, NULL, \
      (ptr_), (min_), (max_), (step_), NULL, 0U, NULL, NULL, 0U, NULL, 2U } } }

#define MENU_FLOAT_STEPS(label_, ptr_, min_, max_, steps_, count_) \
    { (label_), MENU_ITEM_FLOAT, { .value = { (label_), MENU_VALUE_FLOAT, NULL, NULL, \
      (ptr_), (min_), (max_), 0.0, (steps_), (count_), NULL, NULL, 0U, NULL, 2U } } }

#define MENU_I8_STEPS(label_, ptr_, min_, max_, steps_, count_) \
    { (label_), MENU_ITEM_I8, { .value = { (label_), MENU_VALUE_I8, NULL, NULL, \
      (ptr_), (min_), (max_), 0.0, (steps_), (count_), NULL, NULL, 0U, NULL, 0U } } }

#define MENU_U8_STEPS(label_, ptr_, min_, max_, steps_, count_) \
    { (label_), MENU_ITEM_U8, { .value = { (label_), MENU_VALUE_U8, NULL, NULL, \
      (ptr_), (min_), (max_), 0.0, (steps_), (count_), NULL, NULL, 0U, NULL, 0U } } }

#define MENU_I16_STEPS(label_, ptr_, min_, max_, steps_, count_) \
    { (label_), MENU_ITEM_I16, { .value = { (label_), MENU_VALUE_I16, NULL, NULL, \
      (ptr_), (min_), (max_), 0.0, (steps_), (count_), NULL, NULL, 0U, NULL, 0U } } }

#define MENU_U16_STEPS(label_, ptr_, min_, max_, steps_, count_) \
    { (label_), MENU_ITEM_U16, { .value = { (label_), MENU_VALUE_U16, NULL, NULL, \
      (ptr_), (min_), (max_), 0.0, (steps_), (count_), NULL, NULL, 0U, NULL, 0U } } }

#define MENU_I32_STEPS(label_, ptr_, min_, max_, steps_, count_) \
    { (label_), MENU_ITEM_I32, { .value = { (label_), MENU_VALUE_I32, NULL, NULL, \
      (ptr_), (min_), (max_), 0.0, (steps_), (count_), NULL, NULL, 0U, NULL, 0U } } }

#define MENU_U32_STEPS(label_, ptr_, min_, max_, steps_, count_) \
    { (label_), MENU_ITEM_U32, { .value = { (label_), MENU_VALUE_U32, NULL, NULL, \
      (ptr_), (min_), (max_), 0.0, (steps_), (count_), NULL, NULL, 0U, NULL, 0U } } }

/* 枚举值可以是不连续的实际值；显示文本来自 enum_labels。 */
#define MENU_ENUM(label_, ptr_, values_, labels_, count_) \
    { (label_), MENU_ITEM_ENUM, { .value = { (label_), MENU_VALUE_ENUM, NULL, NULL, \
      (ptr_), 0.0, 0.0, 1.0, NULL, 0U, (values_), (labels_), (count_), NULL, 0U } } }

#define MENU_I8(label_, ptr_, min_, max_, step_) \
    { (label_), MENU_ITEM_I8, { .value = { (label_), MENU_VALUE_I8, NULL, NULL, \
      (ptr_), (min_), (max_), (step_), NULL, 0U, NULL, NULL, 0U, NULL, 0U } } }

#define MENU_U8(label_, ptr_, min_, max_, step_) \
    { (label_), MENU_ITEM_U8, { .value = { (label_), MENU_VALUE_U8, NULL, NULL, \
      (ptr_), (min_), (max_), (step_), NULL, 0U, NULL, NULL, 0U, NULL, 0U } } }

#define MENU_I16(label_, ptr_, min_, max_, step_) \
    { (label_), MENU_ITEM_I16, { .value = { (label_), MENU_VALUE_I16, NULL, NULL, \
      (ptr_), (min_), (max_), (step_), NULL, 0U, NULL, NULL, 0U, NULL, 0U } } }

#define MENU_U16(label_, ptr_, min_, max_, step_) \
    { (label_), MENU_ITEM_U16, { .value = { (label_), MENU_VALUE_U16, NULL, NULL, \
      (ptr_), (min_), (max_), (step_), NULL, 0U, NULL, NULL, 0U, NULL, 0U } } }

/* 只读项使用 read 回调；write 即使提供也不会被菜单编辑。 */
#define MENU_VALUE(label_, type_, read_, write_, ctx_, min_, max_, step_) \
    { (label_), MENU_ITEM_READONLY, { .value = { (label_), (type_), (read_), (write_), \
      (ctx_), (min_), (max_), (step_), NULL, 0U, NULL, NULL, 0U, NULL, 2U } } }

/*
 * 实时数值项：只读，并且由菜单核心在每个 tick 比较原始值，值变化时只
 * 重画这一行，其它行不重画。两种写法：
 *   MENU_LIVE_VAR  直接绑定变量地址，例如 &g_gyro_z_dps
 *   MENU_LIVE      用 read 回调取值，适合需要换算或同步的场合
 * precision 只在浮点类型时生效，unit 是数值后面的单位文本，可为 NULL。
 * read 回调每个 tick 都会被调用，必须保持非阻塞，建议只返回已经采样好的
 * 快照；绑定值请先量化到显示分辨率，否则屏幕上看不出的变化也会触发重画。
 */
#define MENU_LIVE_VAR(label_, type_, ptr_, precision_, unit_) \
    { (label_), MENU_ITEM_LIVE, { .value = { (label_), (type_), NULL, NULL, \
      (ptr_), 0.0, 0.0, 1.0, NULL, 0U, NULL, NULL, 0U, (unit_), (precision_) } } }

#define MENU_LIVE(label_, type_, read_, ctx_, precision_, unit_) \
    { (label_), MENU_ITEM_LIVE, { .value = { (label_), (type_), (read_), NULL, \
      (ctx_), 0.0, 0.0, 1.0, NULL, 0U, NULL, NULL, 0U, (unit_), (precision_) } } }

/* 枚举型实时值：显示 enum_labels 中与实际值匹配的那一项。 */
#define MENU_LIVE_ENUM(label_, ptr_, values_, labels_, count_) \
    { (label_), MENU_ITEM_LIVE, { .value = { (label_), MENU_VALUE_ENUM, NULL, NULL, \
      (ptr_), 0.0, 0.0, 1.0, NULL, 0U, (values_), (labels_), (count_), NULL, 0U } } }

#define MENU_BIND(label_, type_, read_, write_, ctx_, min_, max_, step_) \
    { (label_), MENU_ITEM_BINDING, { .value = { (label_), (type_), (read_), (write_), \
      (ctx_), (min_), (max_), (step_), NULL, 0U, NULL, NULL, 0U, NULL, 2U } } }

#define MENU_BIND_STEPS(label_, type_, read_, write_, ctx_, min_, max_, steps_, count_) \
    { (label_), MENU_ITEM_BINDING, { .value = { (label_), (type_), (read_), (write_), \
      (ctx_), (min_), (max_), 0.0, (steps_), (count_), NULL, NULL, 0U, NULL, 2U } } }

#define MENU_CUSTOM(label_, draw_, event_, ctx_) \
    { (label_), MENU_ITEM_CUSTOM, { .custom = { (draw_), (event_), (ctx_) } } }

/*
 * 页面配置宏的 BEGIN/END 必须成对使用。hooks 为 NULL 表示普通列表页；
 * 传入 MenuPageHooks 地址后，页面就可以拥有实时视图和状态机。
 */
#define MENU_PAGE_BEGIN(name_, title_) \
    static const MenuItem name_##_items[] = {

#define MENU_PAGE_END(name_, title_, hooks_) \
    }; \
    static const MenuPage name_ = { (title_), name_##_items, \
        (uint8_t)(sizeof(name_##_items) / sizeof(name_##_items[0])), (hooks_) }

#ifdef __cplusplus
}
#endif

#endif
