/*
 * README 教程「自己做一个实时页（从零开始）」的示例代码 + 行为断言。
 *
 * 本文件中 ===== README 示例开始/结束 ===== 之间的代码，必须与
 * firmware/code/menu/README.md 同一小节里的代码块逐行一致（run.ps1 会自动比对，
 * 不一致直接报错），这样教程里的代码不会腐烂。
 *
 * 运行：在本目录执行 .\run.ps1。
 * 不要拷进 ADS 工程目录，原因见 menu_core_test.c 头部说明。
 */
#include "menu.h"

#include <stdio.h>
#include <string.h>

/* ===== README 示例开始 ===== */
/* 本页业务变量：真实项目里由采样 / PID 更新，这里按时间派生 */
static uint16_t g_motor_rpm;
static uint8_t  g_motor_duty;
static uint16_t g_motor_target = 50U;
/* “屏幕留底”：这两份内存里存的就是屏幕上此刻显示的那两串字。
 * is_dirty 拿它比较来判断要不要重画，update 画完顺手把它更新成新文字。 */
static char g_motor_rpm_text[8];
static char g_motor_duty_text[8];

/* 把两个值翻译成“屏幕上要显示的那串字”，并顺手更新“屏幕留底”：
 * g_motor_rpm_text / g_motor_duty_text 存的就是屏幕上此刻显示的文字，
 * is_dirty 拿它做比较，决定这一帧要不要重画。
 *
 * 格式串写成定宽（%4u / %3u%%）：位数不够用空格补齐，每次写出去的都是
 * 同样宽的字符格，屏幕上直接覆盖旧字，不需要字符差分，也不会留残影。
 *
 * 只在真要画的时候调它（draw / update）。is_dirty 里必须用临时缓冲区格式化
 * 再比较——那里如果也调这个函数，留底会被提前改掉，比较永远相等，
 * 屏幕就再也不会刷新。 */
static void motor_format_values(void)
{
    /* (void) 是显式忽略 snprintf 的返回值；sizeof(数组) 让缓冲区改大小时不用改这里 */
    (void)snprintf(g_motor_rpm_text, sizeof(g_motor_rpm_text), "%4u", (unsigned)g_motor_rpm);
    (void)snprintf(g_motor_duty_text, sizeof(g_motor_duty_text), "%3u%%", (unsigned)g_motor_duty);
}

/* 1) 周期任务：每次 menu_tick 调一次；必须非阻塞，只推进一步 */
static void motor_tick(void *user, uint32_t now_ms)
{
    (void)user;
    g_motor_rpm = (uint16_t)((now_ms / 10U) % 3000U);
    g_motor_duty = (uint8_t)((now_ms / 100U) % 101U);
}

/* 2) 首次完整绘制：实时区的静态内容（边框、标签）在这里画 */
static void motor_draw(void *user, MenuCanvas *canvas)
{
    (void)user;
    menu_canvas_rect(canvas, 0U, 0U, (uint16_t)(canvas->width - 1U), (uint16_t)(canvas->height - 1U));
    menu_canvas_text(canvas, 4U, 6U, "RPM");
    menu_canvas_text(canvas, 4U, 26U, "DUTY");
    motor_format_values();
    menu_canvas_text(canvas, 60U, 6U, g_motor_rpm_text);
    menu_canvas_text(canvas, 60U, 26U, g_motor_duty_text);
}

/* 3) 脏检查：返回 false 时核心这一帧什么都不刷 */
static bool motor_is_dirty(void *user)
{
    char text[8];
    (void)user;
    (void)snprintf(text, sizeof(text), "%4u", (unsigned)g_motor_rpm);
    if (0 != strcmp(text, g_motor_rpm_text)) return true;
    (void)snprintf(text, sizeof(text), "%3u%%", (unsigned)g_motor_duty);
    return 0 != strcmp(text, g_motor_duty_text);
}

/* 4) 局部更新：哪个字段变了就重画哪个，没变的连碰都不碰 */
static void motor_update(void *user, MenuCanvas *canvas)
{
    char text[8];
    (void)user;
    (void)snprintf(text, sizeof(text), "%4u", (unsigned)g_motor_rpm);
    if (0 != strcmp(text, g_motor_rpm_text))
    {
        (void)snprintf(g_motor_rpm_text, sizeof(g_motor_rpm_text), "%s", text);
        menu_canvas_text(canvas, 60U, 6U, text);
    }
    (void)snprintf(text, sizeof(text), "%3u%%", (unsigned)g_motor_duty);
    if (0 != strcmp(text, g_motor_duty_text))
    {
        (void)snprintf(g_motor_duty_text, sizeof(g_motor_duty_text), "%s", text);
        menu_canvas_text(canvas, 60U, 26U, text);
    }
}

/* 5) 钩子表：编号顺序不能改（这是结构体初始化，不是命名赋值） */
static const MenuPageHooks g_motor_hooks = {
    NULL,           /*  1 enter       进页面要重置状态时才写 */
    motor_tick,     /*  2 tick        必须有：核心靠它决定这一页要不要周期刷新 */
    motor_draw,     /*  3 draw        首帧画静态内容 */
    NULL,           /*  4 event       要自己接管按键时才写 */
    NULL,           /*  5 can_leave   运行中不许返回时才写 */
    NULL,           /*  6 leave       离页要停机 / 恢复中断时才写 */
    NULL,           /*  7 user        没有额外上下文 */
    60U,            /*  8 view_height 实时区高度：参数行自动排在它下面 */
    motor_update,   /*  9 update      只刷变化的部分 */
    motor_is_dirty  /* 10 is_dirty    没有变化就不刷 */
};

/* 6) 页面：上面是实时区，下面照常写参数行 */
MENU_PAGE_BEGIN(g_motor_page, "Motor")
    MENU_U16("Target", &g_motor_target, 0, 1000, 10)
MENU_PAGE_END(g_motor_page, "Motor", &g_motor_hooks);
/* ===== README 示例结束 ===== */

/* ---------------- 以下为断言用的假显示层，不属于教程内容 ---------------- */
#define LOG_MAX 32

static char g_text_log[LOG_MAX][48];
static int g_text_count;
static uint16_t g_row_y[LOG_MAX];
static int g_row_count;

static const MenuLayout g_layout = { 240U, 320U, 40U, 20U, 20U, 1U };

static void log_text(uint16_t x, uint16_t y, const char *text)
{
    if (g_text_count < LOG_MAX)
    {
        (void)snprintf(g_text_log[g_text_count], sizeof(g_text_log[0]), "%u,%u:%s",
                       (unsigned)x, (unsigned)y, (NULL != text) ? text : "");
        ++g_text_count;
    }
}

static void log_row(uint16_t y)
{
    if (g_row_count < LOG_MAX)
    {
        g_row_y[g_row_count] = y;
        ++g_row_count;
    }
}

static void reset_logs(void)
{
    g_text_count = 0;
    g_row_count = 0;
    (void)memset(g_text_log, 0, sizeof(g_text_log));
    (void)memset(g_row_y, 0, sizeof(g_row_y));
}

static bool log_has(const char *needle)
{
    int i;
    for (i = 0; i < g_text_count; ++i)
    {
        if (NULL != strstr(g_text_log[i], needle)) return true;
    }
    return false;
}

static void fake_clear(void *u) { (void)u; }
static void fake_clear_region(void *u, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{ (void)u; (void)x; (void)y; (void)w; (void)h; }
static void fake_text(void *u, uint16_t x, uint16_t y, const char *t)
{ (void)u; log_text(x, y, t); }
static void fake_rect(void *u, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{ (void)u; (void)x; (void)y; (void)w; (void)h; }
static void fake_fill_rect(void *u, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{ (void)u; (void)x; (void)y; (void)w; (void)h; }
static void fake_line(void *u, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{ (void)u; (void)x0; (void)y0; (void)x1; (void)y1; }
static void fake_image(void *u, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void *p)
{ (void)u; (void)x; (void)y; (void)w; (void)h; (void)p; }
static void fake_begin(void *u) { (void)u; }
static void fake_end(void *u) { (void)u; }
static void fake_row(void *u, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const char *t, bool sel)
{ (void)u; (void)x; (void)w; (void)h; (void)t; (void)sel; log_row(y); }
static void fake_status(void *u, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const char *t)
{ (void)u; (void)x; (void)y; (void)w; (void)h; (void)t; }

static const MenuDisplayOps g_display = {
    fake_clear, fake_clear_region, fake_text, fake_rect, fake_fill_rect,
    fake_line, fake_image, fake_begin, fake_end, fake_row, fake_status
};

static int g_fail;
#define CHECK(cond, msg) \
    do { if (cond) { printf("PASS  %s\n", msg); } \
         else { printf("FAIL  %s\n", msg); ++g_fail; } } while (0)

int main(void)
{
    static Menu menu;

    menu_init(&menu, &g_motor_page, &g_display, NULL, &g_layout);

    /* 首帧：画静态标签和两个字段 */
    reset_logs();
    menu_tick(&menu, 0U);
    CHECK(log_has("RPM") && log_has("DUTY"), "P1 首帧画出实时区静态内容");
    CHECK(log_has(":   0") && log_has(":  0%"), "P2 首帧画出两个定宽字段");
    CHECK((1 == g_row_count) && (100U == g_row_y[0]), "P3 view_height=60 把参数行推到 y=100");

    /* 值没变：is_dirty 返回 false，核心不刷任何东西 */
    reset_logs();
    menu_tick(&menu, 0U);
    CHECK((0 == g_text_count) && (0 == g_row_count), "P4 值没变时完全不再绘制");

    /* t=10：只有 RPM 变（1），DUTY 仍是 0% */
    reset_logs();
    menu_tick(&menu, 10U);
    CHECK((1 == g_text_count) && log_has(":   1"), "P5 只重画变化的 RPM 字段");
    CHECK(!log_has(":  0%"), "P6 没变的 DUTY 字段一个像素都不碰");

    /* t=100：两个字段都变（RPM=10，DUTY=1%） */
    reset_logs();
    menu_tick(&menu, 100U);
    CHECK(log_has(":  10") && log_has(":  1%"), "P7 两个字段都变时各画一次");
    CHECK(2 == g_text_count, "P8 没有多余的重画");

    printf("\n%s (%d failure(s))\n",
           (0 == g_fail) ? "REALTIME PAGE EXAMPLE PASSED" : "REALTIME PAGE EXAMPLE FAILED",
           g_fail);
    return (0 == g_fail) ? 0 : 1;
}
