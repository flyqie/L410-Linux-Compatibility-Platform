/* Generated from the actual original host AnCo MMI key table and reviewed
 * OpenHarmony/Linux application-key names. Linux-side protocol translation;
 * Android keyCode/scanCode is NOT an evdev code. See keymap-source.json and
 * scripts/extract-stock-keymap-v333.py. Unmapped keys return -1. */
#ifndef ANCO_LINUX_KEYMAP_H
#define ANCO_LINUX_KEYMAP_H
#include <stdint.h>
static int anco_linux_keycode(int32_t android)
{
    switch(android){
    case 7:return 11; /* KEYCODE_0 -> KEY_0 */
    case 8:return 2; /* KEYCODE_1 -> KEY_1 */
    case 9:return 3; /* KEYCODE_2 -> KEY_2 */
    case 10:return 4; /* KEYCODE_3 -> KEY_3 */
    case 11:return 5; /* KEYCODE_4 -> KEY_4 */
    case 12:return 6; /* KEYCODE_5 -> KEY_5 */
    case 13:return 7; /* KEYCODE_6 -> KEY_6 */
    case 14:return 8; /* KEYCODE_7 -> KEY_7 */
    case 15:return 9; /* KEYCODE_8 -> KEY_8 */
    case 16:return 10; /* KEYCODE_9 -> KEY_9 */
    case 19:return 103; /* KEYCODE_DPAD_UP -> KEY_UP */
    case 20:return 108; /* KEYCODE_DPAD_DOWN -> KEY_DOWN */
    case 21:return 105; /* KEYCODE_DPAD_LEFT -> KEY_LEFT */
    case 22:return 106; /* KEYCODE_DPAD_RIGHT -> KEY_RIGHT */
    case 23:return 28; /* KEYCODE_DPAD_CENTER -> KEY_ENTER */
    case 29:return 30; /* KEYCODE_A -> KEY_A */
    case 30:return 48; /* KEYCODE_B -> KEY_B */
    case 31:return 46; /* KEYCODE_C -> KEY_C */
    case 32:return 32; /* KEYCODE_D -> KEY_D */
    case 33:return 18; /* KEYCODE_E -> KEY_E */
    case 34:return 33; /* KEYCODE_F -> KEY_F */
    case 35:return 34; /* KEYCODE_G -> KEY_G */
    case 36:return 35; /* KEYCODE_H -> KEY_H */
    case 37:return 23; /* KEYCODE_I -> KEY_I */
    case 38:return 36; /* KEYCODE_J -> KEY_J */
    case 39:return 37; /* KEYCODE_K -> KEY_K */
    case 40:return 38; /* KEYCODE_L -> KEY_L */
    case 41:return 50; /* KEYCODE_M -> KEY_M */
    case 42:return 49; /* KEYCODE_N -> KEY_N */
    case 43:return 24; /* KEYCODE_O -> KEY_O */
    case 44:return 25; /* KEYCODE_P -> KEY_P */
    case 45:return 16; /* KEYCODE_Q -> KEY_Q */
    case 46:return 19; /* KEYCODE_R -> KEY_R */
    case 47:return 31; /* KEYCODE_S -> KEY_S */
    case 48:return 20; /* KEYCODE_T -> KEY_T */
    case 49:return 22; /* KEYCODE_U -> KEY_U */
    case 50:return 47; /* KEYCODE_V -> KEY_V */
    case 51:return 17; /* KEYCODE_W -> KEY_W */
    case 52:return 45; /* KEYCODE_X -> KEY_X */
    case 53:return 21; /* KEYCODE_Y -> KEY_Y */
    case 54:return 44; /* KEYCODE_Z -> KEY_Z */
    case 55:return 51; /* KEYCODE_COMMA -> KEY_COMMA */
    case 56:return 52; /* KEYCODE_PERIOD -> KEY_DOT */
    case 57:return 56; /* KEYCODE_ALT_LEFT -> KEY_LEFTALT */
    case 58:return 100; /* KEYCODE_ALT_RIGHT -> KEY_RIGHTALT */
    case 59:return 42; /* KEYCODE_SHIFT_LEFT -> KEY_LEFTSHIFT */
    case 60:return 54; /* KEYCODE_SHIFT_RIGHT -> KEY_RIGHTSHIFT */
    case 61:return 15; /* KEYCODE_TAB -> KEY_TAB */
    case 62:return 57; /* KEYCODE_SPACE -> KEY_SPACE */
    case 66:return 28; /* KEYCODE_ENTER -> KEY_ENTER */
    case 67:return 14; /* KEYCODE_DEL -> KEY_BACKSPACE */
    case 68:return 41; /* KEYCODE_GRAVE -> KEY_GRAVE */
    case 69:return 12; /* KEYCODE_MINUS -> KEY_MINUS */
    case 70:return 13; /* KEYCODE_EQUALS -> KEY_EQUAL */
    case 71:return 26; /* KEYCODE_LEFT_BRACKET -> KEY_LEFTBRACE */
    case 72:return 27; /* KEYCODE_RIGHT_BRACKET -> KEY_RIGHTBRACE */
    case 73:return 43; /* KEYCODE_BACKSLASH -> KEY_BACKSLASH */
    case 74:return 39; /* KEYCODE_SEMICOLON -> KEY_SEMICOLON */
    case 75:return 40; /* KEYCODE_APOSTROPHE -> KEY_APOSTROPHE */
    case 76:return 53; /* KEYCODE_SLASH -> KEY_SLASH */
    case 82:return 139; /* KEYCODE_MENU -> KEY_MENU */
    case 92:return 104; /* KEYCODE_PAGE_UP -> KEY_PAGEUP */
    case 93:return 109; /* KEYCODE_PAGE_DOWN -> KEY_PAGEDOWN */
    case 111:return 1; /* KEYCODE_ESCAPE -> KEY_ESC */
    case 112:return 111; /* KEYCODE_FORWARD_DEL -> KEY_DELETE */
    case 113:return 29; /* KEYCODE_CTRL_LEFT -> KEY_LEFTCTRL */
    case 114:return 97; /* KEYCODE_CTRL_RIGHT -> KEY_RIGHTCTRL */
    case 115:return 58; /* KEYCODE_CAPS_LOCK -> KEY_CAPSLOCK */
    case 116:return 70; /* KEYCODE_SCROLL_LOCK -> KEY_SCROLLLOCK */
    case 117:return 125; /* KEYCODE_META_LEFT -> KEY_LEFTMETA */
    case 118:return 126; /* KEYCODE_META_RIGHT -> KEY_RIGHTMETA */
    case 121:return 119; /* KEYCODE_BREAK -> KEY_PAUSE */
    case 122:return 102; /* KEYCODE_MOVE_HOME -> KEY_HOME */
    case 123:return 107; /* KEYCODE_MOVE_END -> KEY_END */
    case 124:return 110; /* KEYCODE_INSERT -> KEY_INSERT */
    case 131:return 59; /* KEYCODE_F1 -> KEY_F1 */
    case 132:return 60; /* KEYCODE_F2 -> KEY_F2 */
    case 133:return 61; /* KEYCODE_F3 -> KEY_F3 */
    case 134:return 62; /* KEYCODE_F4 -> KEY_F4 */
    case 135:return 63; /* KEYCODE_F5 -> KEY_F5 */
    case 136:return 64; /* KEYCODE_F6 -> KEY_F6 */
    case 137:return 65; /* KEYCODE_F7 -> KEY_F7 */
    case 138:return 66; /* KEYCODE_F8 -> KEY_F8 */
    case 139:return 67; /* KEYCODE_F9 -> KEY_F9 */
    case 140:return 68; /* KEYCODE_F10 -> KEY_F10 */
    case 141:return 87; /* KEYCODE_F11 -> KEY_F11 */
    case 142:return 88; /* KEYCODE_F12 -> KEY_F12 */
    case 143:return 69; /* KEYCODE_NUM_LOCK -> KEY_NUMLOCK */
    case 144:return 82; /* KEYCODE_NUMPAD_0 -> KEY_KP0 */
    case 145:return 79; /* KEYCODE_NUMPAD_1 -> KEY_KP1 */
    case 146:return 80; /* KEYCODE_NUMPAD_2 -> KEY_KP2 */
    case 147:return 81; /* KEYCODE_NUMPAD_3 -> KEY_KP3 */
    case 148:return 75; /* KEYCODE_NUMPAD_4 -> KEY_KP4 */
    case 149:return 76; /* KEYCODE_NUMPAD_5 -> KEY_KP5 */
    case 150:return 77; /* KEYCODE_NUMPAD_6 -> KEY_KP6 */
    case 151:return 71; /* KEYCODE_NUMPAD_7 -> KEY_KP7 */
    case 152:return 72; /* KEYCODE_NUMPAD_8 -> KEY_KP8 */
    case 153:return 73; /* KEYCODE_NUMPAD_9 -> KEY_KP9 */
    case 154:return 98; /* KEYCODE_NUMPAD_DIVIDE -> KEY_KPSLASH */
    case 155:return 55; /* KEYCODE_NUMPAD_MULTIPLY -> KEY_KPASTERISK */
    case 156:return 74; /* KEYCODE_NUMPAD_SUBTRACT -> KEY_KPMINUS */
    case 157:return 78; /* KEYCODE_NUMPAD_ADD -> KEY_KPPLUS */
    case 158:return 83; /* KEYCODE_NUMPAD_DOT -> KEY_KPDOT */
    case 159:return 121; /* KEYCODE_NUMPAD_COMMA -> KEY_KPCOMMA */
    case 160:return 96; /* KEYCODE_NUMPAD_ENTER -> KEY_KPENTER */
    case 161:return 117; /* KEYCODE_NUMPAD_EQUALS -> KEY_KPEQUAL */
    case 162:return 179; /* KEYCODE_NUMPAD_LEFT_PAREN -> KEY_KPLEFTPAREN */
    case 163:return 180; /* KEYCODE_NUMPAD_RIGHT_PAREN -> KEY_KPRIGHTPAREN */
    default:return -1;
    }
}
#endif
