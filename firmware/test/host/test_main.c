/*
 * Host-side tests for the portable modules: command parser, key map, movement splitting and
 * the input-record protocol of the iPad Computer Use input tool.
 * Build and run with `make -C test/host test` (plain clang/gcc, no ESP-IDF needed).
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_codec.h"
#include "call_audio.h"
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

/* ---- call commands (HFP) --------------------------------------------------------------- */

static size_t fake_call_status(char *out, size_t size)
{
    return (size_t)snprintf(out, size, "slc=1 audio=cvsd call=1 setup=none held=0 number=10086 peer=aa:bb:cc:dd:ee:ff bonds=1");
}

static esp_err_t fake_call_control(command_call_op_t op, const char *number)
{
    hook_record("call(%d,%s);", (int)op, number ? number : "<null>");
    return hook_error;
}

static size_t fake_audio_status(char *out, size_t size)
{
    return (size_t)snprintf(out, size, "mode=stream link=1 rate=8000 peak=1234");
}

static esp_err_t fake_audio_control(command_audio_op_t op)
{
    hook_record("audio(%d);", (int)op);
    return hook_error;
}

static void fake_audio_in(const char *payload)
{
    hook_record("audio_in(%s);", payload);
}

static esp_err_t fake_call_unpair(void)
{
    hook_record("call_unpair();");
    return hook_error;
}

static void test_call_commands(void)
{
    /* Without the hooks the call commands exist, but are unsupported. */
    static const command_platform_t bare = { .uptime_s = fake_uptime };
    command_parser_init(&bare);
    mock_reset();
    expect("CALL", "ERR unsupported", "");
    expect("DIAL 10086", "ERR unsupported", "");
    expect("ANSWER", "ERR unsupported", "");
    expect("HANGUP", "ERR unsupported", "");
    expect("AUDIO", "ERR unsupported", "");
    expect("AUDIO TONE", "ERR unsupported", "");
    expect("A AAAA", "ERR unsupported", "");
    expect("UNPAIR", "OK", "unpair();");

    static const command_platform_t platform = {
        .uptime_s = fake_uptime,
        .call_status = fake_call_status,
        .call_control = fake_call_control,
        .audio_status = fake_audio_status,
        .audio_control = fake_audio_control,
        .audio_in = fake_audio_in,
        .call_unpair = fake_call_unpair,
    };
    command_parser_init(&platform);
    hook_error = ESP_OK;

    expect("CALL", "CALL slc=1 audio=cvsd call=1 setup=none held=0 number=10086 peer=aa:bb:cc:dd:ee:ff bonds=1", "");
    expect("call", "CALL slc=1 audio=cvsd call=1 setup=none held=0 number=10086 peer=aa:bb:cc:dd:ee:ff bonds=1", "");
    expect("CALL now", "ERR invalid_args", "");

    expect_hook("DIAL 10086", "OK", "call(0,10086);");
    expect_hook("dial +8613800138000", "OK", "call(0,+8613800138000);");
    expect_hook("DIAL *100#", "OK", "call(0,*100#);");
    expect_hook("DIAL", "ERR invalid_args", "");
    expect_hook("DIAL 10086 2", "ERR invalid_args", "");
    expect_hook("DIAL 1-800-555", "ERR invalid_args", "");
    expect_hook("DIAL abc", "ERR invalid_args", "");
    expect_hook("DIAL 123456789012345678901234567890123", "ERR invalid_args", "");      /* 33 digits */
    expect_hook("DIAL 12345678901234567890123456789012", "OK", "call(0,12345678901234567890123456789012);");
    expect_hook("ANSWER", "OK", "call(1,<null>);");
    expect_hook("hangup", "OK", "call(2,<null>);");
    expect_hook("ANSWER 1", "ERR invalid_args", "");
    expect_hook("HANGUP now", "ERR invalid_args", "");
    hook_error = ESP_ERR_INVALID_STATE;
    expect_hook("DIAL 10086", "ERR not_connected", "call(0,10086);");
    expect_hook("ANSWER", "ERR not_connected", "call(1,<null>);");
    hook_error = ESP_FAIL;
    expect_hook("HANGUP", "ERR failed", "call(2,<null>);");
    hook_error = ESP_OK;

    expect("AUDIO", "AUDIO mode=stream link=1 rate=8000 peak=1234", "");
    expect_hook("AUDIO OFF", "OK", "audio(0);");
    expect_hook("AUDIO tone", "OK", "audio(1);");
    expect_hook("audio Loop", "OK", "audio(2);");
    expect_hook("AUDIO STREAM", "OK", "audio(3);");
    expect_hook("AUDIO CONNECT", "OK", "audio(4);");
    expect_hook("AUDIO DISCONNECT", "OK", "audio(5);");
    expect_hook("AUDIO LOUD", "ERR invalid_args", "");
    expect_hook("AUDIO TONE LOOP", "ERR invalid_args", "");
    hook_error = ESP_ERR_INVALID_STATE;
    expect_hook("AUDIO CONNECT", "ERR not_connected", "audio(4);");
    hook_error = ESP_OK;

    /* Audio lines: payload verbatim (base64 is case-sensitive), never a response. */
    expect_hook("A QUJDRGVmZ2g=", "<none>", "audio_in(QUJDRGVmZ2g=);");
    expect_hook("a   QUJDRGVmZ2g=  \r\n", "<none>", "audio_in(QUJDRGVmZ2g=);");
    expect_hook("A", "<none>", "");
    expect_hook("A   ", "<none>", "");

    /* UNPAIR forgets the HID hosts first, then the call phones. */
    hook_calls[0] = '\0';
    expect("UNPAIR", "OK", "unpair();");
    CHECK(strcmp(hook_calls, "call_unpair();") == 0);
    hook_error = ESP_FAIL;
    hook_calls[0] = '\0';
    expect("UNPAIR", "ERR failed", "unpair();");
    CHECK(strcmp(hook_calls, "call_unpair();") == 0);
    hook_error = ESP_OK;
    mock_force_error = ESP_FAIL;
    hook_calls[0] = '\0';
    expect("UNPAIR", "ERR send_failed", "unpair();");
    CHECK(hook_calls[0] == '\0');                               /* the HID error stops the command */
    mock_force_error = ESP_OK;

    /* The HID commands are unaffected by the call hooks. */
    expect("PING", "PONG", "");
    expect("CLICK", "OK", "click();");
}

/* ---- call audio: codecs ---------------------------------------------------------------- */

static void test_audio_codec(void)
{
    /* G.711 mu-law reference points. */
    CHECK(audio_ulaw_encode(0) == 0xFF);
    CHECK(audio_ulaw_encode(32767) == 0x80);
    CHECK(audio_ulaw_encode(-32768) == 0x00);
    CHECK(audio_ulaw_decode(0xFF) == 0);
    CHECK(audio_ulaw_decode(0x7F) == 0);
    CHECK(audio_ulaw_decode(0x80) == 32124);
    CHECK(audio_ulaw_decode(0x00) == -32124);

    /* Every code survives decode -> encode (0x7F is the second zero and folds onto 0xFF). */
    for (int code = 0; code < 256; code++) {
        uint8_t again = audio_ulaw_encode(audio_ulaw_decode((uint8_t)code));
        CHECK(again == (code == 0x7F ? 0xFF : code));
    }
    /* The error of one encode -> decode pass stays inside the segment step, the sign is kept. */
    for (int v = -32768; v <= 32767; v += 7) {
        int16_t back = audio_ulaw_decode(audio_ulaw_encode((int16_t)v));
        int magnitude = v < 0 ? -v : v;
        int limit = magnitude > 32635 ? 1024 : 16 + magnitude / 16;
        int error = back - v;
        CHECK((error < 0 ? -error : error) <= limit);
        CHECK(v == 0 || back == 0 || (v < 0) == (back < 0));
    }

    /* base64: RFC 4648 test vectors. */
    static const struct {
        const char *plain;
        const char *text;
    } VECTORS[] = {
        { "f", "Zg==" }, { "fo", "Zm8=" }, { "foo", "Zm9v" },
        { "foob", "Zm9vYg==" }, { "fooba", "Zm9vYmE=" }, { "foobar", "Zm9vYmFy" },
    };
    for (size_t i = 0; i < sizeof(VECTORS) / sizeof(VECTORS[0]); i++) {
        char text[16];
        uint8_t plain[16];
        size_t len = strlen(VECTORS[i].plain);
        CHECK(audio_b64_encode((const uint8_t *)VECTORS[i].plain, len, text, sizeof(text)) == strlen(VECTORS[i].text));
        CHECK(strcmp(text, VECTORS[i].text) == 0);
        CHECK(audio_b64_decode(VECTORS[i].text, plain, sizeof(plain)) == (int)len);
        CHECK(memcmp(plain, VECTORS[i].plain, len) == 0);
    }
    uint8_t all[256], back[256];
    char text[AUDIO_B64_ENCODED_LEN(256) + 1];
    for (int i = 0; i < 256; i++) {
        all[i] = (uint8_t)i;
    }
    CHECK(audio_b64_encode(all, sizeof(all), text, sizeof(text)) == 344);
    CHECK(audio_b64_decode(text, back, sizeof(back)) == 256);
    CHECK(memcmp(all, back, sizeof(all)) == 0);

    uint8_t out[8];
    char small[4];
    CHECK(audio_b64_encode(all, 3, small, sizeof(small)) == 0);         /* no room for the NUL */
    CHECK(audio_b64_encode(all, 0, text, sizeof(text)) == 0);
    CHECK(audio_b64_decode("", out, sizeof(out)) == -1);
    CHECK(audio_b64_decode("Zm9", out, sizeof(out)) == -1);             /* not a multiple of four */
    CHECK(audio_b64_decode("Zm9v!A==", out, sizeof(out)) == -1);        /* character outside the alphabet */
    CHECK(audio_b64_decode("Zg==Zm9v", out, sizeof(out)) == -1);        /* padding in the middle */
    CHECK(audio_b64_decode("Z===", out, sizeof(out)) == -1);
    CHECK(audio_b64_decode("====", out, sizeof(out)) == -1);
    CHECK(audio_b64_decode("Zm9vYmFy", out, 5) == -1);                  /* output too small */
    CHECK(audio_b64_decode("Zm9vYmFy", out, 6) == 6);
}

/* ---- call audio: rings, test modes, host lines ----------------------------------------- */

#define AUDIO_FRAME_BYTES 120                   /* a typical CVSD packet over HCI: 60 samples */

static void pcm_put(uint8_t *pcm_le, size_t index, int16_t v)
{
    pcm_le[2 * index] = (uint8_t)((uint16_t)v & 0xFF);
    pcm_le[2 * index + 1] = (uint8_t)((uint16_t)v >> 8);
}

static int16_t pcm_get(const uint8_t *pcm_le, size_t index)
{
    return (int16_t)(uint16_t)(pcm_le[2 * index] | ((uint16_t)pcm_le[2 * index + 1] << 8));
}

static bool frame_is_silent(const uint8_t *pcm_le, size_t bytes)
{
    for (size_t i = 0; i < bytes; i++) {
        if (pcm_le[i] != 0) {
            return false;
        }
    }
    return true;
}

static void test_call_audio(void)
{
    uint8_t frame[AUDIO_FRAME_BYTES];
    char line[CALL_AUDIO_LINE_MAX];
    call_audio_stats_t st;

    /* OFF: silence towards the phone, nothing towards the host, the level meter works. */
    call_audio_init();
    call_audio_link_open();
    CHECK(call_audio_get_mode() == CALL_AUDIO_OFF);
    for (size_t i = 0; i < AUDIO_FRAME_BYTES / 2; i++) {
        pcm_put(frame, i, (int16_t)(i == 7 ? -12345 : 100));
    }
    call_audio_rx(frame, sizeof(frame), false);
    memset(frame, 0x55, sizeof(frame));
    call_audio_tx(frame, sizeof(frame));
    CHECK(frame_is_silent(frame, sizeof(frame)));
    CHECK(call_audio_host_out(line, sizeof(line)) == 0);
    call_audio_get_stats(&st, true);
    CHECK(st.link_open && st.rx_frames == 1 && st.tx_frames == 1 && st.rx_peak == 12345);
    call_audio_get_stats(&st, false);
    CHECK(st.rx_peak == 0);                                     /* the read restarted the meter */

    /* A bad frame is counted and carries silence; -32768 does not overflow the meter. */
    pcm_put(frame, 0, 30000);
    call_audio_rx(frame, sizeof(frame), true);
    pcm_put(frame, 0, -32768);
    call_audio_rx(frame, sizeof(frame), false);
    call_audio_get_stats(&st, true);
    CHECK(st.rx_bad_frames == 1 && st.rx_peak == 32767);

    /* TONE: 400 Hz beep, 0.5 s on and 0.5 s off, exact period of 20 samples. */
    call_audio_set_mode(CALL_AUDIO_TONE);
    CHECK(strcmp(call_audio_mode_name(call_audio_get_mode()), "tone") == 0);
    static uint8_t second[2 * CALL_AUDIO_SAMPLE_RATE];
    call_audio_tx(second, sizeof(second));
    CHECK(pcm_get(second, 0) == 0 && pcm_get(second, 5) == 8000 && pcm_get(second, 15) == -8000);
    CHECK(pcm_get(second, 20 * 100 + 5) == 8000);
    CHECK(pcm_get(second, 3999) == -2472);
    CHECK(frame_is_silent(second + 2 * 4000, sizeof(second) - 2 * 4000));
    call_audio_tx(frame, sizeof(frame));                        /* the next second starts with the beep again */
    CHECK(pcm_get(frame, 5) == 8000);

    /* LOOP: the received audio comes back after CALL_AUDIO_LOOP_DELAY samples. */
    call_audio_set_mode(CALL_AUDIO_LOOP);
    int16_t next_rx = 1, next_tx = 1;
    size_t sent = 0;
    bool loop_ok = true;
    for (int n = 0; n < 200; n++) {
        for (size_t i = 0; i < AUDIO_FRAME_BYTES / 2; i++) {
            pcm_put(frame, i, next_rx++);
        }
        call_audio_rx(frame, sizeof(frame), false);
        call_audio_tx(frame, sizeof(frame));
        sent += AUDIO_FRAME_BYTES / 2;
        if (sent < CALL_AUDIO_LOOP_DELAY) {
            loop_ok = loop_ok && frame_is_silent(frame, sizeof(frame));
        } else {
            for (size_t i = 0; i < AUDIO_FRAME_BYTES / 2; i++) {
                loop_ok = loop_ok && pcm_get(frame, i) == next_tx++;
            }
        }
    }
    CHECK(loop_ok);
    CHECK(next_tx > 1);
    CHECK(next_rx - next_tx >= CALL_AUDIO_LOOP_DELAY - (int)(AUDIO_FRAME_BYTES / 2));
    CHECK(next_rx - next_tx <= CALL_AUDIO_LOOP_DELAY);
    call_audio_get_stats(&st, true);
    CHECK(st.tx_underruns == 0 && st.tx_dropped == 0);

    /* STREAM uplink: 160 samples from the phone make one "A <base64 mu-law>" line. */
    call_audio_set_mode(CALL_AUDIO_STREAM);
    CHECK(call_audio_host_out(line, sizeof(line)) == 0);
    int16_t sample = 0;
    for (int n = 0; n < 3; n++) {                               /* 180 samples */
        for (size_t i = 0; i < AUDIO_FRAME_BYTES / 2; i++) {
            pcm_put(frame, i, (int16_t)(sample++ * 64));
        }
        call_audio_rx(frame, sizeof(frame), false);
    }
    CHECK(call_audio_host_out(line, 10) == 0);                  /* buffer too small: nothing is lost */
    size_t len = call_audio_host_out(line, sizeof(line));
    CHECK(len == 2 + AUDIO_B64_ENCODED_LEN(CALL_AUDIO_FRAME_SAMPLES) && strlen(line) == len);
    CHECK(line[0] == 'A' && line[1] == ' ');
    uint8_t ulaw[CALL_AUDIO_FRAME_SAMPLES];
    CHECK(audio_b64_decode(line + 2, ulaw, sizeof(ulaw)) == CALL_AUDIO_FRAME_SAMPLES);
    bool uplink_ok = true;
    for (int i = 0; i < CALL_AUDIO_FRAME_SAMPLES; i++) {
        uplink_ok = uplink_ok && ulaw[i] == audio_ulaw_encode((int16_t)(i * 64));
    }
    CHECK(uplink_ok);
    CHECK(call_audio_host_out(line, sizeof(line)) == 0);        /* 20 samples left: not a full frame */

    /* STREAM downlink: played after the prefill, in order; silence and one underrun when it runs dry. */
    call_audio_tx(frame, sizeof(frame));                        /* takes the flush of the mode change */
    CHECK(frame_is_silent(frame, sizeof(frame)));
    uint8_t tone_ulaw[CALL_AUDIO_FRAME_SAMPLES];
    for (int i = 0; i < CALL_AUDIO_FRAME_SAMPLES; i++) {
        tone_ulaw[i] = audio_ulaw_encode((int16_t)(1000 + i * 100));
    }
    char b64[AUDIO_B64_ENCODED_LEN(CALL_AUDIO_FRAME_SAMPLES) + 1];
    CHECK(audio_b64_encode(tone_ulaw, sizeof(tone_ulaw), b64, sizeof(b64)) > 0);
    for (int n = 0; n < 4; n++) {                               /* 640 samples: below the prefill */
        CHECK(call_audio_host_in(b64));
    }
    call_audio_tx(frame, sizeof(frame));
    CHECK(frame_is_silent(frame, sizeof(frame)));
    CHECK(call_audio_host_in(b64));                             /* 800 samples: playback starts */
    bool downlink_ok = true;
    for (int i = 0; i < 5 * CALL_AUDIO_FRAME_SAMPLES; i += 2) {
        call_audio_tx(frame, 4);                                /* two samples at a time */
        downlink_ok = downlink_ok && pcm_get(frame, 0) == audio_ulaw_decode(tone_ulaw[i % CALL_AUDIO_FRAME_SAMPLES]);
        downlink_ok = downlink_ok && pcm_get(frame, 1) == audio_ulaw_decode(tone_ulaw[(i + 1) % CALL_AUDIO_FRAME_SAMPLES]);
    }
    CHECK(downlink_ok);
    call_audio_tx(frame, sizeof(frame));                        /* dry */
    CHECK(frame_is_silent(frame, sizeof(frame)));
    call_audio_get_stats(&st, false);
    CHECK(st.tx_underruns == 1 && st.host_lines_in == 5 && st.host_lines_bad == 0 && st.host_lines_out == 1);
    call_audio_tx(frame, sizeof(frame));                        /* still dry: not a second underrun */
    call_audio_get_stats(&st, false);
    CHECK(st.tx_underruns == 1);

    /* Bad host lines are counted; a full TX ring drops the newest samples. */
    CHECK(!call_audio_host_in("not base64!"));
    CHECK(!call_audio_host_in(""));
    for (int n = 0; n < 60; n++) {                              /* 9600 samples into a ring of 8192 */
        call_audio_host_in(b64);
    }
    call_audio_get_stats(&st, false);
    CHECK(st.host_lines_bad == 2 && st.tx_dropped == 9600 - 8192);

    /* A slow host loses uplink audio, but the phone side never blocks. */
    static uint8_t burst[2 * 5000];
    call_audio_rx(burst, sizeof(burst), false);                 /* RX ring: 20 left + 5000 > 4096 */
    call_audio_get_stats(&st, false);
    CHECK(st.rx_dropped == 20 + 5000 - 4096);

    /* Outside STREAM the host lines are accepted and ignored; link close is visible. */
    call_audio_set_mode(CALL_AUDIO_OFF);
    CHECK(call_audio_host_in(b64));
    call_audio_tx(frame, sizeof(frame));
    CHECK(frame_is_silent(frame, sizeof(frame)));
    CHECK(call_audio_host_out(line, sizeof(line)) == 0);        /* the mode change emptied the RX ring */
    call_audio_link_close();
    call_audio_get_stats(&st, false);
    CHECK(!st.link_open);
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
    test_call_commands();
    test_audio_codec();
    test_call_audio();
    test_keymap();
    test_motion();
    test_input_records();
    printf("%d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
