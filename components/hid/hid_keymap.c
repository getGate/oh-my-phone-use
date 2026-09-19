#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "hid_keymap.h"

/* US layout punctuation: character -> (usage, needs shift). Letters and digits are computed. */
typedef struct {
    char ch;
    uint8_t code;
    bool shift;
} ascii_key_t;

static const ascii_key_t s_punctuation[] = {
    { ' ',  0x2C, false }, { '!',  0x1E, true  }, { '"',  0x34, true  }, { '#',  0x20, true  },
    { '$',  0x21, true  }, { '%',  0x22, true  }, { '&',  0x24, true  }, { '\'', 0x34, false },
    { '(',  0x26, true  }, { ')',  0x27, true  }, { '*',  0x25, true  }, { '+',  0x2E, true  },
    { ',',  0x36, false }, { '-',  0x2D, false }, { '.',  0x37, false }, { '/',  0x38, false },
    { ':',  0x33, true  }, { ';',  0x33, false }, { '<',  0x36, true  }, { '=',  0x2E, false },
    { '>',  0x37, true  }, { '?',  0x38, true  }, { '@',  0x1F, true  }, { '[',  0x2F, false },
    { '\\', 0x31, false }, { ']',  0x30, false }, { '^',  0x23, true  }, { '_',  0x2D, true  },
    { '`',  0x35, false }, { '{',  0x2F, true  }, { '|',  0x31, true  }, { '}',  0x30, true  },
    { '~',  0x35, true  },
};

typedef struct {
    const char *name;
    uint8_t code;
} named_key_t;

static const named_key_t s_named_keys[] = {
    { "ENTER", HID_KEY_ENTER },   { "RETURN", HID_KEY_ENTER },    { "ESC", HID_KEY_ESCAPE },
    { "ESCAPE", HID_KEY_ESCAPE }, { "BACKSPACE", HID_KEY_BACKSPACE }, { "BKSP", HID_KEY_BACKSPACE },
    { "TAB", HID_KEY_TAB },       { "SPACE", HID_KEY_SPACE },     { "CAPSLOCK", 0x39 },
    { "F1", 0x3A },  { "F2", 0x3B },  { "F3", 0x3C },  { "F4", 0x3D },  { "F5", 0x3E },  { "F6", 0x3F },
    { "F7", 0x40 },  { "F8", 0x41 },  { "F9", 0x42 },  { "F10", 0x43 }, { "F11", 0x44 }, { "F12", 0x45 },
    { "PRINTSCREEN", 0x46 }, { "SCROLLLOCK", 0x47 }, { "PAUSE", 0x48 }, { "INSERT", 0x49 },
    { "HOME", 0x4A }, { "PAGEUP", 0x4B }, { "PGUP", 0x4B }, { "DELETE", HID_KEY_DELETE },
    { "DEL", HID_KEY_DELETE }, { "END", 0x4D }, { "PAGEDOWN", 0x4E }, { "PGDN", 0x4E },
    { "RIGHT", HID_KEY_RIGHT }, { "LEFT", HID_KEY_LEFT }, { "DOWN", HID_KEY_DOWN }, { "UP", HID_KEY_UP },
    { "NUMLOCK", 0x53 },
};

typedef struct {
    const char *name;
    uint8_t mask;
} named_modifier_t;

static const named_modifier_t s_named_modifiers[] = {
    { "CTRL", HID_MOD_LEFT_CTRL },    { "CONTROL", HID_MOD_LEFT_CTRL },
    { "SHIFT", HID_MOD_LEFT_SHIFT },
    { "ALT", HID_MOD_LEFT_ALT },      { "OPT", HID_MOD_LEFT_ALT },     { "OPTION", HID_MOD_LEFT_ALT },
    { "CMD", HID_MOD_LEFT_GUI },      { "COMMAND", HID_MOD_LEFT_GUI }, { "GUI", HID_MOD_LEFT_GUI },
    { "META", HID_MOD_LEFT_GUI },     { "WIN", HID_MOD_LEFT_GUI },     { "SUPER", HID_MOD_LEFT_GUI },
    { "RCTRL", HID_MOD_RIGHT_CTRL },  { "RSHIFT", HID_MOD_RIGHT_SHIFT },
    { "RALT", HID_MOD_RIGHT_ALT },    { "ROPT", HID_MOD_RIGHT_ALT },
    { "RCMD", HID_MOD_RIGHT_GUI },    { "RGUI", HID_MOD_RIGHT_GUI },
};

#define TOKEN_MAX 24

bool hid_keymap_from_ascii(char c, hid_key_t *out)
{
    if (!out) {
        return false;
    }
    out->keycode = 0;
    out->modifiers = 0;

    if (c >= 'a' && c <= 'z') {
        out->keycode = (uint8_t)(0x04 + (c - 'a'));
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        out->keycode = (uint8_t)(0x04 + (c - 'A'));
        out->modifiers = HID_MOD_LEFT_SHIFT;
        return true;
    }
    if (c >= '1' && c <= '9') {
        out->keycode = (uint8_t)(0x1E + (c - '1'));
        return true;
    }
    switch (c) {
    case '0':   out->keycode = 0x27; return true;
    case '\n':  /* fall through */
    case '\r':  out->keycode = HID_KEY_ENTER; return true;
    case '\t':  out->keycode = HID_KEY_TAB; return true;
    case '\b':  out->keycode = HID_KEY_BACKSPACE; return true;
    case 0x1B:  out->keycode = HID_KEY_ESCAPE; return true;
    case 0x7F:  out->keycode = HID_KEY_DELETE; return true;
    default:
        break;
    }
    for (size_t i = 0; i < sizeof(s_punctuation) / sizeof(s_punctuation[0]); i++) {
        if (s_punctuation[i].ch == c) {
            out->keycode = s_punctuation[i].code;
            out->modifiers = s_punctuation[i].shift ? HID_MOD_LEFT_SHIFT : 0;
            return true;
        }
    }
    return false;
}

static void upper_copy(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i + 1 < n && src[i]; i++) {
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static uint8_t modifier_mask(const char *token)
{
    char upper[TOKEN_MAX];
    upper_copy(upper, token, sizeof(upper));
    for (size_t i = 0; i < sizeof(s_named_modifiers) / sizeof(s_named_modifiers[0]); i++) {
        if (strcmp(upper, s_named_modifiers[i].name) == 0) {
            return s_named_modifiers[i].mask;
        }
    }
    return 0;
}

/* Resolve a non-modifier token: single character, "0xNN" raw usage, or a named key. */
static bool resolve_key(const char *token, hid_key_t *key)
{
    if (token[1] == '\0') {
        hid_key_t k;
        if (!hid_keymap_from_ascii(token[0], &k)) {
            return false;
        }
        key->keycode = k.keycode;
        key->modifiers |= k.modifiers;
        return true;
    }
    if (token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
        char *end = NULL;
        unsigned long v = strtoul(token + 2, &end, 16);
        if (end == token + 2 || *end != '\0' || v == 0 || v > 0xFF) {
            return false;
        }
        key->keycode = (uint8_t)v;
        return true;
    }
    char upper[TOKEN_MAX];
    upper_copy(upper, token, sizeof(upper));
    for (size_t i = 0; i < sizeof(s_named_keys) / sizeof(s_named_keys[0]); i++) {
        if (strcmp(upper, s_named_keys[i].name) == 0) {
            key->keycode = s_named_keys[i].code;
            return true;
        }
    }
    return false;
}

bool hid_keymap_from_name(const char *name, hid_key_t *out)
{
    if (!name || !*name || !out) {
        return false;
    }
    hid_key_t key = { 0, 0 };
    const char *p = name;

    for (;;) {
        const char *end;
        if (p[0] == '+' && p[1] == '\0') {
            end = p + 1;                       /* a literal '+' as the final token ("SHIFT++", "+") */
        } else {
            end = strchr(p, '+');
            if (!end) {
                end = p + strlen(p);
            }
        }
        size_t len = (size_t)(end - p);
        if (len == 0 || len >= TOKEN_MAX) {
            return false;
        }
        char token[TOKEN_MAX];
        memcpy(token, p, len);
        token[len] = '\0';
        bool last = (*end == '\0');

        uint8_t mod = modifier_mask(token);
        if (mod) {
            key.modifiers |= mod;
        } else if (!last) {
            return false;                      /* only modifiers may precede the final key */
        } else if (!resolve_key(token, &key)) {
            return false;
        }
        if (last) {
            break;
        }
        p = end + 1;
        if (*p == '\0') {
            return false;                      /* dangling '+' */
        }
    }
    *out = key;
    return true;
}
