/*
 * Bluetooth Classic hands-free unit (HFP): the iPhone is the audio gateway, this board is the
 * car kit. Gives call control (dial, answer, hang up), call state events and the call audio
 * (see call_audio.h) to the host.
 *
 * Needs bt_stack_init() with Classic Bluetooth kept (CONFIG_PHONEUSE_BT_KEEP_CLASSIC).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "call_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Receives unsolicited lines for the host ("EVT ..." and "A <base64>"), without a newline. */
typedef void (*hfp_line_sink_t)(const char *line);

/** Start the profile, make the board discoverable and start the audio uplink task. */
esp_err_t hfp_client_start(hfp_line_sink_t sink);

/** "slc=1 audio=cvsd call=1 setup=none held=0 number=... peer=aa:bb:cc:dd:ee:ff bonds=1" */
size_t hfp_client_call_status(char *out, size_t size);

/** "mode=stream link=1 rate=8000 rx=... rx_bad=... ... peak=1234" (reading restarts the peak meter) */
size_t hfp_client_audio_status(char *out, size_t size);

/* Call control. ESP_ERR_INVALID_STATE when no phone has a service level connection. */
esp_err_t hfp_client_dial(const char *number);
esp_err_t hfp_client_answer(void);
esp_err_t hfp_client_hangup(void);

/** Ask the phone to open (true) or close (false) the audio link. */
esp_err_t hfp_client_audio_link(bool connect);

/** Forget every bonded phone (drops the connection). */
esp_err_t hfp_client_unpair_all(void);

#ifdef __cplusplus
}
#endif
