/*
 * portable-menu/menu.c
 *
 * 菜单核心实现。这里集中处理所有页面都共用的机制：页面栈、列表选择、
 * 数值编辑、绘制调度、实时数值行的自动刷新和生命周期。业务模块只通过
 * MenuPageHooks、数值 getter/setter 和 MenuDisplayOps 与本文件交互。
 */
#include "menu.h"

#include <stdio.h>
#include <string.h>

#define MENU_REDRAW_NONE       (0U)
#define MENU_REDRAW_PARTIAL    (1U)
#define MENU_REDRAW_FULL       (2U)
#define MENU_REDRAW_VIEW       (3U)

/* 判断一行是否携带可显示的数值描述。CUSTOM 行使用另一套绘制路径。 */
/*********************************************************************************************************************
 * @brief  : 判断菜单项是否携带可显示的数值描述
 * @param  : item  菜单项描述
 * @return : true 表示携带数值描述，false 表示不携带
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static bool menu_item_is_value(const MenuItem *item)
{
    if (NULL == item)
    {
        return false;
    }
    return (MENU_ITEM_BOOL == item->type) ||
           (MENU_ITEM_ENUM == item->type) ||
           (MENU_ITEM_I8 == item->type) ||
           (MENU_ITEM_U8 == item->type) ||
           (MENU_ITEM_I16 == item->type) ||
           (MENU_ITEM_U16 == item->type) ||
           (MENU_ITEM_I32 == item->type) ||
           (MENU_ITEM_U32 == item->type) ||
           (MENU_ITEM_FLOAT == item->type) ||
           ((MENU_ITEM_READONLY == item->type) && (NULL != item->data.value.read)) ||
           ((MENU_ITEM_LIVE == item->type) &&
            ((NULL != item->data.value.read) || (NULL != item->data.value.user))) ||
           ((MENU_ITEM_BINDING == item->type) &&
            ((NULL != item->data.value.read) || (NULL != item->data.value.write)));
}

/* 取得数值项描述；非数值项统一返回 NULL，调用者无需重复判断 type。 */
/*********************************************************************************************************************
 * @brief  : 获取菜单项的数值描述结构
 * @param  : item  菜单项描述
 * @return : 数值描述地址，非数值项返回 NULL
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static const MenuValueItem *menu_value_item(const MenuItem *item)
{
    return menu_item_is_value(item) ? &item->data.value : NULL;
}

/* 只读项和实时数值项都不可编辑；动态绑定项必须同时提供 write 回调才能修改。 */
/*********************************************************************************************************************
 * @brief  : 判断菜单项是否允许进入编辑状态
 * @param  : item  菜单项描述
 * @return : true 表示可编辑，false 表示不可编辑
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static bool menu_item_is_editable(const MenuItem *item)
{
    const MenuValueItem *value = menu_value_item(item);
    if ((NULL == value) || (MENU_ITEM_READONLY == item->type) || (MENU_ITEM_LIVE == item->type)) return false;
    if ((MENU_ITEM_BINDING == item->type) && (NULL == value->write)) return false;
    return true;
}

/*
 * 计算实时视图高度。
 *
 * 有列表项时，视图占用除参数列表至少一行之外的全部内容区；没有列表项
 * 的纯实时页则可以占满内容区。这样摄像头页面可以尽量放大图像，同时
 * 仍然保留下面的参数行。
 */
/*********************************************************************************************************************
 * @brief  : 计算当前页面实时视图区高度
 * @param  : menu  菜单运行时对象
 * @return : 实时视图区高度，单位为像素
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static uint16_t menu_view_height(const Menu *menu)
{
    uint16_t content;
    uint16_t requested;
    if ((NULL == menu) || (NULL == menu->page) || (NULL == menu->page->hooks))
        return 0U;
    if (menu->layout.height <= menu->layout.title_height + menu->layout.footer_height)
        return 0U;
    content = (uint16_t)(menu->layout.height - menu->layout.title_height -
                         menu->layout.footer_height);
    requested = menu->page->hooks->view_height;
    if (menu->page->item_count > 0U)
    {
        if (content <= menu->layout.row_height) return 0U;
        {
            uint16_t maximum = (uint16_t)(content - menu->layout.row_height);
            return (requested < maximum) ? requested : maximum;
        }
    }
    return (requested < content) ? requested : content;
}

/* 根据标题、实时视图和页脚剩余空间计算当前页能显示多少行。 */
/*********************************************************************************************************************
 * @brief  : 计算当前页面可显示的列表行数
 * @param  : menu  菜单运行时对象
 * @return : 当前可显示的菜单行数
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static uint8_t menu_visible_rows(const Menu *menu)
{
    uint16_t usable;
    if ((NULL == menu) || (0U == menu->layout.row_height))
    {
        return 1U;
    }
    usable = menu->layout.height;
    if (usable > menu->layout.title_height)
    {
        usable = (uint16_t)(usable - menu->layout.title_height);
    }
    if (usable > menu->layout.footer_height)
    {
        usable = (uint16_t)(usable - menu->layout.footer_height);
    }
    usable = (uint16_t)(usable - menu_view_height(menu));
    usable = (uint16_t)(usable / menu->layout.row_height);
    return (usable > 0U) ? (uint8_t)((usable > 255U) ? 255U : usable) : 1U;
}

/* 让当前高亮行始终落在可见窗口内。 */
/*********************************************************************************************************************
 * @brief  : 调整滚动位置使当前选中行保持可见
 * @param  : menu  菜单运行时对象
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void menu_adjust_scroll(Menu *menu)
{
    uint8_t rows;
    if ((NULL == menu) || (NULL == menu->page))
    {
        return;
    }
    rows = menu_visible_rows(menu);
    if (menu->selection < menu->first_visible)
    {
        menu->first_visible = menu->selection;
    }
    while (((uint16_t)menu->selection >= (uint16_t)menu->first_visible + rows) &&
           (menu->first_visible + rows < menu->page->item_count))
    {
        ++menu->first_visible;
    }
}

/* 记录一次导航引起的局部刷新；页面变化仍由 menu_request_redraw 全屏刷新。 */
/*********************************************************************************************************************
 * @brief  : 记录一次导航或编辑引起的局部刷新请求
 * @param  : menu  菜单运行时对象
 * @param  : old_selection  刷新前的选中项下标
 * @param  : old_first_visible  刷新前的首个可见项下标
 * @param  : allow_pending_full_override  是否允许局部刷新覆盖待执行的整屏刷新
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void menu_request_partial_redraw(Menu *menu, uint8_t old_selection, uint8_t old_first_visible,
                                        bool allow_pending_full_override)
{
    if (NULL == menu)
    {
        return;
    }
    if (MENU_REDRAW_FULL == menu->redraw_mode)
    {
        /* 同一页已经有有效画面时，编辑事件可以覆盖尚未执行的 FULL 请求。
         * 页面刚切换或尚未画出首帧时必须保留整屏重绘，否则标题/静态视图会残留父页内容。 */
        if ((0U == allow_pending_full_override) || (menu->redraw_page != menu->page) ||
            (menu->rendered_page != menu->page))
        {
            menu->redraw_requested = 1U;
            return;
        }
        menu->redraw_old_selection = old_selection;
        menu->redraw_old_first_visible = old_first_visible;
    }
    else if (MENU_REDRAW_NONE == menu->redraw_mode)
    {
        menu->redraw_old_selection = old_selection;
        menu->redraw_old_first_visible = old_first_visible;
        menu->redraw_page = menu->page;
    }
    else if (menu->redraw_page != menu->page)
    {
        menu_request_redraw(menu);
        return;
    }
    else if (MENU_REDRAW_VIEW == menu->redraw_mode)
    {
        /* 视图刷新和行刷新可以在同一帧完成：降级为局部刷新时保留视图刷新，
         * 否则旋转编码器或刷新实时行会吃掉这一帧的视图更新。 */
        menu->view_redraw_requested = 1U;
    }
    menu->redraw_mode = MENU_REDRAW_PARTIAL;
    menu->redraw_requested = 1U;
}

/* 移动列表高亮；wrap_navigation 决定到边界后是否从另一端继续。 */
/*********************************************************************************************************************
 * @brief  : 按指定方向移动菜单选中项
 * @param  : menu  菜单运行时对象
 * @param  : direction  调整方向，正数增加，负数减少
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void menu_move_selection(Menu *menu, int direction)
{
    uint8_t old_selection;
    uint8_t old_first_visible;
    int next;
    if ((NULL == menu) || (NULL == menu->page) || (0U == menu->page->item_count))
    {
        return;
    }
    old_selection = menu->selection;
    old_first_visible = menu->first_visible;
    next = (int)menu->selection + direction;
    if (0U != menu->layout.wrap_navigation)
    {
        if (next < 0)
        {
            next = (int)menu->page->item_count - 1;
        }
        else if (next >= (int)menu->page->item_count)
        {
            next = 0;
        }
    }
    else
    {
        if (next < 0)
        {
            next = 0;
        }
        if (next >= (int)menu->page->item_count)
        {
            next = (int)menu->page->item_count - 1;
        }
    }
    menu->selection = (uint8_t)next;
    menu_adjust_scroll(menu);
    menu_request_partial_redraw(menu, old_selection, old_first_visible, false);
    if (old_selection != menu->selection)
    {
        menu->footer_redraw_requested = 1U;
    }
}

/* 读取当前值：优先使用业务 getter，否则读取宏绑定的基础类型变量。 */
/*********************************************************************************************************************
 * @brief  : 读取菜单项当前值
 * @param  : item  菜单项描述
 * @param  : value  菜单值容器
 * @return : true 表示读取成功，false 表示当前值不可用
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static bool menu_read_value(const MenuItem *item, MenuValue *value)
{
    const MenuValueItem *v = menu_value_item(item);
    if ((NULL == v) || (NULL == value))
    {
        return false;
    }
    value->type = v->type;
    if (NULL != v->read)
    {
        return v->read(v->user, value);
    }
    if (NULL == v->user)
    {
        return false;
    }
    switch (v->type)
    {
        case MENU_VALUE_BOOL:
            value->as.boolean = *((const bool *)v->user);
            return true;
        case MENU_VALUE_I8: value->as.i8 = *((const int8_t *)v->user); return true;
        case MENU_VALUE_U8: value->as.u8 = *((const uint8_t *)v->user); return true;
        case MENU_VALUE_I16: value->as.i16 = *((const int16_t *)v->user); return true;
        case MENU_VALUE_U16: value->as.u16 = *((const uint16_t *)v->user); return true;
        case MENU_VALUE_I32:
            value->as.i32 = *((const int32_t *)v->user);
            return true;
        case MENU_VALUE_U32:
            value->as.u32 = *((const uint32_t *)v->user);
            return true;
        case MENU_VALUE_FLOAT:
            value->as.f32 = *((const float *)v->user);
            return true;
        case MENU_VALUE_ENUM:
            value->as.enumeration = *((const uint32_t *)v->user);
            return true;
        default:
            return false;
    }
}

/* 写入当前值：动态绑定由业务 setter 执行合法性和忙碌状态检查。 */
/*********************************************************************************************************************
 * @brief  : 写入菜单项的新值
 * @param  : item  菜单项描述
 * @param  : value  菜单值容器
 * @return : true 表示写入成功，false 表示写入被拒绝
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static bool menu_write_value(const MenuItem *item, const MenuValue *value)
{
    const MenuValueItem *v = menu_value_item(item);
    if ((NULL == v) || (NULL == value) || (MENU_ITEM_READONLY == item->type) ||
        (MENU_ITEM_LIVE == item->type) ||
        ((MENU_ITEM_BINDING == item->type) && (NULL == v->write)))
    {
        return false;
    }
    if (NULL != v->write)
    {
        return v->write(v->user, value);
    }
    if (NULL == v->user)
    {
        return false;
    }
    switch (v->type)
    {
        case MENU_VALUE_BOOL:
            *((bool *)v->user) = value->as.boolean;
            return true;
        case MENU_VALUE_I8: *((int8_t *)v->user) = value->as.i8; return true;
        case MENU_VALUE_U8: *((uint8_t *)v->user) = value->as.u8; return true;
        case MENU_VALUE_I16: *((int16_t *)v->user) = value->as.i16; return true;
        case MENU_VALUE_U16: *((uint16_t *)v->user) = value->as.u16; return true;
        case MENU_VALUE_I32:
            *((int32_t *)v->user) = value->as.i32;
            return true;
        case MENU_VALUE_U32:
            *((uint32_t *)v->user) = value->as.u32;
            return true;
        case MENU_VALUE_FLOAT:
            *((float *)v->user) = value->as.f32;
            return true;
        case MENU_VALUE_ENUM:
            *((uint32_t *)v->user) = value->as.enumeration;
            return true;
        default:
            return false;
    }
}

/* 将值转换为列表行右侧的 ASCII 文本；未知枚举值仍显示其原始数字。 */
/*********************************************************************************************************************
 * @brief  : 将菜单项值格式化为屏幕文本
 * @param  : item  菜单项描述
 * @param  : text  文本缓冲区或待绘制文本
 * @param  : text_size  文本缓冲区容量
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void menu_format_value(const MenuItem *item, char *text, size_t text_size)
{
    MenuValue value;
    const MenuValueItem *v = menu_value_item(item);
    uint8_t i;

    if ((NULL == text) || (0U == text_size))
    {
        return;
    }
    text[0] = '\0';
    if ((NULL == v) || !menu_read_value(item, &value))
    {
        (void)snprintf(text, text_size, "?");
        return;
    }
    switch (v->type)
    {
        case MENU_VALUE_BOOL:
            (void)snprintf(text, text_size, "%s", value.as.boolean ? "ON" : "OFF");
            break;
        case MENU_VALUE_I8:
            (void)snprintf(text, text_size, "%d%s", (int)value.as.i8, (NULL != v->unit) ? v->unit : ""); break;
        case MENU_VALUE_U8:
            (void)snprintf(text, text_size, "%u%s", (unsigned)value.as.u8, (NULL != v->unit) ? v->unit : ""); break;
        case MENU_VALUE_I16:
            (void)snprintf(text, text_size, "%d%s", (int)value.as.i16, (NULL != v->unit) ? v->unit : ""); break;
        case MENU_VALUE_U16:
            (void)snprintf(text, text_size, "%u%s", (unsigned)value.as.u16, (NULL != v->unit) ? v->unit : ""); break;
        case MENU_VALUE_I32:
            (void)snprintf(text, text_size, "%ld%s", (long)value.as.i32, (NULL != v->unit) ? v->unit : "");
            break;
        case MENU_VALUE_U32:
            (void)snprintf(text, text_size, "%lu%s", (unsigned long)value.as.u32,
                           (NULL != v->unit) ? v->unit : "");
            break;
        case MENU_VALUE_FLOAT:
            (void)snprintf(text, text_size, "%.*f%s", (int)v->precision,
                           (double)value.as.f32, (NULL != v->unit) ? v->unit : "");
            break;
        case MENU_VALUE_ENUM:
            for (i = 0U; i < v->enum_count; ++i)
            {
                if ((NULL != v->enum_values) && (v->enum_values[i] == value.as.enumeration))
                {
                    if ((NULL != v->enum_labels) && (NULL != v->enum_labels[i]))
                    {
                        (void)snprintf(text, text_size, "%s", v->enum_labels[i]);
                    }
                    else
                    {
                        (void)snprintf(text, text_size, "%lu", (unsigned long)value.as.enumeration);
                    }
                    return;
                }
            }
            (void)snprintf(text, text_size, "%lu", (unsigned long)value.as.enumeration);
            break;
        default:
            (void)snprintf(text, text_size, "?");
            break;
    }
}

/*********************************************************************************************************************
 * @brief  : 格式化当前编辑步长提示
 * @param  : item  菜单项描述
 * @param  : menu  菜单运行时对象
 * @param  : text  文本缓冲区或待绘制文本
 * @param  : text_size  文本缓冲区容量
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void menu_format_step(const MenuItem *item, const Menu *menu, char *text, size_t text_size)
{
    const MenuValueItem *v = menu_value_item(item);
    double step;
    if ((NULL == text) || (0U == text_size)) return;
    text[0] = '\0';
    if ((NULL == v) || (NULL == menu)) return;
    if ((NULL != v->steps) && (0U != v->step_count))
    {
        step = v->steps[(menu->edit_step_index < v->step_count) ?
                        menu->edit_step_index : (uint8_t)(v->step_count - 1U)];
        (void)snprintf(text, text_size, "STEP %.3f", step);
    }
    else if (0.0 != v->step)
    {
        (void)snprintf(text, text_size, "STEP %.3f", v->step);
    }
}

/*********************************************************************************************************************
 * @brief  : 格式化底部状态栏文本
 * @param  : menu  菜单运行时对象
 * @param  : text  文本缓冲区或待绘制文本
 * @param  : text_size  文本缓冲区容量
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void menu_format_selected_state(const Menu *menu, char *text, size_t text_size)
{
    const MenuItem *item;
    char step_text[24];

    if ((NULL == text) || (0U == text_size)) return;
    if ((NULL == menu) || (NULL == menu->page) || (menu->selection >= menu->page->item_count))
    {
        (void)snprintf(text, text_size, "STATUS");
        return;
    }
    item = &menu->page->items[menu->selection];
    if (0U != menu->editing)
    {
        menu_format_step(item, menu, step_text, sizeof(step_text));
        if ('\0' != step_text[0])
        {
            (void)snprintf(text, text_size, "STATUS EDITING %s", step_text);
        }
        else
        {
            (void)snprintf(text, text_size, "STATUS EDITING");
        }
        return;
    }
    if (MENU_ITEM_SUBMENU == item->type)
    {
        (void)snprintf(text, text_size, "STATUS MENU OK=ENTER");
    }
    else if (MENU_ITEM_ACTION == item->type)
    {
        (void)snprintf(text, text_size, "STATUS ACTION OK=RUN");
    }
    else if (MENU_ITEM_CUSTOM == item->type)
    {
        (void)snprintf(text, text_size, "STATUS CUSTOM OK=RUN");
    }
    else if (MENU_ITEM_READONLY == item->type || !menu_item_is_editable(item))
    {
        (void)snprintf(text, text_size, "STATUS VIEW ONLY");
    }
    else
    {
        (void)snprintf(text, text_size, "STATUS READY OK=EDIT");
    }
}

/*
 * 按一个方向调整数值。
 *
 * 枚举按 enum_values 的顺序循环；普通数值按当前步长调整并限幅；多档
 * 步进的档位由 edit_step_index 选择。最终是否接受由 menu_write_value 决定。
 */
/*********************************************************************************************************************
 * @brief  : 按方向和步长调整当前数值
 * @param  : item  菜单项描述
 * @param  : menu  菜单运行时对象
 * @param  : direction  调整方向，正数增加，负数减少
 * @return : true 表示调整并写入成功，false 表示失败
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static bool menu_set_numeric_value(const MenuItem *item, Menu *menu, int direction)
{
    const MenuValueItem *v = menu_value_item(item);
    MenuValue current;
    MenuValue next;
    double amount;

    if ((NULL == v) || !menu_read_value(item, &current))
    {
        return false;
    }
    next = current;
    if (MENU_VALUE_BOOL == v->type)
    {
        next.as.boolean = (direction > 0) ? true : false;
    }
    else if (MENU_VALUE_ENUM == v->type)
    {
        uint8_t i;
        uint8_t current_index = 0U;
        if ((NULL == v->enum_values) || (0U == v->enum_count))
        {
            return false;
        }
        for (i = 0U; i < v->enum_count; ++i)
        {
            if (v->enum_values[i] == current.as.enumeration)
            {
                current_index = i;
                break;
            }
        }
        if (direction > 0)
        {
            current_index = (uint8_t)((current_index + 1U) % v->enum_count);
        }
        else if (0U == current_index)
        {
            current_index = (uint8_t)(v->enum_count - 1U);
        }
        else
        {
            --current_index;
        }
        next.as.enumeration = v->enum_values[current_index];
    }
    else
    {
        if ((NULL != v->steps) && (0U != v->step_count))
        {
            amount = v->steps[(menu->edit_step_index < v->step_count) ?
                              menu->edit_step_index : (uint8_t)(v->step_count - 1U)];
        }
        else
        {
            amount = v->step;
        }
        if (MENU_VALUE_I8 == v->type || MENU_VALUE_I16 == v->type || MENU_VALUE_I32 == v->type)
        {
            double current_number = (MENU_VALUE_I8 == v->type) ? (double)current.as.i8 :
                                     (MENU_VALUE_I16 == v->type) ? (double)current.as.i16 : (double)current.as.i32;
            double n = current_number + ((direction > 0) ? amount : -amount);
            if (n < v->minimum) n = v->minimum;
            if (n > v->maximum) n = v->maximum;
            if (MENU_VALUE_I8 == v->type) next.as.i8 = (int8_t)n;
            else if (MENU_VALUE_I16 == v->type) next.as.i16 = (int16_t)n;
            else next.as.i32 = (int32_t)n;
        }
        else if (MENU_VALUE_U8 == v->type || MENU_VALUE_U16 == v->type || MENU_VALUE_U32 == v->type)
        {
            double current_number = (MENU_VALUE_U8 == v->type) ? (double)current.as.u8 :
                                     (MENU_VALUE_U16 == v->type) ? (double)current.as.u16 : (double)current.as.u32;
            double n = current_number + ((direction > 0) ? amount : -amount);
            if (n < v->minimum) n = v->minimum;
            if (n > v->maximum) n = v->maximum;
            if (MENU_VALUE_U8 == v->type) next.as.u8 = (uint8_t)n;
            else if (MENU_VALUE_U16 == v->type) next.as.u16 = (uint16_t)n;
            else next.as.u32 = (uint32_t)n;
        }
        else if (MENU_VALUE_FLOAT == v->type)
        {
            double n = (double)current.as.f32 + ((direction > 0) ? amount : -amount);
            if (n < v->minimum) n = v->minimum;
            if (n > v->maximum) n = v->maximum;
            next.as.f32 = (float)n;
        }
        else
        {
            return false;
        }
    }
    return menu_write_value(item, &next);
}

/* 绘制一行普通项或把 CUSTOM 行交给应用自己的 draw 回调。 */
/*********************************************************************************************************************
 * @brief  : 绘制一行菜单项或自定义菜单项
 * @param  : menu  菜单运行时对象
 * @param  : item  菜单项描述
 * @param  : row  菜单行下标
 * @param  : selected  是否为当前选中行
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void menu_draw_item(Menu *menu, const MenuItem *item, uint8_t row, bool selected)
{
    char value[MENU_MAX_TEXT];
    char line[MENU_MAX_TEXT * 2U];
    uint16_t y;

    if ((NULL == menu) || (NULL == item) || (NULL == menu->display) ||
        ((NULL == menu->display->text) && (NULL == menu->display->row)))
    {
        return;
    }
    y = (uint16_t)(menu->layout.title_height +
                   menu_view_height(menu) +
                   row * menu->layout.row_height);
    if (MENU_ITEM_CUSTOM == item->type)
    {
        if (NULL != menu->display->row)
        {
            menu->display->row(menu->display_user, 0U, y, menu->layout.width,
                               menu->layout.row_height, item->label, selected);
        }
        if (NULL != item->data.custom.draw)
        {
            MenuCanvas canvas = { menu->display, menu->display_user,
                                  0U, y, menu->layout.width, menu->layout.row_height };
            item->data.custom.draw(item->data.custom.user, &canvas, selected);
        }
        return;
    }
    if (menu_item_is_value(item))
    {
        menu_format_value(item, value, sizeof(value));
        (void)snprintf(line, sizeof(line), "%s: %s", item->label, value);
    }
    else
    {
        (void)snprintf(line, sizeof(line), "%s", item->label);
    }
    if (NULL != menu->display->row)
    {
        menu->display->row(menu->display_user, 0U, y, menu->layout.width,
                           menu->layout.row_height, line, selected);
    }
    else if (NULL != menu->display->text)
    {
        if (selected && NULL != menu->display->rect)
        {
            menu->display->rect(menu->display_user, 0U, y, (uint16_t)(menu->layout.width - 1U),
                                (uint16_t)(menu->layout.row_height - 1U));
        }
        menu->display->text(menu->display_user, 2U, (uint16_t)(y + 2U), line);
    }
}

/*
 * 返回父页。
 *
 * 先询问 can_leave，再调用当前页 leave，最后恢复父页的选择和滚动位置。
 * 因此业务清理只发生在真正离页时，退出被拒绝时不会误清理硬件状态。
 */
/*********************************************************************************************************************
 * @brief  : 退出当前页面并恢复父页面状态
 * @param  : menu  菜单运行时对象
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void menu_leave_page(Menu *menu)
{
    if ((NULL == menu) || (NULL == menu->page) || (0U == menu->depth))
    {
        return;
    }
    if ((NULL != menu->page->hooks) && (NULL != menu->page->hooks->can_leave) &&
        !menu->page->hooks->can_leave(menu->page->hooks->user))
    {
        return;
    }
    if ((NULL != menu->page->hooks) && (NULL != menu->page->hooks->leave))
    {
        menu->page->hooks->leave(menu->page->hooks->user);
    }
    --menu->depth;
    menu->page = menu->stack_pages[menu->depth];
    menu->selection = menu->stack_selection[menu->depth];
    menu->first_visible = menu->stack_first_visible[menu->depth];
    menu->editing = 0U;
    menu->edit_step_index = 0U;
    menu_request_redraw(menu);
}

/* 保存父页会话并进入子页；父页的周期任务仍由 menu_tick 继续维护。 */
/*********************************************************************************************************************
 * @brief  : 保存当前页面并进入子页面
 * @param  : menu  菜单运行时对象
 * @param  : page  参数 page
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void menu_enter_submenu(Menu *menu, const MenuPage *page)
{
    if ((NULL == menu) || (NULL == page) || (menu->depth >= MENU_MAX_STACK - 1U))
    {
        return;
    }
    menu->stack_pages[menu->depth] = menu->page;
    menu->stack_selection[menu->depth] = menu->selection;
    menu->stack_first_visible[menu->depth] = menu->first_visible;
    ++menu->depth;
    menu->page = page;
    menu->selection = 0U;
    menu->first_visible = 0U;
    menu->editing = 0U;
    menu->edit_step_index = 0U;
    if ((NULL != page->hooks) && (NULL != page->hooks->enter))
    {
        page->hooks->enter(page->hooks->user);
    }
    menu_request_redraw(menu);
}

/* 初始化运行时对象。Menu 对象本身由应用分配，核心不申请堆内存。 */
/*********************************************************************************************************************
 * @brief  : 初始化菜单运行时对象并进入根页面
 * @param  : menu  菜单运行时对象
 * @param  : root  根页面地址
 * @param  : display  显示设备适配回调表
 * @param  : display_user  传给显示回调的用户上下文
 * @param  : layout  菜单布局参数，可传 NULL 使用默认值
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_init(Menu *menu, const MenuPage *root, const MenuDisplayOps *display, void *display_user,
               const MenuLayout *layout)
{
    if ((NULL == menu) || (NULL == root))
    {
        return;
    }
    (void)memset(menu, 0, sizeof(*menu));
    menu->root = root;
    menu->page = root;
    menu->display = display;
    menu->display_user = display_user;
    menu->layout.width = 240U;
    menu->layout.height = 320U;
    menu->layout.title_height = 24U;
    menu->layout.row_height = 20U;
    menu->layout.footer_height = 18U;
    menu->layout.wrap_navigation = 1U;
    if (NULL != layout)
    {
        menu->layout = *layout;
    }
    menu->initialized = 1U;
    if ((NULL != root->hooks) && (NULL != root->hooks->enter))
    {
        root->hooks->enter(root->hooks->user);
    }
    menu_request_redraw(menu);
}

/* 浮点按位比较：NaN 不等于自身，用 == 比较会让该行每 tick 都重画。 */
/*********************************************************************************************************************
 * @brief  : 比较两个浮点值的二进制表示是否相同
 * @param  : a  第一个值
 * @param  : b  第二个值
 * @return : true 表示按位相同
 * @date   : 2026年9月15日
 * @author : Jx116
 *********************************************************************************************************************/
static bool menu_live_float_equal(float a, float b)
{
    uint32_t bits_a;
    uint32_t bits_b;
    (void)memcpy(&bits_a, &a, sizeof(bits_a));
    (void)memcpy(&bits_b, &b, sizeof(bits_b));
    return bits_a == bits_b;
}

/* 只比较当前有效的联合体成员；不用 memcmp，避免结构体填充字节造成误判。 */
/*********************************************************************************************************************
 * @brief  : 比较两次读取到的菜单值是否相同
 * @param  : a  上一次的值
 * @param  : b  本次的值
 * @return : true 表示值未变化
 * @date   : 2026年9月15日
 * @author : Jx116
 *********************************************************************************************************************/
static bool menu_live_value_equal(const MenuValue *a, const MenuValue *b)
{
    if ((NULL == a) || (NULL == b) || (a->type != b->type))
    {
        return false;
    }
    switch (a->type)
    {
        case MENU_VALUE_BOOL:  return a->as.boolean == b->as.boolean;
        case MENU_VALUE_I8:    return a->as.i8 == b->as.i8;
        case MENU_VALUE_U8:    return a->as.u8 == b->as.u8;
        case MENU_VALUE_I16:   return a->as.i16 == b->as.i16;
        case MENU_VALUE_U16:   return a->as.u16 == b->as.u16;
        case MENU_VALUE_I32:   return a->as.i32 == b->as.i32;
        case MENU_VALUE_U32:   return a->as.u32 == b->as.u32;
        case MENU_VALUE_FLOAT: return menu_live_float_equal(a->as.f32, b->as.f32);
        case MENU_VALUE_ENUM:  return a->as.enumeration == b->as.enumeration;
        /* 两次都取不到值属于同一状态，不需要重复重画。 */
        case MENU_VALUE_UNAVAILABLE: return true;
        default:               return false;
    }
}

/*
 * 扫一遍可见行，标记值发生变化的实时行。
 *
 * 只处理可见行：不可见行的值不比较，也不产生重画请求。比较原始值而不是
 * 格式化文本，是为了在值没变时完全不执行昂贵的浮点 snprintf。
 *
 * 取值失败也要参与比较：从“有值”变成“取不到”必须重画成 "?"，否则屏幕上
 * 会一直留着已经过期的旧值，把过期数据当成实时数据显示。
 */
/*********************************************************************************************************************
 * @brief  : 比较可见实时行的值并标记需要重画的行
 * @param  : menu  菜单运行时对象
 * @return : 无
 * @date   : 2026年9月15日
 * @author : Jx116
 *********************************************************************************************************************/
static void menu_live_scan(Menu *menu)
{
    uint8_t rows;
    uint8_t row;

    if ((NULL == menu) || (NULL == menu->page) || (0U == menu->page->item_count))
    {
        return;
    }
    rows = menu_visible_rows(menu);
    if (rows > (uint8_t)MENU_MAX_LIVE_ROWS) rows = (uint8_t)MENU_MAX_LIVE_ROWS;
    for (row = 0U; row < rows; ++row)
    {
        uint8_t index = (uint8_t)(menu->first_visible + row);
        const MenuItem *item;
        MenuValue value;

        if (index >= menu->page->item_count) break;
        item = &menu->page->items[index];
        if (MENU_ITEM_LIVE != item->type) continue;
        if (!menu_read_value(item, &value))
        {
            /* 失败时联合体内容无意义，清零后再标记，避免把未初始化字节写进缓存。 */
            (void)memset(&value, 0, sizeof(value));
            value.type = MENU_VALUE_UNAVAILABLE;
        }
        if ((menu->live_rows[row].item == item) && menu_live_value_equal(&menu->live_rows[row].value, &value))
        {
            continue;
        }
        menu->live_rows[row].item = item;
        menu->live_rows[row].value = value;
        menu->live_dirty_mask |= (uint16_t)(1U << row);
    }
}

/*
 * 周期入口。
 *
 * 先 tick 页面栈中的父页，再 tick 当前页，保证父页面会话不会因为进入
 * 子页而停止；随后扫描可见的实时数值行，值有变化就请求局部重画；最后
 * 按 redraw_requested 决定是否输出一整帧。实时数值行与页面钩子无关，
 * 普通列表页同样会被扫描。
 */
/*********************************************************************************************************************
 * @brief  : 推进页面周期任务并按需刷新显示
 * @param  : menu  菜单运行时对象
 * @param  : now_ms  当前毫秒时间戳
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_tick(Menu *menu, uint32_t now_ms)
{
    uint8_t i;
    if ((NULL == menu) || (0U == menu->initialized) || (NULL == menu->page))
    {
        return;
    }
    menu->now_ms = now_ms;
    for (i = 0U; i < menu->depth; ++i)
    {
        const MenuPageHooks *hooks = menu->stack_pages[i]->hooks;
        if ((NULL != hooks) && (NULL != hooks->tick)) hooks->tick(hooks->user, now_ms);
    }
    if ((NULL != menu->page->hooks) && (NULL != menu->page->hooks->tick))
    {
        const MenuPageHooks *hooks = menu->page->hooks;
        bool view_dirty;

        hooks->tick(hooks->user, now_ms);
        view_dirty = (NULL == hooks->is_dirty) ||
                     hooks->is_dirty(hooks->user);
        if (MENU_REDRAW_FULL == menu->redraw_mode)
        {
            menu->redraw_requested = 1U;
        }
        else if (MENU_REDRAW_PARTIAL == menu->redraw_mode)
        {
            if (view_dirty) menu->view_redraw_requested = 1U;
            menu->redraw_requested = 1U;
        }
        else if (view_dirty)
        {
            menu->redraw_mode = MENU_REDRAW_VIEW;
            menu->redraw_page = menu->page;
            menu->redraw_requested = 1U;
        }
    }
    /* 实时数值行与页面钩子无关：普通列表页同样能显示实时数据。 */
    menu_live_scan(menu);
    if (0U != menu->live_dirty_mask)
    {
        menu_request_partial_redraw(menu, menu->selection, menu->first_visible, false);
    }
    if (0U != menu->redraw_requested)
    {
        menu_render(menu);
    }
}

/*
 * 事件分发顺序：页面钩子优先，其次是通用导航/编辑。
 * 页面钩子返回 HANDLED 时，通用层不会再次处理同一个事件，避免启动、
 * 取消或确认动作被执行两次。
 */
/*********************************************************************************************************************
 * @brief  : 分发并处理一个菜单语义按键事件
 * @param  : menu  菜单运行时对象
 * @param  : event  已经完成消抖和分类的菜单事件
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_handle_event(Menu *menu, const MenuEvent *event)
{
    const MenuItem *item;
    MenuEventResult result;

    if ((NULL == menu) || (0U == menu->initialized) || (NULL == event) || (NULL == menu->page))
    {
        return;
    }
    if ((NULL != menu->page->hooks) && (NULL != menu->page->hooks->event))
    {
        result = menu->page->hooks->event(menu->page->hooks->user, event);
        if (MENU_EVENT_HANDLED == result || MENU_EVENT_REQUEST_REDRAW == result)
        {
            menu_request_redraw(menu);
            return;
        }
        if (MENU_EVENT_REQUEST_BACK == result)
        {
            menu_leave_page(menu);
            return;
        }
    }
    if ((MENU_KEY_UP == event->key) && (MENU_KEY_SHORT == event->press))
    {
        if (0U == menu->editing)
        {
            menu_move_selection(menu, -1);
        }
        else
        {
            (void)menu_set_numeric_value(&menu->page->items[menu->selection], menu, -1);
            menu_request_partial_redraw(menu, menu->selection, menu->first_visible, true);
        }
        return;
    }
    if ((MENU_KEY_DOWN == event->key) && (MENU_KEY_SHORT == event->press))
    {
        if (0U == menu->editing)
        {
            menu_move_selection(menu, 1);
        }
        else
        {
            (void)menu_set_numeric_value(&menu->page->items[menu->selection], menu, 1);
            menu_request_partial_redraw(menu, menu->selection, menu->first_visible, true);
        }
        return;
    }
    if ((MENU_KEY_LEFT == event->key || MENU_KEY_RIGHT == event->key) &&
        (0U != menu->editing) && (MENU_KEY_LONG != event->press))
    {
        (void)menu_set_numeric_value(&menu->page->items[menu->selection], menu,
                                     (MENU_KEY_RIGHT == event->key) ? 1 : -1);
        menu_request_partial_redraw(menu, menu->selection, menu->first_visible, true);
        return;
    }
    if ((MENU_KEY_AUX1 == event->key) && (0U != menu->editing) && (MENU_KEY_SHORT == event->press))
    {
        const MenuValueItem *v = menu_value_item(&menu->page->items[menu->selection]);
        if ((NULL != v) && (v->step_count > 1U))
        {
            menu->edit_step_index = (uint8_t)((menu->edit_step_index + 1U) % v->step_count);
            menu->footer_redraw_requested = 1U;
            menu_request_partial_redraw(menu, menu->selection, menu->first_visible, true);
        }
        return;
    }
    if (MENU_KEY_BACK == event->key)
    {
        if (0U != menu->editing)
        {
            menu->editing = 0U;
            menu->edit_step_index = 0U;
            menu->footer_redraw_requested = 1U;
            menu_request_partial_redraw(menu, menu->selection, menu->first_visible, true);
        }
        else
        {
            menu_leave_page(menu);
        }
        return;
    }
    if ((MENU_KEY_OK != event->key) || (MENU_KEY_SHORT != event->press))
    {
        return;
    }
    if ((0U == menu->page->item_count) || (menu->selection >= menu->page->item_count))
    {
        return;
    }
    item = &menu->page->items[menu->selection];
    if (MENU_ITEM_SUBMENU == item->type)
    {
        menu_enter_submenu(menu, item->data.submenu);
    }
    else if (MENU_ITEM_ACTION == item->type)
    {
        if (NULL != item->data.action.action)
        {
            item->data.action.action(item->data.action.user);
        }
        /* 动态页面的 Action 通常只改变实时视图状态；保留列表和静态
         * 图形，交给页面的 dirty/update 回调做局部刷新。 */
        if ((NULL != menu->page->hooks) && (NULL != menu->page->hooks->update))
        {
            menu_request_partial_redraw(menu, menu->selection, menu->first_visible, true);
            menu->view_redraw_requested = 1U;
        }
        else
        {
            menu_request_redraw(menu);
        }
    }
    else if (menu_item_is_editable(item))
    {
        if (0U != menu->editing)
        {
            const MenuValueItem *v = menu_value_item(item);
            if ((NULL != v) && (NULL != v->steps) && (v->step_count > 1U))
            {
                menu->edit_step_index = (uint8_t)((menu->edit_step_index + 1U) % v->step_count);
                menu->footer_redraw_requested = 1U;
                menu_request_partial_redraw(menu, menu->selection, menu->first_visible, true);
            }
            /* 编辑态短按只负责切换步长；参数确认/退出由长按 BACK 事件完成。 */
            return;
        }
        menu->editing = (uint8_t)!menu->editing;
        menu->edit_step_index = 0U;
        menu->footer_redraw_requested = 1U;
        menu_request_partial_redraw(menu, menu->selection, menu->first_visible, true);
    }
    else if (MENU_ITEM_CUSTOM == item->type && NULL != item->data.custom.event)
    {
        result = item->data.custom.event(item->data.custom.user, event);
        if (MENU_EVENT_REQUEST_BACK == result) menu_leave_page(menu);
        else if (MENU_EVENT_REQUEST_REDRAW == result) menu_request_redraw(menu);
        else
        {
            menu_request_partial_redraw(menu, menu->selection, menu->first_visible, true);
        }
    }
}

/*********************************************************************************************************************
 * @brief  : 清空一行菜单显示区域
 * @param  : menu  菜单运行时对象
 * @param  : row  菜单行下标
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
static void menu_draw_empty_row(Menu *menu, uint8_t row)
{
    uint16_t y;
    if ((NULL == menu) || (NULL == menu->display) || (NULL == menu->display->row))
    {
        return;
    }
    y = (uint16_t)(menu->layout.title_height + menu_view_height(menu) +
                   row * menu->layout.row_height);
    menu->display->row(menu->display_user, 0U, y, menu->layout.width, menu->layout.row_height, "", false);
}

/* 清掉某一可见行的实时刷新标记；行号越界时忽略。 */
/*********************************************************************************************************************
 * @brief  : 清除某一行待重画的实时数值标记
 * @param  : menu  菜单运行时对象
 * @param  : row  可见行下标
 * @return : 无
 * @date   : 2026年9月15日
 * @author : Jx116
 *********************************************************************************************************************/
static void menu_live_clear_row(Menu *menu, uint8_t row)
{
    if ((NULL == menu) || (row >= (uint8_t)MENU_MAX_LIVE_ROWS))
    {
        return;
    }
    menu->live_dirty_mask &= (uint16_t)~(uint16_t)(1U << row);
}

/*
 * 只重画被标记的实时行。
 *
 * 整屏重画和滚动重画已经覆盖全部可见行，会先把位图清掉；因此这里通常
 * 只处理“值变了但布局没变”的情况，开销与真正变化的行数成正比。
 */
/*********************************************************************************************************************
 * @brief  : 重画所有值发生变化且仍在可见窗口内的实时数值行
 * @param  : menu  菜单运行时对象
 * @return : 无
 * @date   : 2026年9月15日
 * @author : Jx116
 *********************************************************************************************************************/
static void menu_render_live_rows(Menu *menu)
{
    uint16_t mask;
    uint8_t rows;
    uint8_t row;

    if ((NULL == menu) || (NULL == menu->page) || (0U == menu->live_dirty_mask))
    {
        return;
    }
    mask = menu->live_dirty_mask;
    menu->live_dirty_mask = 0U;
    rows = menu_visible_rows(menu);
    if (rows > (uint8_t)MENU_MAX_LIVE_ROWS) rows = (uint8_t)MENU_MAX_LIVE_ROWS;
    for (row = 0U; row < rows; ++row)
    {
        uint8_t index;

        if (0U == (mask & (uint16_t)(1U << row))) continue;
        index = (uint8_t)(menu->first_visible + row);
        if (index >= menu->page->item_count) continue;
        if (MENU_ITEM_LIVE != menu->page->items[index].type) continue;
        menu_draw_item(menu, &menu->page->items[index], row, (index == menu->selection));
    }
}

/* 按当前状态重画；导航时只重画变化的行，页面变化时才整屏重画，
 * 最后补画值发生变化的实时数值行。 */
/*********************************************************************************************************************
 * @brief  : 按当前状态执行菜单绘制
 * @param  : menu  菜单运行时对象
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_render(Menu *menu)
{
    uint8_t row;
    uint8_t index;
    uint16_t view_height = 0U;
    char title[MENU_MAX_TEXT];
    char footer[MENU_MAX_TEXT];
    bool full_redraw;
    bool view_redraw;
    bool region_full_redraw;

    if ((NULL == menu) || (0U == menu->initialized) || (NULL == menu->display))
    {
        return;
    }
    view_redraw = (MENU_REDRAW_VIEW == menu->redraw_mode) ||
                  (0U != menu->view_redraw_requested);
    full_redraw = ((MENU_REDRAW_PARTIAL != menu->redraw_mode) && !view_redraw) ||
                  (NULL == menu->display->row) ||
                  (menu->redraw_page != menu->page) ||
                  (view_redraw && (NULL == menu->display->clear_region));
    if (full_redraw) view_redraw = false;
    region_full_redraw = full_redraw &&
                         (NULL != menu->display->clear_region) &&
                         (NULL != menu->display->row);
    if (NULL != menu->display->begin) menu->display->begin(menu->display_user);
    if (region_full_redraw)
    {
        /* 页面切换时标题/页脚文字长度可能变化，先清掉各自的矩形区域；
         * 已有菜单行会由 row() 整行覆盖，避免先做一次完整 240x320 清屏。 */
        menu->display->clear_region(menu->display_user, 0U, 0U,
                                     menu->layout.width, menu->layout.title_height);
    }
    else if (full_redraw && NULL != menu->display->clear)
    {
        menu->display->clear(menu->display_user);
    }
    if (full_redraw && (NULL != menu->display->text) && (NULL != menu->page))
    {
        (void)snprintf(title, sizeof(title), "%s", menu->page->title);
        menu->display->text(menu->display_user, 2U, 10U, title);
    }
    if (full_redraw && (NULL != menu->page) && (NULL != menu->page->hooks))
    {
        view_height = menu_view_height(menu);
        if (view_height > 0U)
        {
            if (region_full_redraw)
            {
                menu->display->clear_region(menu->display_user, 0U, menu->layout.title_height,
                                             menu->layout.width, view_height);
            }
            if (NULL != menu->page->hooks->draw)
            {
                MenuCanvas canvas = { menu->display, menu->display_user,
                                      0U, menu->layout.title_height,
                                      menu->layout.width, view_height };
                menu->page->hooks->draw(menu->page->hooks->user, &canvas);
            }
        }
    }
    else if (view_redraw && (NULL != menu->page) && (NULL != menu->page->hooks))
    {
        view_height = menu_view_height(menu);
        if (view_height > 0U)
        {
            if ((NULL != menu->page->hooks->update) && (NULL != menu->display->clear_region))
            {
                MenuCanvas canvas = { menu->display, menu->display_user,
                                      0U, menu->layout.title_height,
                                      menu->layout.width, view_height };
                menu->page->hooks->update(menu->page->hooks->user, &canvas);
            }
            else
            {
                menu->display->clear_region(menu->display_user, 0U, menu->layout.title_height,
                                             menu->layout.width, view_height);
                if (NULL != menu->page->hooks->draw)
                {
                    MenuCanvas canvas = { menu->display, menu->display_user,
                                          0U, menu->layout.title_height,
                                          menu->layout.width, view_height };
                    menu->page->hooks->draw(menu->page->hooks->user, &canvas);
                }
            }
        }
    }
    if (full_redraw && (NULL != menu->page))
    {
        uint8_t rows = menu_visible_rows(menu);
        uint8_t blank_row = rows;
        for (row = 0U; row < rows; ++row)
        {
            index = (uint8_t)(menu->first_visible + row);
            if (index < menu->page->item_count)
            {
                menu_draw_item(menu, &menu->page->items[index], row, (index == menu->selection));
            }
            else
            {
                blank_row = row;
                break;
            }
        }
        if (region_full_redraw && (blank_row < rows))
        {
            menu->display->clear_region(menu->display_user, 0U,
                (uint16_t)(menu->layout.title_height + view_height +
                           blank_row * menu->layout.row_height), menu->layout.width,
                (uint16_t)((rows - blank_row) * menu->layout.row_height));
        }
        /* 整屏已经重画过所有行，实时数值行的标记无需再处理。 */
        menu->live_dirty_mask = 0U;
    }
    else if ((MENU_REDRAW_PARTIAL == menu->redraw_mode) && (NULL != menu->page) &&
             (menu->redraw_old_first_visible != menu->first_visible))
    {
        uint8_t rows = menu_visible_rows(menu);
        for (row = 0U; row < rows; ++row)
        {
            index = (uint8_t)(menu->first_visible + row);
            if (index < menu->page->item_count)
            {
                menu_draw_item(menu, &menu->page->items[index], row, (index == menu->selection));
            }
            else
            {
                menu_draw_empty_row(menu, row);
            }
        }
        /* 所有可见行都已重画，实时数值行的标记无需再处理。 */
        menu->live_dirty_mask = 0U;
    }
    else if ((MENU_REDRAW_PARTIAL == menu->redraw_mode) && (NULL != menu->page))
    {
        uint8_t rows = menu_visible_rows(menu);
        uint8_t old_selection = menu->redraw_old_selection;
        if ((old_selection != menu->selection) && (old_selection >= menu->first_visible) &&
            (old_selection < (uint8_t)(menu->first_visible + rows)) &&
            (old_selection < menu->page->item_count))
        {
            menu_live_clear_row(menu, (uint8_t)(old_selection - menu->first_visible));
            menu_draw_item(menu, &menu->page->items[old_selection],
                           (uint8_t)(old_selection - menu->first_visible), false);
        }
        if ((menu->selection >= menu->first_visible) &&
            (menu->selection < (uint8_t)(menu->first_visible + rows)) &&
            (menu->selection < menu->page->item_count))
        {
            menu_live_clear_row(menu, (uint8_t)(menu->selection - menu->first_visible));
            menu_draw_item(menu, &menu->page->items[menu->selection],
                           (uint8_t)(menu->selection - menu->first_visible), true);
        }
    }
    /* 布局没变时只补画值变化的实时行；上面两个分支已把覆盖到的行清掉。 */
    menu_render_live_rows(menu);
    if (region_full_redraw)
    {
        menu->display->clear_region(menu->display_user, 0U,
                                     (menu->layout.height > menu->layout.footer_height) ?
                                     (uint16_t)(menu->layout.height - menu->layout.footer_height) : 0U,
                                     menu->layout.width, menu->layout.footer_height);
    }
    if (!full_redraw && (0U != menu->footer_redraw_requested) && (NULL == menu->display->status) &&
        (NULL != menu->display->clear_region))
    {
        menu->display->clear_region(menu->display_user, 0U,
                                    (menu->layout.height > menu->layout.footer_height) ?
                                    (uint16_t)(menu->layout.height - menu->layout.footer_height) : 0U,
                                    menu->layout.width, menu->layout.footer_height);
    }
    if ((full_redraw || (0U != menu->footer_redraw_requested)) &&
        ((NULL != menu->display->status) || (NULL != menu->display->text)))
    {
        uint16_t footer_y = (menu->layout.height > menu->layout.footer_height) ?
                            (uint16_t)(menu->layout.height - menu->layout.footer_height) : 0U;
        menu_format_selected_state(menu, footer, sizeof(footer));
        if (NULL != menu->display->status)
        {
            menu->display->status(menu->display_user, 0U, footer_y,
                                  menu->layout.width, menu->layout.footer_height,
                                  footer);
        }
        else if (NULL != menu->display->text)
        {
            if ((footer_y > 0U) && (NULL != menu->display->line))
            {
                menu->display->line(menu->display_user, 0U, (uint16_t)(footer_y - 2U),
                                    (uint16_t)(menu->layout.width - 1U), (uint16_t)(footer_y - 2U));
            }
            menu->display->text(menu->display_user, 2U, footer_y, footer);
        }
    }
    if (NULL != menu->display->end) menu->display->end(menu->display_user);
    menu->redraw_requested = 0U;
    menu->redraw_mode = MENU_REDRAW_NONE;
    menu->view_redraw_requested = 0U;
    menu->footer_redraw_requested = 0U;
    menu->redraw_page = NULL;
    menu->rendered_page = menu->page;
}

/* 只设置标志，不直接访问显示器，适合在业务回调中调用。 */
/*********************************************************************************************************************
 * @brief  : 请求下一次周期任务执行整屏重绘
 * @param  : menu  菜单运行时对象
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_request_redraw(Menu *menu)
{
    if (NULL != menu)
    {
        menu->redraw_requested = 1U;
        menu->redraw_mode = MENU_REDRAW_FULL;
        menu->view_redraw_requested = 0U;
        menu->footer_redraw_requested = 0U;
        menu->redraw_page = menu->page;
    }
}

/*********************************************************************************************************************
 * @brief  : 查询菜单是否处于数值编辑状态
 * @param  : menu  菜单运行时对象
 * @return : true 表示正在编辑，false 表示正在浏览
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
bool menu_is_editing(const Menu *menu)
{
    return (NULL != menu) && (0U != menu->editing);
}

/*********************************************************************************************************************
 * @brief  : 获取当前菜单页面
 * @param  : menu  菜单运行时对象
 * @return : 当前页面地址，参数无效时返回 NULL
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
const MenuPage *menu_current_page(const Menu *menu)
{
    return (NULL != menu) ? menu->page : NULL;
}

/*********************************************************************************************************************
 * @brief  : 获取当前页面选中项下标
 * @param  : menu  菜单运行时对象
 * @return : 当前选中项下标
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
uint8_t menu_current_selection(const Menu *menu)
{
    return (NULL != menu) ? menu->selection : 0U;
}

/* 以下辅助函数把视图相对坐标转换成屏幕绝对坐标，并安全处理 NULL 回调。 */
/*********************************************************************************************************************
 * @brief  : 在自定义视图中绘制文字
 * @param  : canvas  自定义视图画布
 * @param  : x  横坐标
 * @param  : y  纵坐标
 * @param  : text  文本缓冲区或待绘制文本
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_canvas_text(MenuCanvas *canvas, uint16_t x, uint16_t y, const char *text)
{
    if ((NULL != canvas) && (NULL != canvas->display) && (NULL != canvas->display->text))
        canvas->display->text(canvas->display_user, (uint16_t)(canvas->x + x),
                              (uint16_t)(canvas->y + y), text);
}

/*********************************************************************************************************************
 * @brief  : 在自定义视图中绘制矩形边框
 * @param  : canvas  自定义视图画布
 * @param  : x  横坐标
 * @param  : y  纵坐标
 * @param  : w  宽度
 * @param  : h  高度
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_canvas_rect(MenuCanvas *canvas, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if ((NULL != canvas) && (NULL != canvas->display) && (NULL != canvas->display->rect))
        canvas->display->rect(canvas->display_user, (uint16_t)(canvas->x + x),
                              (uint16_t)(canvas->y + y), w, h);
}

/*********************************************************************************************************************
 * @brief  : 在自定义视图中绘制实心矩形
 * @param  : canvas  自定义视图画布
 * @param  : x  横坐标
 * @param  : y  纵坐标
 * @param  : w  宽度
 * @param  : h  高度
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_canvas_fill_rect(MenuCanvas *canvas, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if ((NULL != canvas) && (NULL != canvas->display) && (NULL != canvas->display->fill_rect))
        canvas->display->fill_rect(canvas->display_user, (uint16_t)(canvas->x + x),
                                   (uint16_t)(canvas->y + y), w, h);
}

/*********************************************************************************************************************
 * @brief  : 在自定义视图中绘制直线
 * @param  : canvas  自定义视图画布
 * @param  : x0  起点横坐标
 * @param  : y0  起点纵坐标
 * @param  : x1  终点横坐标
 * @param  : y1  终点纵坐标
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_canvas_line(MenuCanvas *canvas, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    if ((NULL != canvas) && (NULL != canvas->display) && (NULL != canvas->display->line))
        canvas->display->line(canvas->display_user, (uint16_t)(canvas->x + x0),
                              (uint16_t)(canvas->y + y0), (uint16_t)(canvas->x + x1),
                              (uint16_t)(canvas->y + y1));
}

/*********************************************************************************************************************
 * @brief  : 在自定义视图中绘制图像
 * @param  : canvas  自定义视图画布
 * @param  : x  横坐标
 * @param  : y  纵坐标
 * @param  : w  宽度
 * @param  : h  高度
 * @param  : pixels  图像像素数据地址
 * @return : 无
 * @date   : 2026年9月14日
 * @author : Jx116
 *********************************************************************************************************************/
void menu_canvas_image(MenuCanvas *canvas, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void *pixels)
{
    if ((NULL != canvas) && (NULL != canvas->display) && (NULL != canvas->display->image))
        canvas->display->image(canvas->display_user, (uint16_t)(canvas->x + x),
                               (uint16_t)(canvas->y + y), w, h, pixels);
}
