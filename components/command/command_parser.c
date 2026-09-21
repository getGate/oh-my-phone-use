#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "command_parser.h"
#include "hid_device.h"
#include "hid_keymap.h"

#define MAX_ARGS 4
#define WIFI_SSID_MAX      32
#define WIFI_PASSWORD_MIN  8
#define WIFI_PASSWORD_MAX  63
#define SECRET_MAX         128

static command_platform_t s_platform;

void command_parser_init(const command_platform_t *platform)
{
    if (platform) {
        s_platform = *platform;
    } else {
        memset(&s_platform, 0, sizeof(s_platform));
    }
}

static size_t reply(char *out, size_t size, const char *fmt, ...)
{
    if (!out || size == 0) {
        return 0;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, size, fmt, ap);
    va_end(ap);
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n < size ? (size_t)n : size - 1;
}

static const char *error_name(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_STATE:  return "not_connected";
    case ESP_ERR_INVALID_ARG:    return "invalid_args";
    case ESP_ERR_NOT_SUPPORTED:  return "unsupported_char";
    case ESP_ERR_INVALID_SIZE:   return "too_many_keys";
    default:                     return "send_failed";
    }
}

static size_t result(char *out, size_t size, esp_err_t err)
{
    if (err == ESP_OK) {
        return reply(out, size, "OK");
    }
    return reply(out, size, "ERR %s", error_name(err));
}

/* Outcome of a platform hook (Wi-Fi / secret storage), which is not a HID transfer. */
static size_t hook_result(char *out, size_t size, esp_err_t err)
{
    switch (err) {
    case ESP_OK:                return reply(out, size, "OK");
    case ESP_ERR_INVALID_ARG:   return reply(out, size, "ERR invalid_args");
    case ESP_ERR_INVALID_STATE: return reply(out, size, "ERR not_ready");
    default:                    return reply(out, size, "ERR failed");
    }
}

/* Outcome of a call / call audio hook: ESP_ERR_INVALID_STATE means that no phone is connected. */
static size_t call_result(char *out, size_t size, esp_err_t err)
{
    switch (err) {
    case ESP_OK:                return reply(out, size, "OK");
    case ESP_ERR_INVALID_ARG:   return reply(out, size, "ERR invalid_args");
    case ESP_ERR_INVALID_STATE: return reply(out, size, "ERR not_connected");
    case ESP_ERR_NOT_SUPPORTED: return reply(out, size, "ERR unsupported");
    default:                    return reply(out, size, "ERR failed");
    }
}

static bool parse_int(const char *token, long min, long max, int *value)
{
    char *end = NULL;
    long v = strtol(token, &end, 10);
    if (end == token || *end != '\0' || v < min || v > max) {
        return false;
    }
    *value = (int)v;
    return true;
}

/* In-place unescape of \n \t \r \\ in TYPE text. Unknown escapes are kept literally. */
static void unescape(char *text)
{
    char *w = text;
    for (const char *r = text; *r; r++) {
        if (*r == '\\' && r[1]) {
            r++;
            switch (*r) {
            case 'n':  *w++ = '\n'; break;
            case 't':  *w++ = '\t'; break;
            case 'r':  *w++ = '\r'; break;
            case '\\': *w++ = '\\'; break;
            default:   *w++ = '\\'; *w++ = *r; break;
            }
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

/*
 * Split rest (in place) into at most max tokens. A token may be double-quoted so it can
 * contain spaces; inside quotes \" and \\ are escapes. quoted[i] tells whether token i was
 * quoted. Returns the token count, or -1 on unbalanced quotes / too many tokens.
 */
static int split_quoted(char *rest, char **tokens, bool *quoted, int max)
{
    int argc = 0;
    char *p = rest;
    for (;;) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            return argc;
        }
        if (argc == max) {
            return -1;
        }
        if (*p == '"') {
            char *w = ++p;
            char *r = p;
            tokens[argc] = w;
            quoted[argc] = true;
            for (;;) {
                if (*r == '\0') {
                    return -1;                              /* unterminated quote */
                }
                if (*r == '\\' && (r[1] == '"' || r[1] == '\\')) {
                    *w++ = r[1];
                    r += 2;
                    continue;
                }
                if (*r == '"') {
                    r++;
                    break;
                }
                *w++ = *r++;
            }
            if (*r != '\0' && *r != ' ' && *r != '\t') {
                return -1;                                  /* text glued to the closing quote */
            }
            *w = '\0';
            p = r;
        } else {
            tokens[argc] = p;
            quoted[argc] = false;
            while (*p && *p != ' ' && *p != '\t') {
                p++;
            }
            if (*p) {
                *p++ = '\0';
            }
        }
        argc++;
    }
}

static bool printable_ascii(const char *s)
{
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E) {
            return false;
        }
    }
    return true;
}

static size_t status_line(char *out, size_t size)
{
    hid_status_t st;
    hid_get_status(&st);
    uint32_t uptime = s_platform.uptime_s ? s_platform.uptime_s() : 0;
    size_t n = reply(out, size,
                     "STATUS connected=%d advertising=%d bonds=%d buttons=%u sent=%lu failed=%lu uptime=%lu",
                     st.connected ? 1 : 0, st.advertising ? 1 : 0, st.bonded_peers,
                     (unsigned)st.mouse_buttons, (unsigned long)st.reports_sent,
                     (unsigned long)st.reports_failed, (unsigned long)uptime);
    if (s_platform.net_status && n < size - 1) {
        n += s_platform.net_status(out + n, size - n);
        out[size - 1] = '\0';
        n = strlen(out);
    }
    return n;
}

typedef enum { CMD_KEY_PRESS, CMD_KEY_DOWN, CMD_KEY_UP } key_action_t;

static size_t key_command(char *out, size_t size, const char *arg, key_action_t action)
{
    hid_key_t key;
    if (!hid_keymap_from_name(arg, &key)) {
        return reply(out, size, "ERR unknown_key");
    }
    switch (action) {
    case CMD_KEY_DOWN: return result(out, size, hid_key_down(key));
    case CMD_KEY_UP:   return result(out, size, hid_key_up(key));
    default:           return result(out, size, hid_key_press(key));
    }
}

static size_t wifi_command(char *out, size_t size, char *rest)
{
    char *argv[3];
    bool quoted[3];
    int argc = rest ? split_quoted(rest, argv, quoted, 3) : 0;
    if (argc < 0 || argc > 2) {
        return reply(out, size, "ERR invalid_args");
    }
    if (argc == 0) {
        if (!s_platform.wifi_status) {
            return reply(out, size, "ERR unsupported");
        }
        char body[COMMAND_RESPONSE_MAX];
        s_platform.wifi_status(body, sizeof(body));
        return reply(out, size, "WIFI %s", body);
    }
    if (!s_platform.wifi_set) {
        return reply(out, size, "ERR unsupported");
    }
    if (argc == 1 && !quoted[0] && strcasecmp(argv[0], "CLEAR") == 0) {
        return hook_result(out, size, s_platform.wifi_set(NULL, NULL));
    }
    const char *ssid = argv[0];
    const char *password = argc == 2 ? argv[1] : "";
    size_t ssid_len = strlen(ssid), pass_len = strlen(password);
    if (ssid_len < 1 || ssid_len > WIFI_SSID_MAX ||
        (pass_len != 0 && (pass_len < WIFI_PASSWORD_MIN || pass_len > WIFI_PASSWORD_MAX)) ||
        !printable_ascii(ssid) || !printable_ascii(password)) {
        return reply(out, size, "ERR invalid_args");
    }
    return hook_result(out, size, s_platform.wifi_set(ssid, password));
}

static size_t secret_command(char *out, size_t size, char *rest)
{
    char *argv[2];
    bool quoted[2];
    int argc = rest ? split_quoted(rest, argv, quoted, 2) : 0;
    if (argc < 0 || argc > 1) {
        return reply(out, size, "ERR invalid_args");
    }
    if (argc == 0) {
        if (!s_platform.secret_status) {
            return reply(out, size, "ERR unsupported");
        }
        char body[COMMAND_RESPONSE_MAX];
        s_platform.secret_status(body, sizeof(body));
        return reply(out, size, "SECRET %s", body);
    }
    if (!s_platform.secret_set) {
        return reply(out, size, "ERR unsupported");
    }
    if (!quoted[0] && strcasecmp(argv[0], "CLEAR") == 0) {
        return hook_result(out, size, s_platform.secret_set(NULL));
    }
    size_t len = strlen(argv[0]);
    if (len < 1 || len > SECRET_MAX || !printable_ascii(argv[0])) {
        return reply(out, size, "ERR invalid_args");
    }
    return hook_result(out, size, s_platform.secret_set(argv[0]));
}

static bool dial_number_valid(const char *number)
{
    size_t len = strlen(number);
    if (len < 1 || len > COMMAND_DIAL_MAX) {
        return false;
    }
    for (const char *p = number; *p; p++) {
        if (!isdigit((unsigned char)*p) && *p != '*' && *p != '#' && *p != '+') {
            return false;
        }
    }
    return true;
}

static size_t audio_command(char *out, size_t size, char **args, int argc)
{
    static const struct {
        const char *name;
        command_audio_op_t op;
    } OPS[] = {
        { "OFF", COMMAND_AUDIO_OFF },       { "TONE", COMMAND_AUDIO_TONE },
        { "LOOP", COMMAND_AUDIO_LOOP },     { "STREAM", COMMAND_AUDIO_STREAM },
        { "CONNECT", COMMAND_AUDIO_CONNECT }, { "DISCONNECT", COMMAND_AUDIO_DISCONNECT },
    };
    if (argc == 0) {
        if (!s_platform.audio_status) {
            return reply(out, size, "ERR unsupported");
        }
        char body[COMMAND_RESPONSE_MAX];
        s_platform.audio_status(body, sizeof(body));
        return reply(out, size, "AUDIO %s", body);
    }
    if (!s_platform.audio_control) {
        return reply(out, size, "ERR unsupported");
    }
    if (argc == 1) {
        for (size_t i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) {
            if (strcasecmp(args[0], OPS[i].name) == 0) {
                return call_result(out, size, s_platform.audio_control(OPS[i].op));
            }
        }
    }
    return reply(out, size, "ERR invalid_args");
}

size_t command_parser_execute(const char *line, char *response, size_t response_size)
{
    if (!line || !response || response_size == 0) {
        return 0;
    }

    char buf[COMMAND_LINE_MAX];
    size_t len = strlen(line);
    if (len >= sizeof(buf)) {
        return reply(response, response_size, "ERR line_too_long");
    }
    memcpy(buf, line, len + 1);

    /* Trim trailing CR/LF/whitespace and leading whitespace. */
    while (len > 0 && isspace((unsigned char)buf[len - 1])) {
        buf[--len] = '\0';
    }
    char *p = buf;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p == '\0') {
        return 0;                                   /* blank line: no response */
    }

    /* Command word. */
    char *word = p;
    while (*p && !isspace((unsigned char)*p)) {
        *p = (char)toupper((unsigned char)*p);
        p++;
    }
    char *rest = NULL;                              /* text after the first separator, verbatim */
    if (*p) {
        *p++ = '\0';
        rest = p;
    }

    /* Call audio for the phone: up to 50 lines a second, never answered. */
    if (word[0] == 'A' && word[1] == '\0') {
        if (!s_platform.audio_in) {
            return reply(response, response_size, "ERR unsupported");
        }
        while (rest && (*rest == ' ' || *rest == '\t')) {
            rest++;
        }
        if (rest && *rest) {
            s_platform.audio_in(rest);
        }
        return 0;
    }

    /* Commands that keep their argument text verbatim (spaces matter). */
    if (strcmp(word, "TYPE") == 0) {
        if (!rest || *rest == '\0') {
            return reply(response, response_size, "ERR invalid_args");
        }
        unescape(rest);
        return result(response, response_size, hid_type_text(rest));
    }
    if (strcmp(word, "WIFI") == 0) {
        return wifi_command(response, response_size, rest);
    }
    if (strcmp(word, "SECRET") == 0) {
        return secret_command(response, response_size, rest);
    }

    /* Everything else: whitespace-separated arguments. */
    char *args[MAX_ARGS + 1];
    int argc = 0;
    if (rest) {
        char *save = NULL;
        for (char *tok = strtok_r(rest, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
            if (argc > MAX_ARGS) {
                break;
            }
            args[argc++] = tok;
        }
    }

    if (strcmp(word, "PING") == 0) {
        return reply(response, response_size, "PONG");
    }
    if (strcmp(word, "STATUS") == 0) {
        return status_line(response, response_size);
    }
    if (strcmp(word, "MOVE") == 0) {
        int dx, dy;
        if (argc != 2 || !parse_int(args[0], -HID_MAX_DELTA, HID_MAX_DELTA, &dx) ||
            !parse_int(args[1], -HID_MAX_DELTA, HID_MAX_DELTA, &dy)) {
            return reply(response, response_size, "ERR invalid_args");
        }
        return result(response, response_size, hid_move(dx, dy));
    }
    if (strcmp(word, "SCROLL") == 0) {
        int dy;
        if (argc != 1 || !parse_int(args[0], -HID_MAX_DELTA, HID_MAX_DELTA, &dy)) {
            return reply(response, response_size, "ERR invalid_args");
        }
        return result(response, response_size, hid_scroll(dy));
    }
    if (strcmp(word, "KEY") == 0 || strcmp(word, "KEYDOWN") == 0 || strcmp(word, "KEYUP") == 0) {
        if (argc != 1) {
            return reply(response, response_size, "ERR invalid_args");
        }
        key_action_t action = (word[3] == '\0') ? CMD_KEY_PRESS : (word[3] == 'D' ? CMD_KEY_DOWN : CMD_KEY_UP);
        return key_command(response, response_size, args[0], action);
    }
    if (strcmp(word, "AUDIO") == 0) {
        return audio_command(response, response_size, args, argc);
    }
    if (strcmp(word, "DIAL") == 0) {
        if (!s_platform.call_control) {
            return reply(response, response_size, "ERR unsupported");
        }
        if (argc != 1 || !dial_number_valid(args[0])) {
            return reply(response, response_size, "ERR invalid_args");
        }
        return call_result(response, response_size, s_platform.call_control(COMMAND_CALL_DIAL, args[0]));
    }

    /* Argument-less commands. */
    if (argc != 0) {
        if (strcmp(word, "CLICK") == 0 || strcmp(word, "DOWN") == 0 || strcmp(word, "UP") == 0 ||
            strcmp(word, "UNPAIR") == 0 || strcmp(word, "CALL") == 0 || strcmp(word, "ANSWER") == 0 ||
            strcmp(word, "HANGUP") == 0) {
            return reply(response, response_size, "ERR invalid_args");
        }
        return reply(response, response_size, "ERR invalid_command");
    }
    if (strcmp(word, "CALL") == 0) {
        if (!s_platform.call_status) {
            return reply(response, response_size, "ERR unsupported");
        }
        char body[COMMAND_RESPONSE_MAX];
        s_platform.call_status(body, sizeof(body));
        return reply(response, response_size, "CALL %s", body);
    }
    if (strcmp(word, "ANSWER") == 0 || strcmp(word, "HANGUP") == 0) {
        if (!s_platform.call_control) {
            return reply(response, response_size, "ERR unsupported");
        }
        command_call_op_t op = word[0] == 'A' ? COMMAND_CALL_ANSWER : COMMAND_CALL_HANGUP;
        return call_result(response, response_size, s_platform.call_control(op, NULL));
    }
    if (strcmp(word, "CLICK") == 0) {
        return result(response, response_size, hid_click());
    }
    if (strcmp(word, "DOWN") == 0) {
        return result(response, response_size, hid_mouse_down());
    }
    if (strcmp(word, "UP") == 0) {
        return result(response, response_size, hid_mouse_up());
    }
    if (strcmp(word, "UNPAIR") == 0) {
        esp_err_t err = hid_unpair_all();
        if (err == ESP_OK && s_platform.call_unpair) {
            return call_result(response, response_size, s_platform.call_unpair());
        }
        return result(response, response_size, err);
    }
    return reply(response, response_size, "ERR invalid_command");
}
