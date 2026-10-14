# CHIRP driver: TONE CALL key action

The firmware adds the side-key action `TONE CALL` (`ACTION_OPT_TONECALL`, persisted value 24, see `App/settings.h`). The CHIRP driver lists the key actions in `KEYACTIONS_LIST`, indexed by that persisted value, so it needs the same entry at the same position.

`f4hwn.chirp.tonecall.patch` is that change against the v6.1.0 driver (`f4hwn.chirp.v6.1.0.py`, 2026/09/28): one entry, `"TONE CALL"`, appended after `"BEACON"`. Nothing else in the driver depends on the list length: `get_action()` and the five key settings (`F1Shrt`, `F1Long`, `F2Shrt`, `F2Long`, `M Long`) read it, and the 7-bit `keyM_longpress_action` field holds up to 127.

A patch rather than a copy of the driver: the driver is maintained and released with the firmware, and a one-line patch applies to the next version where a copy would go stale.

```sh
patch f4hwn.chirp.v6.1.0.py < App/apps/tonecall/chirp/f4hwn.chirp.tonecall.patch
```

## Why it matters

An unpatched driver shows a key set to `TONE CALL` as `NONE` (`get_action()` falls back to 0 for an unknown value) and writes every setting back on upload: the key assignment is silently reset to `NONE`. The firmware itself is safe either way (`SETTINGS_InitEEPROM` keeps only values below `ACTION_OPT_LEN`).
