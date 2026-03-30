#include "menu.h"

#include <string.h>
#include <stdio.h>

#include "zf_common_debug.h"
#include "zf_driver_delay.h"

// =========================
// 按键驱动
// =========================
static uint32 menu_key_scanner_period = 0;
static uint32 menu_key_press_time[MENU_KEY_NUMBER];
static menu_key_state_enum menu_key_state[MENU_KEY_NUMBER];
static const gpio_pin_enum menu_key_pin_map[MENU_KEY_NUMBER] = MENU_KEY_LIST;
static uint8 menu_key_long_press_flag[MENU_KEY_NUMBER] = {1, 1, 1, 1};

void menu_key_scanner(void)
{
    uint8 i;

    for (i = 0; i < MENU_KEY_NUMBER; i++)
    {
        if (MENU_KEY_RELEASE_LEVEL != gpio_get_level(menu_key_pin_map[i]))
        {
            menu_key_press_time[i]++;

            if (0 == menu_key_long_press_flag[i])
            {
                menu_key_state[i] = MENU_KEY_RELEASE;
            }
            else if ((MENU_KEY_LONG_PRESS_PERIOD / menu_key_scanner_period) <= menu_key_press_time[i])
            {
                menu_key_state[i] = MENU_KEY_LONG_PRESS;
                menu_key_long_press_flag[i] = 0;
            }
        }
        else
        {
            if (1 == menu_key_long_press_flag[i])
            {
                if ((MENU_KEY_LONG_PRESS != menu_key_state[i]) &&
                    ((MENU_KEY_MAX_SHOCK_PERIOD / menu_key_scanner_period) <= menu_key_press_time[i]))
                {
                    menu_key_state[i] = MENU_KEY_SHORT_PRESS;
                }
                else
                {
                    menu_key_state[i] = MENU_KEY_RELEASE;
                }
            }

            menu_key_press_time[i] = 0;
            menu_key_long_press_flag[i] = 1;
        }
    }
}

menu_key_state_enum menu_key_get_state(menu_key_index_enum key_n)
{
    return menu_key_state[key_n];
}

void menu_key_clear_state(menu_key_index_enum key_n)
{
    menu_key_state[key_n] = MENU_KEY_RELEASE;
}

void menu_key_clear_all_state(void)
{
    uint8 i;
    for (i = 0; i < MENU_KEY_NUMBER; i++)
    {
        menu_key_state[i] = MENU_KEY_RELEASE;
    }
}

void menu_key_init(uint32 period)
{
    uint8 i;
    zf_assert(0 < period);

    for (i = 0; i < MENU_KEY_NUMBER; i++)
    {
        gpio_init(menu_key_pin_map[i], GPI, GPIO_HIGH, GPI_PULL_UP);
        menu_key_state[i] = MENU_KEY_RELEASE;
        menu_key_press_time[i] = 0;
        menu_key_long_press_flag[i] = 1;
    }

    menu_key_scanner_period = period;
}

// =========================
// 编码器驱动
// =========================
static int menu_encoder_change_num = 0;
static int menu_encoder_pending_steps = 0;
static int menu_encoder_raw_accum = 0;
static uint8 menu_encoder_prev_state = 0xFF;

#define MENU_ENCODER_COUNTS_PER_STEP   2
#define MENU_ENCODER_PENDING_LIMIT     8
#define MENU_ENCODER_DIR_SIGN         -1

static int8 menu_encoder_transition_table[16] =
{
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
};

static uint8 menu_encoder_read_state(void)
{
    uint8 a_level = (uint8)gpio_get_level(MENU_ENCODER_A_PIN);
    uint8 b_level = (uint8)gpio_get_level(MENU_ENCODER_B_PIN);
    return (uint8)((a_level << 1) | b_level);
}

void menu_encoder_init(void)
{
    gpio_init(MENU_ENCODER_A_PIN, GPI, GPIO_HIGH, GPI_PULL_UP);
    gpio_init(MENU_ENCODER_B_PIN, GPI, GPIO_HIGH, GPI_PULL_UP);

    menu_encoder_change_num = 0;
    menu_encoder_pending_steps = 0;
    menu_encoder_raw_accum = 0;
    menu_encoder_prev_state = menu_encoder_read_state();
}

void menu_encoder_update(void)
{
    uint8 current_state = menu_encoder_read_state();
    uint8 transition_index;
    int8 delta;

    if (menu_encoder_prev_state > 3U)
    {
        menu_encoder_prev_state = current_state;
        return;
    }

    if (current_state == menu_encoder_prev_state)
    {
        return;
    }

    transition_index = (uint8)((menu_encoder_prev_state << 2) | current_state);
    delta = menu_encoder_transition_table[transition_index];
    menu_encoder_prev_state = current_state;

    if (0 == delta)
    {
        return;
    }

    delta = (int8)(delta * MENU_ENCODER_DIR_SIGN);
    menu_encoder_raw_accum += delta;

    if (menu_encoder_raw_accum >= MENU_ENCODER_COUNTS_PER_STEP)
    {
        if (menu_encoder_pending_steps < MENU_ENCODER_PENDING_LIMIT)
        {
            menu_encoder_pending_steps++;
        }
        menu_encoder_raw_accum = 0;
    }
    else if (menu_encoder_raw_accum <= -MENU_ENCODER_COUNTS_PER_STEP)
    {
        if (menu_encoder_pending_steps > -MENU_ENCODER_PENDING_LIMIT)
        {
            menu_encoder_pending_steps--;
        }
        menu_encoder_raw_accum = 0;
    }
}

uint8 menu_encoder_has_change(void)
{
    return (0 != menu_encoder_pending_steps);
}

int menu_encoder_get_change(void)
{
    if (menu_encoder_pending_steps > 0)
    {
        menu_encoder_pending_steps--;
        menu_encoder_change_num = 1;
        return menu_encoder_change_num;
    }
    else if (menu_encoder_pending_steps < 0)
    {
        menu_encoder_pending_steps++;
        menu_encoder_change_num = -1;
        return menu_encoder_change_num;
    }

    menu_encoder_change_num = 0;
    return 0;
}

// =========================
// 菜单显示层
// =========================
static MenuPage *menu_current_page = 0;
static int menu_current_selection = 0;
static int menu_last_selection = -1;
static uint8 menu_need_update = 1;
static uint8 menu_full_redraw = 1;
static const char *menu_status_text = 0;

static void menu_fill_rect(uint16 x_start, uint16 y_start, uint16 x_end, uint16 y_end, uint16 color)
{
    uint16 y;

    if (x_start >= ips200_width_max) x_start = ips200_width_max - 1;
    if (x_end >= ips200_width_max) x_end = ips200_width_max - 1;
    if (y_start >= ips200_height_max) y_start = ips200_height_max - 1;
    if (y_end >= ips200_height_max) y_end = ips200_height_max - 1;

    if (x_start > x_end) { uint16 t = x_start; x_start = x_end; x_end = t; }
    if (y_start > y_end) { uint16 t = y_start; y_start = y_end; y_end = t; }

    for (y = y_start; y <= y_end; y++)
    {
        ips200_draw_line(x_start, y, x_end, y, color);
    }
}

static void menu_show_string_fit(uint16 x, uint16 y, const char *s)
{
    int max_chars = (int)((ips200_width_max - 1 - x) / 8);
    char buf[64];
    int i = 0;

    if (max_chars <= 0) return;

    while (i < max_chars && s[i] != '\0' && i < (int)sizeof(buf) - 1)
    {
        buf[i] = s[i];
        i++;
    }
    buf[i] = '\0';
    ips200_show_string(x, y, buf);
}

static void menu_show_string_fit_width_pad(uint16 x, uint16 y, uint16 max_width, const char *s)
{
    int max_chars;
    char buf[64];
    int i = 0;

    if (0U == max_width) return;

    max_chars = (int)(max_width / 8U);
    if (max_chars <= 0) return;

    while (i < max_chars && s[i] != '\0' && i < (int)sizeof(buf) - 1)
    {
        buf[i] = s[i];
        i++;
    }

    while (i < max_chars && i < (int)sizeof(buf) - 1)
    {
        buf[i] = ' ';
        i++;
    }

    buf[i] = '\0';
    ips200_show_string(x, y, buf);
}

void menu_set_status(const char *text)
{
    menu_status_text = text;
    menu_need_update = 1;
    menu_full_redraw = 1;
}

MenuPage *menu_get_current_page(void)
{
    return menu_current_page;
}

int menu_get_current_index(void)
{
    return menu_current_selection;
}

void menu_init(MenuPage *root)
{
#if MENU_USE_IPS200_SPI
    ips200_init(IPS200_TYPE_SPI);
#else
    ips200_init(IPS200_TYPE_PARALLEL8);
#endif

    ips200_set_dir(IPS200_DEFAULT_DISPLAY_DIR);
    ips200_set_font(IPS200_DEFAULT_DISPLAY_FONT);
    ips200_set_color(RGB565_WHITE, RGB565_BLACK);
    ips200_full(RGB565_BLACK);

    menu_key_init(MENU_SCAN_PERIOD_MS);
    menu_key_clear_all_state();
    menu_encoder_init();

    menu_current_page = root;
    if (menu_current_page)
    {
        menu_current_page->parent = 0;
    }
    menu_current_selection = 0;
    menu_last_selection = -1;
    menu_need_update = 1;
    menu_full_redraw = 1;
    menu_status_text = 0;
}

void menu_draw(void)
{
    int i;

    if (0 == menu_current_page || menu_current_page->num_items <= 0)
    {
        return;
    }

    if (menu_full_redraw)
    {
        ips200_full(RGB565_BLACK);

        ips200_set_color(RGB565_YELLOW, RGB565_BLACK);
        menu_show_string_fit(10, 10, menu_current_page->title);
        ips200_draw_line(0, 30, ips200_width_max - 1, 30, RGB565_GRAY);

        for (i = 0; i < menu_current_page->num_items; i++)
        {
            uint16 y_pos = (uint16)(MENU_ITEM_START_Y + i * MENU_ITEM_STEP_Y);
            if (y_pos + 16 >= ips200_height_max) break;

            if (i == menu_current_selection)
            {
                menu_fill_rect(5, (uint16)(y_pos - 2), (uint16)(ips200_width_max - 5), (uint16)(y_pos + 16), RGB565_WHITE);
                ips200_set_color(RGB565_BLACK, RGB565_WHITE);
                menu_show_string_fit(10, y_pos, menu_current_page->items[i].name);
            }
            else
            {
                ips200_set_color(RGB565_WHITE, RGB565_BLACK);
                menu_show_string_fit(10, y_pos, menu_current_page->items[i].name);
            }
        }

        ips200_set_color(RGB565_GRAY, RGB565_BLACK);
        menu_show_string_fit_width_pad(5, (uint16)(ips200_height_max - 20), (uint16)(ips200_width_max - 10), "ENC:Move K1:OK/LONG:BK");

        if (menu_status_text)
        {
            ips200_set_color(RGB565_CYAN, RGB565_BLACK);
            menu_show_string_fit_width_pad(5, (uint16)(ips200_height_max - 40), (uint16)(ips200_width_max - 10), menu_status_text);
        }

        menu_last_selection = menu_current_selection;
        menu_full_redraw = 0;
        menu_need_update = 0;
        return;
    }

    if (menu_need_update)
    {
        int prev = menu_last_selection;
        int curr = menu_current_selection;

        if (prev >= 0 && prev < menu_current_page->num_items)
        {
            uint16 y_prev = (uint16)(MENU_ITEM_START_Y + prev * MENU_ITEM_STEP_Y);
            if (y_prev + 16 < ips200_height_max)
            {
                menu_fill_rect(5, (uint16)(y_prev - 2), (uint16)(ips200_width_max - 5), (uint16)(y_prev + 16), RGB565_BLACK);
                ips200_set_color(RGB565_WHITE, RGB565_BLACK);
                menu_show_string_fit(10, y_prev, menu_current_page->items[prev].name);
            }
        }

        if (curr >= 0 && curr < menu_current_page->num_items)
        {
            uint16 y_curr = (uint16)(MENU_ITEM_START_Y + curr * MENU_ITEM_STEP_Y);
            if (y_curr + 16 < ips200_height_max)
            {
                menu_fill_rect(5, (uint16)(y_curr - 2), (uint16)(ips200_width_max - 5), (uint16)(y_curr + 16), RGB565_WHITE);
                ips200_set_color(RGB565_BLACK, RGB565_WHITE);
                menu_show_string_fit(10, y_curr, menu_current_page->items[curr].name);
            }
        }

        if (menu_status_text)
        {
            menu_fill_rect(0, (uint16)(ips200_height_max - 42), (uint16)(ips200_width_max - 1), (uint16)(ips200_height_max - 26), RGB565_BLACK);
            ips200_set_color(RGB565_CYAN, RGB565_BLACK);
            menu_show_string_fit_width_pad(5, (uint16)(ips200_height_max - 40), (uint16)(ips200_width_max - 10), menu_status_text);
        }

        menu_last_selection = menu_current_selection;
        menu_need_update = 0;
    }
}

void menu_task(void)
{
    int encoder_change;
    MenuItem *item;

    if (0 == menu_current_page || menu_current_page->num_items <= 0)
    {
        return;
    }

    system_delay_ms(MENU_SCAN_PERIOD_MS);
    menu_key_scanner();
    menu_encoder_update();

    if (menu_encoder_has_change())
    {
        encoder_change = menu_encoder_get_change();
        menu_current_selection -= encoder_change;

        while (menu_current_selection >= menu_current_page->num_items)
        {
            menu_current_selection -= menu_current_page->num_items;
        }

        while (menu_current_selection < 0)
        {
            menu_current_selection += menu_current_page->num_items;
        }

        menu_need_update = 1;
    }

    if (MENU_KEY_LONG_PRESS == menu_key_get_state(MENU_KEY_1))
    {
        menu_key_clear_state(MENU_KEY_1);
        if (menu_current_page->parent)
        {
            menu_current_page = menu_current_page->parent;
            menu_current_selection = 0;
            menu_status_text = 0;
            menu_need_update = 1;
            menu_full_redraw = 1;
        }

        menu_draw();
        return;
    }

    if (MENU_KEY_SHORT_PRESS == menu_key_get_state(MENU_KEY_1))
    {
        menu_key_clear_state(MENU_KEY_1);
        item = &menu_current_page->items[menu_current_selection];

        if (item->action)
        {
            item->action();
            menu_full_redraw = 1;
        }
        else if (item->sub_page)
        {
            item->sub_page->parent = menu_current_page;
            menu_current_page = item->sub_page;
            menu_current_selection = 0;
            menu_status_text = 0;
            menu_need_update = 1;
            menu_full_redraw = 1;
        }
    }

    menu_draw();
}
