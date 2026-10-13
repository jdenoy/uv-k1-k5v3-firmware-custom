#ifndef APPS_TONECALL_PRESETS_H
#define APPS_TONECALL_PRESETS_H

/* Mutable ToneCall asset block: the user's presets.  ToneCall Edit owns the
 * UI and rewrites the block through the configuration proxy; ToneCall only
 * reads it (asset_read) to send a preset.
 *
 * Why a separate editor: an app's own config area holds APP_CONFIG_LOCAL_MAX
 * (16) bytes, too small for named presets, and an app cannot proxy its own
 * assets.  The proxy lets an editor app rewrite up to APP_CONFIG_PROXY_MAX
 * (117) bytes of another app's assets, committed after the editor exits:
 * the same split as APRS TX / APRS Edit, with no resident code.
 *
 * Layout: magic, then TC_PRESET_COUNT records of TC_REC_LEN bytes:
 *   [0..7]  name, uppercase ASCII padded with spaces (all spaces = no name)
 *   [8]     digit count (bits 7:4, 0 = empty preset) | type (bits 3:0, TC_*)
 *   [9..12] code digits as BCD, first digit in the high nibble of [9]
 * Reinstalling ToneCall restores the bundled block (all presets empty). */
#define TC_PRESETS_MAGIC   0x31504354u   /* "TCP1" */
#define TC_PRESET_COUNT    8u
#define TC_NAME_MAX        8u
#define TC_REC_LEN         13u
#define TC_REC_NAME        0u
#define TC_REC_FORMAT      8u
#define TC_REC_CODE        9u
#define TC_PRESETS_LEN     108u          /* 4 + TC_PRESET_COUNT * TC_REC_LEN */

_Static_assert(4u + TC_PRESET_COUNT * TC_REC_LEN == TC_PRESETS_LEN,
               "ToneCall preset block must match its declared length");
_Static_assert(TC_REC_CODE + 4u == TC_REC_LEN, "8 BCD digits fill the record");

#endif
