/*
 * 宏形态检查：用与 firmware/code/menu/menu_app.c 完全相同的
 * MENU_LIVE / MENU_LIVE_VAR / MENU_LIVE_ENUM / MENU_U16 调用形式单独编译，
 * 核对展开后的字段（类型、绑定地址、精度、单位、枚举标签、只读位）是否符合
 * 预期——menu_app.c 依赖 TC264 头文件，主机编译不了，这一层就是它的替身。
 *
 * 运行：在本目录执行 .\run.ps1。
 * 不要拷进 ADS 工程目录，原因见 menu_core_test.c 头部说明。
 */
#include "menu.h"

#include <stdio.h>
#include <string.h>

/* 与 menu_app.c 中同名的演示变量，类型必须与菜单项声明的类型一致。 */
static uint32_t g_demo_ticks;
static uint8_t g_demo_progress;
static float g_live_gyro_z;
static uint8_t g_live_level;
static uint32_t g_live_state;
static uint16_t g_live_step = 10U;
static const uint32_t g_live_state_values[] = { 0U, 1U, 2U };
static const char *const g_live_state_labels[] = { "IDLE", "RUN", "FAULT" };

static bool demo_read_uptime(void *user, MenuValue *value)
{
    (void)user;
    value->type = MENU_VALUE_FLOAT;
    value->as.f32 = 1.0f;
    return true;
}

/* 与 menu_app.c 里的实时数值行一一对应（Live State / 首页 / Live Values）。 */
MENU_PAGE_BEGIN(check_page, "Check")
    MENU_LIVE_VAR("Ticks", MENU_VALUE_U32, &g_demo_ticks, 0U, NULL),
    MENU_LIVE_VAR("Progress", MENU_VALUE_U8, &g_demo_progress, 0U, "%"),
    MENU_LIVE("Uptime", MENU_VALUE_FLOAT, demo_read_uptime, NULL, 1U, "s"),
    MENU_LIVE_VAR("GyroZ", MENU_VALUE_FLOAT, &g_live_gyro_z, 1U, "dps"),
    MENU_LIVE_VAR("Level", MENU_VALUE_U8, &g_live_level, 0U, "%"),
    MENU_LIVE_ENUM("State", &g_live_state, g_live_state_values,
                   g_live_state_labels, 3U),
    MENU_U16("Step", &g_live_step, 0, 1000, 10)
MENU_PAGE_END(check_page, "Check", NULL);

static bool same_text(const char *a, const char *b)
{
    if ((NULL == a) || (NULL == b)) return false;
    return 0 == strcmp(a, b);
}

int main(void)
{
    int fail = 0;
    const MenuItem *it = check_page.items;

    if (7U != check_page.item_count)
    {
        printf("FAIL item_count=%u\n", (unsigned)check_page.item_count);
        ++fail;
    }
    if ((MENU_ITEM_LIVE != it[0].type) || (MENU_VALUE_U32 != it[0].data.value.type) ||
        ((void *)&g_demo_ticks != it[0].data.value.user))
    {
        printf("FAIL row0 Ticks\n");
        ++fail;
    }
    if ((MENU_ITEM_LIVE != it[1].type) || (MENU_VALUE_U8 != it[1].data.value.type) ||
        !same_text(it[1].data.value.unit, "%"))
    {
        printf("FAIL row1 Progress\n");
        ++fail;
    }
    if ((MENU_ITEM_LIVE != it[2].type) || (it[2].data.value.read != demo_read_uptime) ||
        (1U != it[2].data.value.precision) || !same_text(it[2].data.value.unit, "s"))
    {
        printf("FAIL row2 Uptime\n");
        ++fail;
    }
    if ((MENU_ITEM_LIVE != it[3].type) || (MENU_VALUE_FLOAT != it[3].data.value.type) ||
        ((void *)&g_live_gyro_z != it[3].data.value.user) ||
        (1U != it[3].data.value.precision) || !same_text(it[3].data.value.unit, "dps"))
    {
        printf("FAIL row3 GyroZ\n");
        ++fail;
    }
    if ((MENU_ITEM_LIVE != it[4].type) || (MENU_VALUE_U8 != it[4].data.value.type) ||
        ((void *)&g_live_level != it[4].data.value.user))
    {
        printf("FAIL row4 Level\n");
        ++fail;
    }
    if ((MENU_ITEM_LIVE != it[5].type) || (MENU_VALUE_ENUM != it[5].data.value.type) ||
        (3U != it[5].data.value.enum_count) ||
        (g_live_state_labels != it[5].data.value.enum_labels) ||
        ((void *)&g_live_state != it[5].data.value.user))
    {
        printf("FAIL row5 State\n");
        ++fail;
    }
    if ((MENU_ITEM_U16 != it[6].type) || ((void *)&g_live_step != it[6].data.value.user))
    {
        printf("FAIL row6 Step\n");
        ++fail;
    }
    if ((NULL != it[0].data.value.write) || (NULL != it[3].data.value.write))
    {
        printf("FAIL live rows must stay read-only\n");
        ++fail;
    }
    if (MENU_VALUE_UNAVAILABLE == MENU_VALUE_ENUM)
    {
        printf("FAIL unavailable sentinel must not collide with enum\n");
        ++fail;
    }
    if (0 != fail) return fail;
    printf("PASS  MENU_LIVE macro forms match the demo usage\n");
    return 0;
}
