/* 输入通道协议：宿主输入服务与容器侧输入服务之间（窗口级，不是控件级）。
 *
 * 逐条对应 anco 的输入路径与 SurfaceViewListenerStub 的窗口生命周期：
 *   宿主 -> 容器：焦点/几何/指针/按键/滚轮/触摸、输入法状态（启用、光标矩形、键盘显隐）
 *   容器 -> 宿主：窗口就绪、输入法请求（显示/隐藏键盘、光标矩形更新）、文本上屏结果
 *
 * 关键点（照抄 anco 的分工）：
 *   - 宿主只做"窗口级"投递：告诉容器哪个窗口有焦点、指针在哪个窗口的什么位置、按了什么键；
 *   - 应用内部的命中测试、控件交互、文本编辑由容器里应用自己的输入栈完成；
 *   - 软键盘与候选窗由宿主输入法服务绘制，位置由容器上报的"光标矩形"决定。
 */
#ifndef L410_INPUT_CHANNEL_H
#define L410_INPUT_CHANNEL_H

#include <stdint.h>

enum {
    /* 宿主 -> 容器：窗口级输入 */
    L410_INPUT_FOCUS        = 1,   /* FOCUS window= active= */
    L410_INPUT_GEOMETRY     = 2,   /* GEOMETRY window= x= y= w= h= */
    L410_INPUT_POINTER      = 3,   /* POINTER action=down|move|up window= x= y= button= mods= */
    L410_INPUT_WHEEL        = 4,   /* WHEEL window= x= y= delta= horizontal= mods= */
    L410_INPUT_KEY          = 5,   /* KEY action=down|up window= code= mods= text=<utf8> */
    L410_INPUT_TOUCH        = 6,   /* TOUCH action=down|move|up id= window= x= y= */
    L410_INPUT_CURSOR_ENTER = 7,   /* CURSOR_ENTER window= x= y= */
    L410_INPUT_CURSOR_LEAVE = 8,   /* CURSOR_LEAVE window= */
    L410_INPUT_CLOSE        = 9,   /* CLOSE window=     （宿主标题栏关闭按钮） */
    L410_INPUT_DPI          = 10,  /* DPI density= scale= */
    L410_INPUT_DEVICE       = 11,  /* DEVICE name=<evdev 节点> action=add|remove */

    /* 宿主 -> 容器：输入法状态（由宿主输入法服务驱动） */
    L410_INPUT_IME_ENABLE   = 32,  /* IME_ENABLE window= on=  */
    L410_INPUT_IME_KEYBOARD = 33,  /* IME_KEYBOARD window= visible= height= */
    L410_INPUT_IME_COMMIT   = 34,  /* IME_COMMIT window= text=<utf8> */
    L410_INPUT_IME_PREEDIT  = 35,  /* IME_PREEDIT window= text=<utf8> cursor= */
    L410_INPUT_CLIPBOARD    = 36,  /* CLIPBOARD action=set|clear text=<utf8> */

    /* 容器 -> 宿主 */
    L410_INPUT_WINDOW_READY = 64,  /* WINDOW_READY window= */
    L410_INPUT_IME_REQUEST  = 65,  /* IME_REQUEST window= action=show|hide */
    L410_INPUT_IME_CURSOR   = 66,  /* IME_CURSOR window= x= y= w= h=   （应用侧上报的光标矩形，宿主据此放候选窗） */
    L410_INPUT_IME_STATE    = 67,  /* IME_STATE window= enabled= type=  （文本配置：普通/数字/密码/长度） */
    L410_INPUT_CLIPBOARD_GET = 68, /* CLIPBOARD_GET window=             （请求宿主回送剪贴板内容） */
    L410_INPUT_CLIPBOARD_SET = 69, /* CLIPBOARD_SET text=<utf8> */
};

#endif /* L410_INPUT_CHANNEL_H */
