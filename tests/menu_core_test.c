/*
 * 菜单核心主机侧回归测试（portable menu core）。
 *
 * 只依赖 menu.c/menu.h，不需要任何单片机头文件，主机 gcc 秒级跑完全部断言。
 * 覆盖：实时数值项（MENU_ITEM_LIVE）的脏检测、只重画变化行、缓存防重复、
 * 浮点 NaN 按位比较、读取失败显示 "?"、枚举标签与未知值回退、跨子页缓存
 * 失效、与实时视图区同帧刷新、tick-only 页面、编辑另一行时实时行继续刷新、
 * 实时行只读。
 *
 * 运行：在本目录执行 .\run.ps1（默认编译上一级 menu\ 里的核心源码）。
 *
 * 注意：这是主机测试，**不要拷进固件工程目录**。工程树里的
 * .c 会被 AURIX 构建扫描并编译，这里的 main() 与 menu_* 符号会和固件重复。
 */
#include "menu.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAXROWS 16

static int g_fail;
#define CHECK(cond, msg) \
    do { if (cond) { printf("PASS  %s\n", msg); } \
         else { printf("FAIL  %s\n", msg); ++g_fail; } } while (0)

/* ---------------- fake display ---------------- */
static int g_row_draw[MAXROWS];
static char g_row_text[MAXROWS][128];
static int g_row_selected[MAXROWS];
static int g_text_calls;
static int g_clear_region_calls;
static uint16_t g_view_height;

static int row_of(uint16_t y)
{
    int idx;
    int origin = 40 + (int)g_view_height;
    if ((int)y < origin) return 0;
    idx = ((int)y - origin) / 20;
    if (idx < 0) idx = 0;
    if (idx >= MAXROWS) idx = 0;
    return idx;
}

static void fake_clear(void *u) { (void)u; }
static void fake_clear_region(void *u, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{ (void)u; (void)x; (void)y; (void)w; (void)h; ++g_clear_region_calls; }
static void fake_text(void *u, uint16_t x, uint16_t y, const char *t)
{ (void)u; (void)x; (void)y; (void)t; ++g_text_calls; }
static void fake_row(void *u, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                     const char *text, bool selected)
{
    int idx = row_of(y);
    (void)u; (void)x; (void)w; (void)h;
    ++g_row_draw[idx];
    (void)snprintf(g_row_text[idx], sizeof(g_row_text[idx]), "%s", (NULL != text) ? text : "");
    g_row_selected[idx] = selected ? 1 : 0;
}
static void fake_status(void *u, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                        const char *t)
{ (void)u; (void)x; (void)y; (void)w; (void)h; (void)t; }

static const MenuDisplayOps g_display = {
    fake_clear, fake_clear_region, fake_text, NULL, NULL, NULL, NULL, NULL, NULL,
    fake_row, fake_status
};

static const MenuLayout g_layout = { 240U, 320U, 40U, 20U, 20U, 1U };

static void reset_counters(void)
{
    (void)memset(g_row_draw, 0, sizeof(g_row_draw));
    (void)memset(g_row_text, 0, sizeof(g_row_text));
    (void)memset(g_row_selected, 0, sizeof(g_row_selected));
    g_text_calls = 0;
    g_clear_region_calls = 0;
}

static int total_row_draws(void)
{
    int i;
    int sum = 0;
    for (i = 0; i < MAXROWS; ++i) sum += g_row_draw[i];
    return sum;
}

/* ---------------- test data ---------------- */
static uint32_t v_u32 = 100U;
static float v_float = 1.5f;
static uint8_t v_u8 = 7U;
static uint32_t v_static = 5U;
static uint32_t v_readonly = 1U;
static int g_action_calls;

static bool ro_read(void *user, MenuValue *value)
{
    (void)user;
    value->type = MENU_VALUE_U32;
    value->as.u32 = v_readonly;
    return true;
}

static bool nan_read(void *user, MenuValue *value)
{
    (void)user;
    value->type = MENU_VALUE_FLOAT;
    value->as.f32 = (float)NAN;
    return true;
}

/* ---------- F/G: read failure and enum label coverage ---------- */
static int g_fail_read;
static uint32_t g_state = 2U;
static const uint32_t g_state_values[] = { 0U, 1U, 2U };
static const char *const g_state_labels[] = { "IDLE", "RUN", "FAULT" };

static bool flaky_read(void *user, MenuValue *value)
{
    (void)user;
    if (0 != g_fail_read) return false;
    value->type = MENU_VALUE_U32;
    value->as.u32 = 77U;
    return true;
}

static void noop_action(void *user) { (void)user; ++g_action_calls; }

/* ---------------- page 3: live row + always-dirty view ---------------- */
static int g_view_dirty = 1;
static void p3_tick(void *user, uint32_t now_ms)
{ (void)user; (void)now_ms; g_view_dirty = 1; }
static bool p3_is_dirty(void *user) { (void)user; return g_view_dirty != 0; }
static void p3_draw(void *user, MenuCanvas *canvas)
{ (void)user; menu_canvas_text(canvas, 0U, 0U, "VIEW"); }
static void p3_update(void *user, MenuCanvas *canvas)
{ (void)user; menu_canvas_text(canvas, 0U, 0U, "V"); g_view_dirty = 0; }

static const MenuPageHooks g_p3_hooks = {
    NULL, p3_tick, p3_draw, NULL, NULL, NULL, NULL, 40U, p3_update, p3_is_dirty
};

MENU_PAGE_BEGIN(t_page3, "View Page")
    MENU_LIVE_VAR("Live", MENU_VALUE_U32, &v_u32, 0U, NULL),
    MENU_ACTION_CTX("Noop", noop_action, NULL)
MENU_PAGE_END(t_page3, "View Page", &g_p3_hooks);

/* ---------------- page 2 + page 1 (submenu navigation) ---------------- */
static const MenuPage t_page2;

MENU_PAGE_BEGIN(t_page2, "Page Two")
    MENU_LIVE_VAR("Counter", MENU_VALUE_U32, &v_u32, 0U, NULL),
    MENU_ACTION_CTX("Back", noop_action, NULL)
MENU_PAGE_END(t_page2, "Page Two", NULL);

MENU_PAGE_BEGIN(t_page, "Live Test")
    /* row 0 */ MENU_LIVE_VAR("Counter", MENU_VALUE_U32, &v_u32, 0U, NULL),
    /* row 1 */ MENU_U32("Static", &v_static, 0U, 1000U, 1U),
    /* row 2 */ MENU_LIVE("NanRow", MENU_VALUE_FLOAT, nan_read, NULL, 2U, NULL),
    /* row 3 */ MENU_VALUE("ReadOnly", MENU_VALUE_U32, ro_read, NULL, NULL, 0.0, 100.0, 1.0),
    /* row 4 */ MENU_LIVE_VAR("Float", MENU_VALUE_FLOAT, &v_float, 2U, "dps"),
    /* row 5 */ MENU_LIVE_VAR("Byte", MENU_VALUE_U8, &v_u8, 0U, "%"),
    /* row 6 */ MENU_SUBMENU("Page Two", &t_page2),
    /* row 7 */ MENU_LIVE("Flaky", MENU_VALUE_U32, flaky_read, NULL, 0U, NULL),
    /* row 8 */ MENU_LIVE_ENUM("State", &g_state, g_state_values, g_state_labels, 3U)
MENU_PAGE_END(t_page, "Live Test", NULL);

/* ---------------- page 4: tick-only hook (the Live Values demo shape) ---------------- */
static uint16_t v_step16 = 10U;
static void p4_tick(void *user, uint32_t now_ms)
{ (void)user; (void)now_ms; ++v_u32; }

static const MenuPageHooks g_p4_hooks = {
    NULL, p4_tick, NULL, NULL, NULL, NULL, NULL, 0U, NULL, NULL
};

MENU_PAGE_BEGIN(t_page4, "Tick Only")
    MENU_LIVE_VAR("Counter", MENU_VALUE_U32, &v_u32, 0U, NULL),
    MENU_U16("Step", &v_step16, 0U, 1000U, 1U)
MENU_PAGE_END(t_page4, "Tick Only", &g_p4_hooks);

int main(void)
{
    static Menu m1;
    static Menu m3;
    MenuEvent event;
    int i;

    /* ---------- A. hook-less page: dirty detection ---------- */
    menu_init(&m1, &t_page, &g_display, NULL, &g_layout);
    g_view_height = 0U;
    menu_tick(&m1, 0U);
    CHECK(g_row_draw[0] >= 1, "A1 first frame draws live row");
    CHECK(0 == strcmp(g_row_text[0], "Counter: 100"), "A2 first frame text is current value");
    CHECK(1 == g_row_selected[0], "A3 selected live row keeps selected flag");

    reset_counters();
    menu_tick(&m1, 10U);
    menu_tick(&m1, 20U);
    CHECK(0 == total_row_draws(), "A4 unchanged values draw nothing (incl. NaN row)");
    CHECK(0 == g_text_calls, "A5 unchanged values do not touch the display at all");

    reset_counters();
    v_u32 = 101U;
    menu_tick(&m1, 30U);
    CHECK(1 == g_row_draw[0], "A6 changed live row is drawn exactly once");
    CHECK(1 == total_row_draws(), "A7 only the changed row is redrawn");
    CHECK(0 == strcmp(g_row_text[0], "Counter: 101"), "A8 changed row shows new value");
    CHECK(1 == g_row_selected[0] && 0 == g_row_draw[4],
          "A9 selected live row keeps highlight, other rows untouched");

    reset_counters();
    menu_tick(&m1, 40U);
    CHECK(0 == total_row_draws(), "A10 cache prevents repeat redraw of same value");

    reset_counters();
    v_readonly = 2U;
    menu_tick(&m1, 50U);
    CHECK(0 == total_row_draws(), "A11 READONLY without LIVE stays manual (no auto refresh)");

    reset_counters();
    v_float = -12.25f;
    menu_tick(&m1, 60U);
    CHECK(1 == g_row_draw[4], "A12 float live row redraws on change");
    CHECK(NULL != strstr(g_row_text[4], "-12.25dps"), "A13 float precision + unit formatting");
    CHECK(0 == g_row_selected[4], "A14 unselected live row drawn unselected");

    reset_counters();
    v_u8 = 42U;
    menu_tick(&m1, 70U);
    CHECK(1 == g_row_draw[5], "A15 u8 live row redraws on change");
    CHECK(NULL != strstr(g_row_text[5], "42%"), "A16 integer + unit formatting");

    reset_counters();
    event.key = MENU_KEY_DOWN;
    event.press = MENU_KEY_SHORT;
    menu_handle_event(&m1, &event);
    menu_tick(&m1, 80U);
    CHECK(1 == g_row_draw[0] && 1 == g_row_draw[1],
          "A17 navigation redraws exactly the old and new selection rows");
    CHECK(0 == g_row_draw[4] && 0 == g_row_draw[5],
          "A18 navigation leaves other live rows alone");
    reset_counters();
    menu_tick(&m1, 90U);
    CHECK(0 == total_row_draws(), "A19 no live-triggered redraw right after navigation");

    /* ---------- B. submenu round trip ---------- */
    for (i = 0; i < 5; ++i)
    {
        event.key = MENU_KEY_DOWN;
        event.press = MENU_KEY_SHORT;
        menu_handle_event(&m1, &event);
        menu_tick(&m1, (uint32_t)(100 + i * 10));
    }
    event.key = MENU_KEY_OK;
    event.press = MENU_KEY_SHORT;
    menu_handle_event(&m1, &event);
    menu_tick(&m1, 200U);
    CHECK(0 == strcmp(menu_current_page(&m1)->title, "Page Two"), "B1 submenu entered");

    reset_counters();
    v_u32 = 202U;
    menu_tick(&m1, 210U);
    CHECK(1 == g_row_draw[0], "B2 live row on submenu page refreshes");
    reset_counters();
    menu_tick(&m1, 220U);
    CHECK(0 == total_row_draws(), "B3 submenu page cache also settles");

    event.key = MENU_KEY_BACK;
    event.press = MENU_KEY_SHORT;
    menu_handle_event(&m1, &event);
    menu_tick(&m1, 300U);
    CHECK(0 == strcmp(menu_current_page(&m1)->title, "Live Test"), "B4 returned to root page");

    reset_counters();
    v_u32 = 303U;
    menu_tick(&m1, 310U);
    CHECK(1 == g_row_draw[0], "B5 stale cache from other page does not suppress refresh");
    reset_counters();
    menu_tick(&m1, 320U);
    CHECK(0 == total_row_draws(), "B6 root page settles again after change");

    /* ---------- C. live rows together with a dirty page view ---------- */
    g_view_height = 40U;
    menu_init(&m3, &t_page3, &g_display, NULL, &g_layout);
    reset_counters();
    menu_tick(&m3, 0U);
    CHECK(g_row_draw[0] >= 1, "C1 first frame draws live row on view page");

    reset_counters();
    menu_tick(&m3, 10U);
    CHECK(g_text_calls > 0, "C2 view update still runs when view is dirty");
    CHECK(0 == g_row_draw[0], "C3 unchanged live row stays quiet in view mode");

    reset_counters();
    v_u32 = 555U;
    menu_tick(&m3, 20U);
    CHECK(1 == g_row_draw[0], "C4 changed live row refreshes in the same tick as view update");
    CHECK(g_text_calls > 0, "C5 view update is not lost when a live row is dirty");

    reset_counters();
    menu_tick(&m3, 30U);
    CHECK(0 == g_row_draw[0], "C6 live row settles while view keeps refreshing");

    /* ---------- F. read failure shows "?" and settles ---------- */
    g_view_height = 0U; /* m1 的页面没有实时视图区，恢复行号映射 */
    reset_counters();
    g_fail_read = 1;
    menu_tick(&m1, 400U);
    CHECK(1 == g_row_draw[7], "F1 read failure redraws the row once");
    CHECK(0 == strcmp(g_row_text[7], "Flaky: ?"),
          "F2 read failure shows ? instead of a stale value");
    reset_counters();
    menu_tick(&m1, 410U);
    CHECK(0 == total_row_draws(), "F3 continuing failure does not redraw every tick");
    reset_counters();
    g_fail_read = 0;
    menu_tick(&m1, 420U);
    CHECK(1 == g_row_draw[7], "F4 recovery redraws the row once");
    CHECK(0 == strcmp(g_row_text[7], "Flaky: 77"), "F5 recovery shows the value again");
    reset_counters();
    menu_tick(&m1, 430U);
    CHECK(0 == total_row_draws(), "F6 recovery also settles");

    /* ---------- G. enum live row ---------- */
    reset_counters();
    g_state = 1U;
    menu_tick(&m1, 500U);
    CHECK(1 == g_row_draw[8], "G1 enum live row redraws on change");
    CHECK(0 == strcmp(g_row_text[8], "State: RUN"), "G2 enum live row shows its label");
    reset_counters();
    g_state = 9U;
    menu_tick(&m1, 510U);
    CHECK(1 == g_row_draw[8], "G3 unknown enum value still refreshes");
    CHECK(0 == strcmp(g_row_text[8], "State: 9"),
          "G4 unknown enum value falls back to the number");
    reset_counters();
    menu_tick(&m1, 520U);
    CHECK(0 == total_row_draws(), "G5 enum live row settles");

    /* ---------- E. live rows are read-only ---------- */
    event.key = MENU_KEY_OK;
    event.press = MENU_KEY_SHORT;
    menu_handle_event(&m3, &event);
    CHECK(!menu_is_editing(&m3), "E1 OK on a live row does not enter edit mode");
    event.key = MENU_KEY_UP;
    event.press = MENU_KEY_SHORT;
    menu_handle_event(&m3, &event);
    CHECK(555U == v_u32, "E2 live row binding is never written by menu navigation");

    /* ---------- D. capacity limit ---------- */
    /* 240x320 屏幕、40 标题、20 页脚、20 行高、无实时视图 = 13 个可见行。 */
    CHECK(MENU_MAX_LIVE_ROWS >= 13U, "D1 live cache covers every visible row of this layout");

    /* ---------- H. tick-only hook page (shape of the Live Values demo page) ---------- */
    {
        static Menu m4;
        menu_init(&m4, &t_page4, &g_display, NULL, &g_layout);
        g_view_height = 0U;
        menu_tick(&m4, 0U);
        reset_counters();
        menu_tick(&m4, 10U);
        CHECK(1 == g_row_draw[0], "H1 tick-only hook page refreshes its live row");
        reset_counters();
        menu_tick(&m4, 20U);
        CHECK(1 == g_row_draw[0], "H2 refresh repeats while the value keeps changing");

        event.key = MENU_KEY_DOWN;
        event.press = MENU_KEY_SHORT;
        menu_handle_event(&m4, &event);
        event.key = MENU_KEY_OK;
        event.press = MENU_KEY_SHORT;
        menu_handle_event(&m4, &event);
        CHECK(menu_is_editing(&m4), "H3 editable row enters edit mode");
        reset_counters();
        menu_tick(&m4, 30U);
        CHECK(1 == g_row_draw[0], "H4 live row keeps refreshing while another row is edited");
        event.key = MENU_KEY_BACK;
        event.press = MENU_KEY_SHORT;
        menu_handle_event(&m4, &event);
    }

    printf("\n%s (%d failure(s))\n", (0 == g_fail) ? "ALL TESTS PASSED" : "TESTS FAILED", g_fail);
    return (0 == g_fail) ? 0 : 1;
}
