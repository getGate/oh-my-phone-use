/*
 * Host-side tests for the portable modules: command parser, key map, movement splitting and
 * the input-record protocol of the iPad Computer Use input tool.
 * Build and run with `make -C test/host test` (plain clang/gcc, no ESP-IDF needed).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "command_parser.h"
#include "hid_keymap.h"
#include "hid_motion.h"
#include "input_records.h"
#include "mock_hid.h"

static int s_failures;
static int s_checks;

#define CHECK(cond) do { \
        s_checks++; \
        if (!(cond)) { \
            s_failures++; \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

static uint32_t fake_uptime(void)
{
    return 42;
}

/* Run a line through the parser and return the response (static buffer). */
static const char *run(const char *line)
{
    static char response[COMMAND_RESPONSE_MAX];
    size_t n = command_parser_execute(line, response, sizeof(response));
    if (n == 0) {
        strcpy(response, "<none>");
    }
    return response;
}

static void expect(const char *line, const char *want_response, const char *want_calls)
{
    mock_calls[0] = '\0';
    const char *got = run(line);
    s_checks++;
    if (strcmp(got, want_response) != 0) {
        s_failures++;
        printf("FAIL \"%s\": response \"%s\", wanted \"%s\"\n", line, got, want_response);
    }
    if (want_calls) {
        s_checks++;
        if (strcmp(mock_calls, want_calls) != 0) {
            s_failures++;
            printf("FAIL \"%s\": calls \"%s\", wanted \"%s\"\n", line, mock_calls, want_calls);
        }
    }
}

static void test_parser(void)
{
    static const command_platform_t platform = { .uptime_s = fake_uptime };
    command_parser_init(&platform);
    mock_reset();

    /* Liveness / status */
    expect("PING", "PONG", "");
    expect("ping", "PONG", "");
    expect("PING\r\n", "PONG", "");
    expect("  PING  ", "PONG", "");
    expect("STATUS", "STATUS connected=1 advertising=0 bonds=1 buttons=0 sent=17 failed=2 uptime=42", "");

    /* Mouse */
    expect("MOVE 20 -10", "OK", "move(20,-10);");
    expect("move 800 400", "OK", "move(800,400);");
    expect("MOVE   5    6", "OK", "move(5,6);");
    expect("MOVE 32767 -32767", "OK", "move(32767,-32767);");
    expect("MOVE 32768 0", "ERR invalid_args", "");
    expect("MOVE 1 2 3", "ERR invalid_args", "");
    expect("MOVE 1", "ERR invalid_args", "");
    expect("MOVE x y", "ERR invalid_args", "");
    expect("MOVE 1.5 2", "ERR invalid_args", "");
    expect("MOVE", "ERR invalid_args", "");
    expect("CLICK", "OK", "click();");
    expect("CLICK now", "ERR invalid_args", "");
    expect("DOWN", "OK", "down();");
    expect("UP", "OK", "up();");
    expect("SCROLL -100", "OK", "scroll(-100);");
    expect("SCROLL", "ERR invalid_args", "");
    expect("SCROLL 1 2", "ERR invalid_args", "");

    /* Keyboard */
    expect("TYPE hello world", "OK", "type(\"hello world\");");
    expect("TYPE  two leading spaces", "OK", "type(\" two leading spaces\");");
    expect("TYPE line1\\nline2\\ttab\\\\slash", "OK", "type(\"line1\nline2\ttab\\slash\");");
    expect("TYPE", "ERR invalid_args", "");
    expect("TYPE ", "ERR invalid_args", "");
    expect("TYPE caf\xc3\xa9", "ERR unsupported_char", "");
    expect("KEY ENTER", "OK", "press(0x28,0x00);");
    expect("key enter", "OK", "press(0x28,0x00);");
    expect("KEY BACKSPACE", "OK", "press(0x2a,0x00);");
    expect("KEY TAB", "OK", "press(0x2b,0x00);");
    expect("KEY ESC", "OK", "press(0x29,0x00);");
    expect("KEY UP", "OK", "press(0x52,0x00);");
    expect("KEY DOWN", "OK", "press(0x51,0x00);");
    expect("KEY LEFT", "OK", "press(0x50,0x00);");
    expect("KEY RIGHT", "OK", "press(0x4f,0x00);");
    expect("KEY a", "OK", "press(0x04,0x00);");
    expect("KEY A", "OK", "press(0x04,0x02);");
    expect("KEY cmd+h", "OK", "press(0x0b,0x08);");
    expect("KEY SHIFT+TAB", "OK", "press(0x2b,0x02);");
    expect("KEY CTRL+ALT+DELETE", "OK", "press(0x4c,0x05);");
    expect("KEY 0x28", "OK", "press(0x28,0x00);");
    expect("KEY +", "OK", "press(0x2e,0x02);");
    expect("KEY SHIFT++", "OK", "press(0x2e,0x02);");
    expect("KEY SHIFT", "OK", "press(0x00,0x02);");
    expect("KEY BOGUS", "ERR unknown_key", "");
    expect("KEY a+b", "ERR unknown_key", "");
    expect("KEY CMD+", "ERR unknown_key", "");
    expect("KEY", "ERR invalid_args", "");
    expect("KEY ENTER TAB", "ERR invalid_args", "");
    expect("KEYDOWN SHIFT", "OK", "keydown(0x00,0x02);");
    expect("KEYUP SHIFT", "OK", "keyup(0x00,0x02);");

    /* Pairing */
    expect("UNPAIR", "OK", "unpair();");

    /* Unknown / blank */
    expect("FOO", "ERR invalid_command", "");
    expect("FOO 1 2", "ERR invalid_command", "");
    expect("", "<none>", "");
    expect("   \r\n", "<none>", "");

    /* Overlong line */
    char big[COMMAND_LINE_MAX + 10];
    memset(big, 'A', sizeof(big) - 1);
    memcpy(big, "TYPE ", 5);
    big[sizeof(big) - 1] = '\0';
    expect(big, "ERR line_too_long", "");

    /* Not connected: HID commands fail, protocol commands keep working */
    mock_connected = false;
    expect("MOVE 1 1", "ERR not_connected", "move(1,1);");
    expect("TYPE hi", "ERR not_connected", "type(\"hi\");");
    expect("PING", "PONG", "");
    expect("STATUS", "STATUS connected=0 advertising=1 bonds=1 buttons=0 sent=17 failed=2 uptime=42", "");
    mock_connected = true;

    /* Transport failure */
    mock_force_error = ESP_FAIL;
    expect("CLICK", "ERR send_failed", "click();");
    mock_force_error = ESP_OK;

    /* Network commands are unavailable without platform hooks. */
    expect("WIFI", "ERR unsupported", "");
    expect("WIFI MyNet password1", "ERR unsupported", "");
    expect("SECRET", "ERR unsupported", "");
    expect("SECRET abc", "ERR unsupported", "");
    expect("WIFI a b c", "ERR invalid_args", "");
}

/* ---- platform hooks for the WIFI / SECRET / STATUS tests ---------------------------------- */

static char hook_calls[512];
static esp_err_t hook_error;

static void hook_record(const char *fmt, ...)
{
    size_t len = strlen(hook_calls);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(hook_calls + len, sizeof(hook_calls) - len, fmt, ap);
    va_end(ap);
}

static size_t fake_net_status(char *out, size_t size)
{
    return (size_t)snprintf(out, size, " wifi=1 ip=192.168.1.50 player=idle");
}

static size_t fake_wifi_status(char *out, size_t size)
{
    return (size_t)snprintf(out, size, "configured=1 connected=1 ssid=Home ip=192.168.1.50 rssi=-51 hostname=phoneuse.local");
}

static esp_err_t fake_wifi_set(const char *ssid, const char *password)
{
    hook_record("wifi_set(%s,%s);", ssid ? ssid : "<null>", password ? password : "<null>");
    return hook_error;
}

static size_t fake_secret_status(char *out, size_t size)
{
    return (size_t)snprintf(out, size, "configured=1 source=nvs");
}

static esp_err_t fake_secret_set(const char *secret)
{
    hook_record("secret_set(%s);", secret ? secret : "<null>");
    return hook_error;
}

static void expect_hook(const char *line, const char *want_response, const char *want_calls)
{
    hook_calls[0] = '\0';
    expect(line, want_response, "");
    s_checks++;
    if (strcmp(hook_calls, want_calls) != 0) {
        s_failures++;
        printf("FAIL \"%s\": hook calls \"%s\", wanted \"%s\"\n", line, hook_calls, want_calls);
    }
}

static void test_network_commands(void)
{
    static const command_platform_t platform = {
        .uptime_s = fake_uptime,
        .net_status = fake_net_status,
        .wifi_status = fake_wifi_status,
        .wifi_set = fake_wifi_set,
        .secret_status = fake_secret_status,
        .secret_set = fake_secret_set,
    };
    command_parser_init(&platform);
    mock_reset();
    hook_error = ESP_OK;

    expect("STATUS", "STATUS connected=1 advertising=0 bonds=1 buttons=0 sent=17 failed=2 uptime=42"
                     " wifi=1 ip=192.168.1.50 player=idle", "");
    expect("WIFI", "WIFI configured=1 connected=1 ssid=Home ip=192.168.1.50 rssi=-51 hostname=phoneuse.local", "");
    expect("wifi  ", "WIFI configured=1 connected=1 ssid=Home ip=192.168.1.50 rssi=-51 hostname=phoneuse.local", "");

    expect_hook("WIFI MyNet password1", "OK", "wifi_set(MyNet,password1);");
    expect_hook("wifi MyNet password1", "OK", "wifi_set(MyNet,password1);");
    expect_hook("WIFI OpenNet", "OK", "wifi_set(OpenNet,);");
    expect_hook("WIFI \"My Net\" \"pass word 1\"", "OK", "wifi_set(My Net,pass word 1);");
    expect_hook("WIFI \"quo\\\"te\" \"back\\\\slash\"", "OK", "wifi_set(quo\"te,back\\slash);");
    expect_hook("WIFI \"MyNet\" pass1234", "OK", "wifi_set(MyNet,pass1234);");
    expect_hook("WIFI CLEAR", "OK", "wifi_set(<null>,<null>);");
    expect_hook("WIFI clear", "OK", "wifi_set(<null>,<null>);");
    expect_hook("WIFI \"CLEAR\" password1", "OK", "wifi_set(CLEAR,password1);");
    expect_hook("WIFI \"CLEAR\"", "OK", "wifi_set(CLEAR,);");
    expect_hook("WIFI MyNet short", "ERR invalid_args", "");                     /* password < 8 */
    expect_hook("WIFI MyNet 0123456789012345678901234567890123456789012345678901234567890123", "ERR invalid_args", "");
    expect_hook("WIFI 012345678901234567890123456789012 password1", "ERR invalid_args", "");  /* ssid > 32 */
    expect_hook("WIFI \"unterminated", "ERR invalid_args", "");
    expect_hook("WIFI \"a\"b password1", "ERR invalid_args", "");
    expect_hook("WIFI a b c", "ERR invalid_args", "");
    expect_hook("WIFI caf\xc3\xa9 password1", "ERR invalid_args", "");
    hook_error = ESP_ERR_INVALID_ARG;
    expect_hook("WIFI MyNet password1", "ERR invalid_args", "wifi_set(MyNet,password1);");
    hook_error = ESP_FAIL;
    expect_hook("WIFI MyNet password1", "ERR failed", "wifi_set(MyNet,password1);");
    hook_error = ESP_ERR_INVALID_STATE;
    expect_hook("WIFI MyNet password1", "ERR not_ready", "wifi_set(MyNet,password1);");
    hook_error = ESP_OK;

    expect("SECRET", "SECRET configured=1 source=nvs", "");
    expect_hook("SECRET abcDEF123-_", "OK", "secret_set(abcDEF123-_);");
    expect_hook("secret \"with space\"", "OK", "secret_set(with space);");
    expect_hook("SECRET CLEAR", "OK", "secret_set(<null>);");
    expect_hook("SECRET \"CLEAR\"", "OK", "secret_set(CLEAR);");
    expect_hook("SECRET a b", "ERR invalid_args", "");
    expect_hook("SECRET \"open", "ERR invalid_args", "");
    char long_secret[7 + 129 + 1];
    memcpy(long_secret, "SECRET ", 7);
    memset(long_secret + 7, 'x', 129);
    long_secret[7 + 129] = '\0';
    expect_hook(long_secret, "ERR invalid_args", "");
    long_secret[7 + 128] = '\0';
    hook_calls[0] = '\0';
    expect(long_secret, "OK", "");                              /* 128 characters: accepted */
    CHECK(strncmp(hook_calls, "secret_set(xxxx", 15) == 0 && strlen(hook_calls) == 11 + 128 + 2);

    /* The HID commands are unaffected by the extra hooks. */
    expect("PING", "PONG", "");
    expect("CLICK", "OK", "click();");
}

/* ---- input-record protocol ----------------------------------------------------------- */

static void test_input_records(void)
{
    input_record_t r[INPUT_RECORDS_MAX];
    size_t n = 0;

    /* Decoding */
    CHECK(input_records_decode("0100680000", 10, r, &n) == INPUT_HEX_OK && n == 1);
    CHECK(r[0].type == 1 && r[0].a == 0 && r[0].b == 'h' && r[0].c == 0 && r[0].d == 0);
    CHECK(input_records_decode("0201F0FF7F", 10, r, &n) == INPUT_HEX_OK && n == 1);
    CHECK(r[0].type == 2 && r[0].a == 1 && r[0].b == 0xF0 && r[0].c == 0xFF && r[0].d == 0x7F);
    CHECK(input_records_decode("03ee020000" "0200000000", 20, r, &n) == INPUT_HEX_OK && n == 2);
    CHECK(input_record_wait_ms(&r[0]) == 750);
    CHECK(input_records_decode("", 0, r, &n) == INPUT_HEX_BAD_LENGTH && n == 0);
    CHECK(input_records_decode("010068000", 9, r, &n) == INPUT_HEX_BAD_LENGTH);
    CHECK(input_records_decode("01006800000", 11, r, &n) == INPUT_HEX_BAD_LENGTH);
    CHECK(input_records_decode("010068000g", 10, r, &n) == INPUT_HEX_BAD_DIGIT);
    CHECK(input_records_decode("NOT-HEX!!!", 10, r, &n) == INPUT_HEX_BAD_DIGIT);
    static char big[INPUT_HEX_MAX + 10 + 1];
    memset(big, '0', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    CHECK(input_records_decode(big, INPUT_HEX_MAX + 10, r, &n) == INPUT_HEX_BAD_LENGTH);
    for (size_t i = 0; i < INPUT_HEX_MAX; i += 10) {
        memcpy(big + i, "0310270000", 10);              /* 512 waits of 10 s would exceed the total */
    }
    CHECK(input_records_decode(big, INPUT_HEX_MAX, r, &n) == INPUT_HEX_OK && n == INPUT_RECORDS_MAX);
    CHECK(!input_records_validate(r, n));
    for (size_t i = 0; i < INPUT_HEX_MAX; i += 10) {
        memcpy(big + i, "0300000000", 10);
    }
    CHECK(input_records_decode(big, INPUT_HEX_MAX, r, &n) == INPUT_HEX_OK && n == INPUT_RECORDS_MAX);
    CHECK(input_records_validate(r, n));

    /* Validation: port of the reference firmware's input.test.cpp */
    input_record_t seq[] = { {1,0,'h',0,0}, {2,1,0,0,0}, {2,1,120,0,0}, {2,0,0,0,0}, {3,100,0,0,0} };
    CHECK(input_records_validate(seq, 5));
    CHECK(!input_records_validate(seq, 0));
    CHECK(!input_records_validate(seq, 3));                 /* button still held at the end */
    seq[0].a = 16; CHECK(!input_records_validate(seq, 5)); seq[0].a = 0;
    seq[2].b = 128; CHECK(!input_records_validate(seq, 5)); seq[2].b = 120;
    seq[4] = (input_record_t){3,255,255,0,0}; CHECK(!input_records_validate(seq, 5));   /* 65535 ms wait */
    seq[4] = (input_record_t){3,0,0,0,1};     CHECK(!input_records_validate(seq, 5));
    seq[4] = (input_record_t){2,0,0,0,255};   CHECK(input_records_validate(seq, 5));
    seq[2] = (input_record_t){3,1,0,0,0};     CHECK(!input_records_validate(seq, 5));   /* wait while held */
    input_record_t key_held[] = { {2,1,0,0,0}, {1,0,'a',0,0}, {2,0,0,0,0} };
    CHECK(!input_records_validate(key_held, 3));
    input_record_t bad_type[] = { {4,0,0,0,0} };
    CHECK(!input_records_validate(bad_type, 1));
    input_record_t bad_key[] = { {1,0,127,0,0} };
    CHECK(!input_records_validate(bad_key, 1));
    input_record_t key_tail[] = { {1,0,'a',1,0} };
    CHECK(!input_records_validate(key_tail, 1));
    input_record_t buttons8[] = { {2,8,0,0,0}, {2,0,0,0,0} };
    CHECK(!input_records_validate(buttons8, 2));
    input_record_t waits[] = { {3,0x10,0x27,0,0} };            /* exactly 10000 ms */
    CHECK(input_records_validate(waits, 1));
    input_record_t waits2[] = { {3,0x10,0x27,0,0}, {3,1,0,0,0} };
    CHECK(!input_records_validate(waits2, 2));
    CHECK(!input_records_validate(NULL, 1));

    /* Key mapping: Arduino codes -> HID usages */
    hid_key_t k;
    input_record_t key = { 1, 0, 'h', 0, 0 };
    CHECK(input_record_key(&key, &k) && k.keycode == 0x0B && k.modifiers == 0);
    key.b = 'H';
    CHECK(input_record_key(&key, &k) && k.keycode == 0x0B && k.modifiers == HID_MOD_LEFT_SHIFT);
    key.b = 'a'; key.a = 2;                                   /* Shift+a */
    CHECK(input_record_key(&key, &k) && k.keycode == 0x04 && k.modifiers == HID_MOD_LEFT_SHIFT);
    key.b = ' '; key.a = 8;                                   /* Cmd+Space */
    CHECK(input_record_key(&key, &k) && k.keycode == 0x2C && k.modifiers == HID_MOD_LEFT_GUI);
    key.b = '!'; key.a = 1 | 4;                               /* Ctrl+Alt+! (shift added by the map) */
    CHECK(input_record_key(&key, &k) && k.keycode == 0x1E &&
          k.modifiers == (HID_MOD_LEFT_CTRL | HID_MOD_LEFT_ALT | HID_MOD_LEFT_SHIFT));
    key.a = 0;
    key.b = 176; CHECK(input_record_key(&key, &k) && k.keycode == HID_KEY_ENTER);
    key.b = 177; CHECK(input_record_key(&key, &k) && k.keycode == HID_KEY_ESCAPE);
    key.b = 178; CHECK(input_record_key(&key, &k) && k.keycode == HID_KEY_BACKSPACE);
    key.b = 179; CHECK(input_record_key(&key, &k) && k.keycode == HID_KEY_TAB);
    key.b = 194; CHECK(input_record_key(&key, &k) && k.keycode == 0x3A);       /* F1 */
    key.b = 205; CHECK(input_record_key(&key, &k) && k.keycode == 0x45);       /* F12 */
    key.b = 210; CHECK(input_record_key(&key, &k) && k.keycode == 0x4A);       /* Home */
    key.b = 211; CHECK(input_record_key(&key, &k) && k.keycode == 0x4B);       /* Page Up */
    key.b = 212; CHECK(input_record_key(&key, &k) && k.keycode == HID_KEY_DELETE);
    key.b = 213; CHECK(input_record_key(&key, &k) && k.keycode == 0x4D);       /* End */
    key.b = 214; CHECK(input_record_key(&key, &k) && k.keycode == 0x4E);       /* Page Down */
    key.b = 215; CHECK(input_record_key(&key, &k) && k.keycode == HID_KEY_RIGHT);
    key.b = 216; CHECK(input_record_key(&key, &k) && k.keycode == HID_KEY_LEFT);
    key.b = 217; CHECK(input_record_key(&key, &k) && k.keycode == HID_KEY_DOWN);
    key.b = 218; CHECK(input_record_key(&key, &k) && k.keycode == HID_KEY_UP);
    for (int c = 32; c <= 126; c++) {
        key.b = (uint8_t)c;
        CHECK(input_record_key(&key, &k) && k.keycode != 0);
    }
    key.b = 127; CHECK(!input_record_key(&key, &k));
    key.b = 180; CHECK(!input_record_key(&key, &k));
    key.b = 'a'; key.a = 16; CHECK(!input_record_key(&key, &k));
    key.a = 0; key.type = 2; CHECK(!input_record_key(&key, &k));
    CHECK(!input_record_key(NULL, &k));
}

static void test_keymap(void)
{
    hid_key_t k;
    for (int c = 0x20; c <= 0x7E; c++) {
        CHECK(hid_keymap_from_ascii((char)c, &k));
        CHECK(k.keycode != 0);
    }
    CHECK(hid_keymap_from_ascii('a', &k) && k.keycode == 0x04 && k.modifiers == 0);
    CHECK(hid_keymap_from_ascii('z', &k) && k.keycode == 0x1D && k.modifiers == 0);
    CHECK(hid_keymap_from_ascii('Z', &k) && k.keycode == 0x1D && k.modifiers == HID_MOD_LEFT_SHIFT);
    CHECK(hid_keymap_from_ascii('1', &k) && k.keycode == 0x1E && k.modifiers == 0);
    CHECK(hid_keymap_from_ascii('0', &k) && k.keycode == 0x27 && k.modifiers == 0);
    CHECK(hid_keymap_from_ascii('!', &k) && k.keycode == 0x1E && k.modifiers == HID_MOD_LEFT_SHIFT);
    CHECK(hid_keymap_from_ascii(' ', &k) && k.keycode == 0x2C);
    CHECK(hid_keymap_from_ascii('\n', &k) && k.keycode == HID_KEY_ENTER);
    CHECK(hid_keymap_from_ascii('\t', &k) && k.keycode == HID_KEY_TAB);
    CHECK(hid_keymap_from_ascii('\b', &k) && k.keycode == HID_KEY_BACKSPACE);
    CHECK(hid_keymap_from_ascii('~', &k) && k.keycode == 0x35 && k.modifiers == HID_MOD_LEFT_SHIFT);
    CHECK(!hid_keymap_from_ascii((char)0xE9, &k));
    CHECK(!hid_keymap_from_ascii((char)0x01, &k));

    CHECK(hid_keymap_from_name("enter", &k) && k.keycode == HID_KEY_ENTER && k.modifiers == 0);
    CHECK(hid_keymap_from_name("Cmd+Shift+3", &k) && k.keycode == 0x20 &&
          k.modifiers == (HID_MOD_LEFT_GUI | HID_MOD_LEFT_SHIFT));
    CHECK(hid_keymap_from_name("RSHIFT+a", &k) && k.keycode == 0x04 && k.modifiers == HID_MOD_RIGHT_SHIFT);
    CHECK(hid_keymap_from_name("F12", &k) && k.keycode == 0x45);
    CHECK(!hid_keymap_from_name("", &k));
    CHECK(!hid_keymap_from_name("0x", &k));
    CHECK(!hid_keymap_from_name("0x100", &k));
    CHECK(!hid_keymap_from_name("ENTER+a", &k));
}

static void check_motion(int32_t dx, int32_t dy, int32_t max_step)
{
    hid_motion_t m;
    int32_t steps = hid_motion_begin(&m, dx, dy, max_step);
    int32_t ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    int32_t nx = (ax + max_step - 1) / max_step, ny = (ay + max_step - 1) / max_step;
    CHECK(steps == (nx > ny ? nx : ny));

    int64_t sum_x = 0, sum_y = 0;
    int32_t count = 0;
    int8_t sx, sy;
    while (hid_motion_next(&m, &sx, &sy)) {
        CHECK(sx >= -max_step && sx <= max_step);
        CHECK(sy >= -max_step && sy <= max_step);
        sum_x += sx;
        sum_y += sy;
        count++;
    }
    CHECK(count == steps);
    CHECK(sum_x == dx);
    CHECK(sum_y == dy);
}

static void test_motion(void)
{
    check_motion(0, 0, 127);
    check_motion(20, -10, 127);
    check_motion(800, 400, 127);
    check_motion(-800, 400, 127);
    check_motion(127, 127, 127);
    check_motion(128, 0, 127);
    check_motion(-128, -1, 127);
    check_motion(32767, -32767, 127);
    check_motion(32767, 32767, 1);
    check_motion(1000, 3, 10);

    hid_motion_t m;
    CHECK(hid_motion_begin(&m, 800, 400, 127) == 7);
    CHECK(hid_motion_begin(&m, 0, 0, 127) == 0);
    CHECK(hid_motion_begin(&m, 1, 0, 127) == 1);
    CHECK(hid_motion_begin(&m, 254, 0, 127) == 2);
    CHECK(hid_motion_begin(&m, 255, 0, 127) == 3);

    srand(1234);
    for (int i = 0; i < 20000; i++) {
        int32_t dx = (rand() % 65535) - 32767;
        int32_t dy = (rand() % 65535) - 32767;
        int32_t step = 1 + rand() % 127;
        check_motion(dx, dy, step);
    }
}

int main(void)
{
    test_parser();
    test_network_commands();
    test_keymap();
    test_motion();
    test_input_records();
    printf("%d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
