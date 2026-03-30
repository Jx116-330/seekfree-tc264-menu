#ifndef MENU_H
#define MENU_H

#include "zf_common_typedef.h"
#include "zf_driver_gpio.h"
#include "zf_device_ips200.h"

// =========================
// 按键配置
// =========================
#define MENU_KEY_LIST                    {P20_2, P20_8, P20_6, P20_7}
#define MENU_KEY_RELEASE_LEVEL           (GPIO_HIGH)
#define MENU_KEY_MAX_SHOCK_PERIOD        (20)
#define MENU_KEY_LONG_PRESS_PERIOD       (250)

typedef enum
{
    MENU_KEY_1,
    MENU_KEY_2,
    MENU_KEY_3,
    MENU_KEY_4,
    MENU_KEY_NUMBER,
} menu_key_index_enum;

typedef enum
{
    MENU_KEY_RELEASE,
    MENU_KEY_SHORT_PRESS,
    MENU_KEY_LONG_PRESS,
} menu_key_state_enum;

// =========================
// 编码器配置
// =========================
#define MENU_ENCODER_A_PIN               P20_3
#define MENU_ENCODER_B_PIN               P20_0

// =========================
// 菜单结构
// =========================
typedef struct MenuPage MenuPage;
typedef struct MenuItem MenuItem;
typedef void (*menu_action_t)(void);

struct MenuItem
{
    const char *name;
    menu_action_t action;
    MenuPage *sub_page;
};

struct MenuPage
{
    const char *title;
    MenuItem *items;
    int num_items;
    MenuPage *parent;
};

// =========================
// 用户可选配置
// =========================
#ifndef MENU_USE_IPS200_SPI
#define MENU_USE_IPS200_SPI              1
#endif

#ifndef MENU_SCAN_PERIOD_MS
#define MENU_SCAN_PERIOD_MS              10
#endif

#ifndef MENU_ITEM_START_Y
#define MENU_ITEM_START_Y                40
#endif

#ifndef MENU_ITEM_STEP_Y
#define MENU_ITEM_STEP_Y                 20
#endif

// =========================
// 对外接口
// =========================
void menu_init(MenuPage *root);
void menu_task(void);
void menu_draw(void);
void menu_set_status(const char *text);

MenuPage *menu_get_current_page(void);
int menu_get_current_index(void);

// =========================
// 输入驱动接口（一般不用外部调用）
// =========================
void menu_key_init(uint32 period);
void menu_key_scanner(void);
menu_key_state_enum menu_key_get_state(menu_key_index_enum key_n);
void menu_key_clear_state(menu_key_index_enum key_n);
void menu_key_clear_all_state(void);

void menu_encoder_init(void);
void menu_encoder_update(void);
uint8 menu_encoder_has_change(void);
int menu_encoder_get_change(void);

#endif
