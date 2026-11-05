# JSON Layout Player

The player that renders layouts pushed by the
[espOS HMI Designer](https://github.com/signalk-espOS/signalk-espos-hmi-designer).
The wire contract is
[JLP-PROTOCOL.md](https://github.com/dirkwa/espos-p4-cockpit/blob/main/JLP-PROTOCOL.md)
in espos-p4-cockpit.

## Where it comes from

Every file here comes from
[espos-p4-cockpit](https://github.com/dirkwa/espos-p4-cockpit) `main/jlp/`
at `c3eaeed` (Apache-2.0, Dirk Wahrheit), except `features.h`. Each file says
so in its header. What changed for the M5Stack Tough:

| Change | Files |
|---|---|
| `cockpit_hal` renamed to `tough_hal` (the UI task API is the same) | all that use the UI task |
| Voice and stream behind `JLP_VOICE` / `JLP_STREAM` (`features.h`), both 0 here: no `voice`, `speaker`, `mic`, `volume`, `stream` widgets, no `/mic_probe*`, `/mic_gain`, `/stream_soak` | `widgets/widget_factory.cpp`, `net/http_api.*` |
| Not taken over: `audio/voice_control.*`, `mic_overlay.*`, `net/stream_client.*`, `net/wake_discover.*` | |
| `/hello` reports `firmware: m5-tough-jlp-<version>` and falls back to the hostname `m5-tough` | `net/http_api.cpp` |
| Status strip: on a panel narrower than 480 px only host, WiFi and Signal K | `status_overlay.cpp` |
| Alert overlay: on a panel lower than 400 px the whole screen, 10 px padding, 150x48 ACK button | `alert_overlay.cpp` |
| Default layout for 320x240, read-only widgets only | `default_layout.h` |
| `lv_display_get_default()` instead of `NULL`, which LVGL 9.6 deprecates | `idle_dimmer.cpp` |
| Boot layout from `applicationData/global/m5-tough/1/layout.json` instead of `…/p4-cockpit/…` (JLP-PROTOCOL.md says `<hostname>`; both firmwares use a fixed application id) | `net/layout_fetch.cpp` |
| The `screenshot[jpeg]` log line carries internal RAM free/largest (KB) at start, after the snapshot, after encoding and after sending | `net/http_api.cpp` |
| The zone and layout fetch tasks log their stack high-water mark when they end | `net/zone_fetch.cpp`, `net/layout_fetch.cpp` |
| Tab strip and pages in a flex column, so the strip never covers a page | `layout/layout_manager.cpp` |
| HTTP handlers: the semaphore a handler waits on is shared with the UI-task lambda, so a timeout leaves no dangling handle; a request body not complete within 10 s gives 408 (receive timeout 10 s instead of 120 s); a `/layout` 504 means the layout was not applied; a timed-out screenshot keeps the single-flight flag until its snapshot has run | `net/http_api.cpp` |
| Non-finite numbers and text that is no number on a numeric path are dropped from Signal K; a REST seed no longer overwrites a newer stream value; the int seed is clamped before the cast | `subject_registry.*`, `net/zone_fetch.cpp` |
| `zones: []` removes a path's zones instead of keeping the old ones | `zone_registry.cpp` |
| The notification log line says whether the source asked for sound (`sound`/`silent`) | `notifications_registry.cpp` |
| Chime tones at 1.6-2.6 kHz, shaped towards a square, alarms at full scale and longer, for the Tough's small speaker | `audio/chime.cpp` |
| Per-level notifications: optional `notifications.levels.<state>` with `show`, `sound` (`off`/`source`/`always`) and `repeat_s`; without it `min_state` applies as before. Only shown or sounding levels wake the panel (an addition to JLP-PROTOCOL.md) | `alert_overlay.*`, `layout/layout_manager.cpp`, `notifications_registry.*`, `app_main.cpp` |
| Status strip on swipe: with `status_overlay: false` a swipe down from the top edge shows the strip above the widgets for `cockpit.status_peek_s` (default 5 s), and it shows while the Signal K stream is down | `status_overlay.*`, `app_main.cpp` |
| Night brightness: `cockpit.night_bright` applies while `environment.mode` is night; the idle dimmer feeds the `@brightness` slider, which then adjusts the brightness in force | `idle_dimmer.*`, `widgets/widget_factory.cpp` |
| The ACK echo guard expires after 10 s, so an ACK that was never sent cannot hide the real clear | `notifications_registry.*` |
| `SunState` keeps its state in a `std::atomic` (stream task writes, UI task reads) | `sun_state.*` |
| Comments that described the P4 or the cockpit's history rewritten for this board | `layout/store.*`, `net/http_api.cpp`, `net/zone_fetch.cpp`, `zone_registry.cpp` |

The guards instead of deletions keep the files close to the cockpit's, so a
fix can travel in either direction and the player can become a shared
component later.

## Rules

The architecture invariants in [AGENTS.md](../../AGENTS.md) apply here first:
never blank the helm, LVGL only on the UI task, no optimistic switch latch,
blocking Signal K REST calls on their own tasks, an additive wire format.
