/*
 * Line-oriented command protocol. Transport-agnostic and free of ESP-IDF dependencies so it
 * can be driven by the serial console today, by other front ends later, and by host unit tests.
 *
 * Commands (case-insensitive):
 *   MOVE <dx> <dy>      relative pointer move            -> OK
 *   CLICK               left click (tap)                 -> OK
 *   DOWN / UP           hold / release the left button   -> OK
 *   SCROLL <dy>         vertical scroll                  -> OK
 *   TYPE <text>         type ASCII text (\n \t \r \\ escapes) -> OK
 *   KEY <key>           press+release, e.g. ENTER, a, CMD+H -> OK
 *   KEYDOWN / KEYUP <key>                                -> OK
 *   PING                                                 -> PONG
 *   STATUS                                               -> STATUS connected=<0|1> advertising=<0|1> bonds=<n> ...
 *   UNPAIR              forget all bonded phones         -> OK
 *   WIFI                Wi-Fi link state                 -> WIFI configured=<0|1> connected=<0|1> ssid=... ip=...
 *   WIFI <ssid> [pass]  store credentials and connect (quote values containing spaces) -> OK
 *   WIFI CLEAR          forget the stored credentials    -> OK
 *   SECRET              input-device secret state        -> SECRET configured=<0|1> source=<nvs|build|none>
 *   SECRET <value>      store the input-device secret    -> OK
 *   SECRET CLEAR        remove the stored secret         -> OK
 *   CALL                phone call state (HFP)           -> CALL slc=<0|1> audio=<off|cvsd|msbc> call=<0|1> ...
 *   DIAL <number>       place a call (digits * # +)      -> OK
 *   ANSWER / HANGUP     answer / reject or end a call    -> OK
 *   AUDIO               call audio state and counters    -> AUDIO mode=<off|tone|loop|stream> link=<0|1> ...
 *   AUDIO OFF|TONE|LOOP|STREAM   call audio mode         -> OK
 *   AUDIO CONNECT|DISCONNECT     ask the phone to open / close the audio link -> OK
 *   A <base64>          call audio for the phone (8 kHz mu-law), STREAM mode -> no response
 * Errors: ERR invalid_command | invalid_args | not_connected | send_failed | unsupported_char |
 *         unknown_key | too_many_keys | line_too_long | unsupported | failed
 *
 * WIFI, SECRET and the call commands are only available when the platform provides the
 * matching hooks; they answer "ERR unsupported" otherwise.
 *
 * Next to the responses, a firmware with the call hooks sends unsolicited lines: "EVT ..."
 * (call and link events) and "A <base64>" (call audio from the phone in STREAM mode).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define COMMAND_LINE_MAX      512   /* longest accepted command line, including TYPE text */
#define COMMAND_RESPONSE_MAX  256   /* longest response line */
#define COMMAND_DIAL_MAX      32    /* longest DIAL number */

typedef enum {
    COMMAND_CALL_DIAL,              /* number = the digits to dial */
    COMMAND_CALL_ANSWER,
    COMMAND_CALL_HANGUP,
} command_call_op_t;

typedef enum {
    COMMAND_AUDIO_OFF,
    COMMAND_AUDIO_TONE,
    COMMAND_AUDIO_LOOP,
    COMMAND_AUDIO_STREAM,
    COMMAND_AUDIO_CONNECT,
    COMMAND_AUDIO_DISCONNECT,
} command_audio_op_t;

/** Optional platform hooks (NULL members are allowed). */
typedef struct {
    uint32_t (*uptime_s)(void);                         /*!< seconds since boot, reported by STATUS */
    /** Extra STATUS fields, written with a leading space (" wifi=1 ip=10.0.0.5"). */
    size_t (*net_status)(char *out, size_t size);
    /** Body of the WIFI response ("configured=1 connected=1 ssid=x ip=y ..."). */
    size_t (*wifi_status)(char *out, size_t size);
    /** Store credentials and connect; ssid NULL clears them. password "" = open network. */
    esp_err_t (*wifi_set)(const char *ssid, const char *password);
    /** Body of the SECRET response ("configured=1 source=nvs"). */
    size_t (*secret_status)(char *out, size_t size);
    /** Store the input-device secret; NULL clears the stored one. */
    esp_err_t (*secret_set)(const char *secret);
    /** Body of the CALL response ("slc=1 audio=cvsd call=1 ..."). */
    size_t (*call_status)(char *out, size_t size);
    /** Call control; number is only set for COMMAND_CALL_DIAL. ESP_ERR_INVALID_STATE = no phone. */
    esp_err_t (*call_control)(command_call_op_t op, const char *number);
    /** Body of the AUDIO response ("mode=stream link=1 ..."). */
    size_t (*audio_status)(char *out, size_t size);
    esp_err_t (*audio_control)(command_audio_op_t op);
    /** One line of call audio for the phone: the base64 text after "A ". No response is sent. */
    void (*audio_in)(const char *payload);
    /** Forget the phones bonded for calls; runs with UNPAIR. */
    esp_err_t (*call_unpair)(void);
} command_platform_t;

void command_parser_init(const command_platform_t *platform);

/**
 * @brief Execute one command line and produce the single-line response (no trailing newline).
 * @param line      NUL-terminated line; trailing CR/LF are ignored.
 * @param response  buffer of at least COMMAND_RESPONSE_MAX bytes
 * @return length of the response, or 0 when the line was blank and nothing should be sent.
 */
size_t command_parser_execute(const char *line, char *response, size_t response_size);

#ifdef __cplusplus
}
#endif
