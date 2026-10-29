/* 用 uinput 造一个虚拟键盘，替人敲一次登录密码 + 回车。
 * 只在设备本地跑（root），不碰任何系统配置：用完就把虚拟设备销毁。 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fd = -1;

static void emit(unsigned type, unsigned code, int value) {
    struct input_event event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.code = code;
    event.value = value;
    if (write(fd, &event, sizeof(event)) < 0) perror("write");
}

static void tap(int code) {
    emit(EV_KEY, (unsigned)code, 1);
    emit(EV_SYN, SYN_REPORT, 0);
    usleep(60000);
    emit(EV_KEY, (unsigned)code, 0);
    emit(EV_SYN, SYN_REPORT, 0);
    usleep(60000);
}

static int code_for(char character) {
    if (character >= '1' && character <= '9') return KEY_1 + (character - '1');
    if (character == '0') return KEY_0;
    if (character >= 'a' && character <= 'z') return KEY_A + (character - 'a');
    if (character >= 'A' && character <= 'Z') return KEY_A + (character - 'A');
    if (character == '\n') return KEY_ENTER;
    if (character == ' ') return KEY_SPACE;
    if (character == '.') return KEY_DOT;
    if (character == ',') return KEY_COMMA;
    if (character == '/') return KEY_SLASH;
    if (character == '-') return KEY_MINUS;
    return -1;
}

static int needs_shift(char character) {
    return (character >= 'A' && character <= 'Z');
}

static void move_pointer(int dx, int dy) {
    emit(EV_REL, REL_X, dx);
    emit(EV_REL, REL_Y, dy);
    emit(EV_SYN, SYN_REPORT, 0);
    usleep(30000);
}

/* 先把指针怼到左上角（相对位移会被屏幕边缘截断，等于归零），再移到目标点。
   这样不需要知道指针原来在哪，也不会越界。 */
static void click_at(int x, int y) {
    for (int i = 0; i < 4; i++) move_pointer(-2000, -2000);
    int remaining_x = x, remaining_y = y;
    while (remaining_x > 0 || remaining_y > 0) {
        int step_x = remaining_x > 600 ? 600 : remaining_x;
        int step_y = remaining_y > 600 ? 600 : remaining_y;
        move_pointer(step_x, step_y);
        remaining_x -= step_x;
        remaining_y -= step_y;
    }
    usleep(200000);
    emit(EV_KEY, BTN_LEFT, 1);
    emit(EV_SYN, SYN_REPORT, 0);
    usleep(80000);
    emit(EV_KEY, BTN_LEFT, 0);
    emit(EV_SYN, SYN_REPORT, 0);
    usleep(200000);
}

int main(int argc, char **argv) {
    const char *text = "123456\n";
    int click_x = -1, click_y = -1;
    if (argc > 3 && !strcmp(argv[1], "click")) {
        click_x = atoi(argv[2]);
        click_y = atoi(argv[3]);
        text = argc > 4 ? argv[4] : "";
    } else if (argc > 1) {
        text = argv[1];
    }

    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("open /dev/uinput"); return 1; }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    for (int code = 0; code < KEY_MAX; code++) ioctl(fd, UI_SET_KEYBIT, code);

    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x4c34;
    setup.id.product = 0x1001;
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "l410-login-keyboard");
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) { perror("UI_DEV_SETUP"); return 1; }
    if (ioctl(fd, UI_DEV_CREATE) < 0) { perror("UI_DEV_CREATE"); return 1; }

    /* 等系统把这个虚拟键盘/鼠标枚举进输入栈，再开始操作。 */
    sleep(3);
    if (click_x >= 0) {
        fprintf(stderr, "click at %d,%d\n", click_x, click_y);
        click_at(click_x, click_y);
    }
    for (const char *at = text; *at; at++) {
        int code = code_for(*at);
        if (code < 0) continue;
        const int shift = needs_shift(*at);
        if (shift) {
            emit(EV_KEY, KEY_LEFTSHIFT, 1);
            emit(EV_SYN, SYN_REPORT, 0);
        }
        tap(code);
        if (shift) {
            emit(EV_KEY, KEY_LEFTSHIFT, 0);
            emit(EV_SYN, SYN_REPORT, 0);
        }
        fprintf(stderr, "typed %c (code %d)\n", *at == '\n' ? ' ' : *at, code);
    }
    sleep(1);
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    printf("INJECT_DONE\n");
    return 0;
}
