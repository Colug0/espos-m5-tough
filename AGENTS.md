# espos-m5-tough

ESP-IDF 6 firmware for the **M5Stack Tough** (ESP32-D0WDQ6-V3, 16 MB flash,
8 MB PSRAM, 320×240 ILI9342C, CHSC6540 touch, AXP192 PMIC, NS4168 speaker).
It runs a **JSON Layout Player (JLP)**: the UI is a JSON layout pushed over HTTP
by the espOS HMI Designer and rendered live with LVGL 9, bound to Signal K
paths. No firmware rebuild per layout change.

Built on **espOS** (`espos/` submodule, pinned to a release tag): WiFi,
provisioning portal, config store and web UI, Signal K discovery, token, stream
in and out, PUT, REST, mDNS, health watchdog, logs, core dump and signed OTA
are espOS. This repo is the display on top.

The plan, decisions, hardware facts and phases are in [PLAN.md](PLAN.md).
Read it first; it says which phase the project is in.

## Companion projects

| Repo | Relation |
|---|---|
| [signalk-espOS/espOS](https://github.com/signalk-espOS/espOS) | The base. Generic device features belong there, with a host test, not here |
| [dirkwa/espos-p4-cockpit](https://github.com/dirkwa/espos-p4-cockpit) | The reference JLP device (ESP32-P4). Its `JLP-PROTOCOL.md` is the wire contract; its `main/jlp/` is the source of this repo's player code |
| [signalk-espOS/signalk-espos-hmi-designer](https://github.com/signalk-espOS/signalk-espos-hmi-designer) | The designer that pushes layouts. Reads `/hello` and refuses kinds and fields the device does not advertise |
| [signalk-espOS/espos-p4-cockpit-wasm](https://github.com/signalk-espOS/espos-p4-cockpit-wasm) | The cockpit renderer in WASM for the designer preview |
| [signalk-espOS/registry](https://github.com/signalk-espOS/registry) | Firmware listing for the manager and the web flasher |
| [bareboat-necessities/bbn-m5stack-tough](https://github.com/bareboat-necessities/bbn-m5stack-tough) | Arduino firmware for the same board (MIT). Reference for pins and AXP192 setup only |

## Architecture invariants

Non-negotiable, as in the cockpit:

1. **Never blank the helm.** Parse, validate and build a layout under a hidden
   staging parent; swap only a complete tree. A failed push returns
   400/422/500 and the previous layout keeps rendering.
2. **LVGL is single-writer** on the UI task. The HTTP task parses and
   validates, then marshals build and swap with `ui::post(...)`. espOS Signal K
   callbacks and `ESPOS_EVENT` handlers copy what they need and `ui::post`. No
   `lv_*` call from any other task.
3. **No optimistic switch latch.** Toggle visuals follow the Signal K echo.
4. **Blocking Signal K REST calls** run on their own worker tasks, never on the
   UI task, the stream task, an event handler or a URI handler.
5. **The wire format is additive.** New optional fields are fine. Removing,
   renaming or changing the meaning of a field bumps `schema`, and needs the
   cockpit and the designer to change with it.
6. **OSS only**, programmatic LVGL API only. Runtime and bundled dependencies
   stay permissive.

## Board facts that bite

* The ESP32's SPI DMA cannot read PSRAM. LVGL draw buffers are internal,
  DMA-capable memory. Task stacks stay internal on this chip.
* `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` defaults to 16384, which leaves PSRAM
  almost unused. Measure internal RAM per boot stage before and after any
  memory change (`espos-memory-budget` skill).
* Display power, backlight (LDO3, brightness = voltage), LCD reset and speaker
  enable all go through the AXP192 on I2C 0x34. Initialise it before the panel
  and the touch controller.
* Internal RAM dips last under 100 ms; the once-a-minute `heap running` line
  misses them, `min` in it does not. When it falls, sample every 25 ms to find
  the moment (PLAN.md has the table of causes found so far). Allocations that
  ask for internal RAM explicitly ignore `SPIRAM_MALLOC_ALWAYSINTERNAL`; the
  JPEG encoder did, and is redirected in `main/jpeg_psram.c`.
* WiFi/lwIP buffers in PSRAM made internal RAM worse on this chip, not better;
  capping the dynamic RX buffers and the TCP window is what helped.
* The ESP32 restarts without deauthenticating. The boat's MikroTik then keeps
  the old station state and delivers nothing to the device for minutes, so
  `app_main.cpp` drops the first association of every boot. Check the router
  log for "reconnecting" before blaming firmware for a lost Signal K link.
* `espos_start_opts_t.board` is `"M5Stack Tough"` and the CMake project name is
  `m5_tough`. Never change either: OTA and the registry match on them.

## Build

ESP-IDF v6.0.3 in WSL2 (Ubuntu), the version in `.idf-version`. Always build
through the wrapper, never bare `idf.py build`:

```bash
. ~/esp/esp-idf-v6.0.3/export.sh
git submodule update --init --recursive
scripts/build.sh -B build -DIDF_TARGET=esp32 build
scripts/build.sh -B build -p /dev/ttyACM0 flash monitor
```

ESP-IDF refuses a project path that contains spaces, and building on
`/mnt/c` from WSL is slow. When the checkout lives on the Windows drive
(`C:\Users\...\espos M5 Tough`), mirror it into the WSL filesystem with
`rsync -a --delete` (excluding `build*/`, `managed_components/`, `sdkconfig*`,
`dependencies.lock` and `secure_boot_signing_key.pem`) and build there.

Flash from Windows, not through `usbipd` into WSL: the forwarded CH9102
dropped the connection mid-write and then hung. The Tough is a native COM
port on Windows, and esptool reads the build from the WSL filesystem:

```powershell
cd \\wsl$\Ubuntu\home\thomas\dev\espos-m5-tough\build
py -m esptool --chip esp32 -p COM4 -b 460800 --before default-reset --after hard-reset write-flash "@flash_args"
py -m serial.tools.miniterm COM4 115200
```

If a write stops with "Lost connection", unplug and replug the USB cable and
start again; the ROM loader is always there.

The wrapper lives in espOS: one build lock per machine, half the cores,
`nice`/`ionice`. Zero warnings is the gate; CI fails on compiler, linker and
CMake warnings. sdkconfig: espOS's defaults first, this repo's
`sdkconfig.defaults` holds only what differs, a git-ignored `sdkconfig.local`
takes bench overrides. Defaults reach a fresh `sdkconfig` only, so delete it
after changing any of them.

Verify on the device: `curl http://<device>/api/v1/logs?limit=200`,
`/api/v1/system/info` (`last_reset`), and the JLP `GET /hello`.

## The test server is the boat

The only Signal K server is the owner's boat server (Halpi2). **Every contact
with it needs the owner's approval first**: connecting, sending a request,
installing or configuring a plugin, and above all any PUT or delta that changes
state there (raising the anchor, switches, acknowledging a notification). Say
what will be sent and what it changes, then wait. Test with display-only
widgets first.

## Never commit

WiFi SSIDs or passwords, server IPs or hostnames, API keys, tokens, `*.pem`, or
any other boat configuration: not in source, `sdkconfig.defaults` or Kconfig
defaults. `strings` on a release image prints all of it. Site config lives in
NVS (setup portal); bench overrides go in `sdkconfig.local`.

## Licensing

This repo is Apache-2.0, and so is espos-p4-cockpit since its relicensing
(`c3eaeed`, PR #198). Code taken over from the cockpit keeps Dirk Wahrheit's
`SPDX-FileCopyrightText` line, gains one for whoever changes it here, and says
in its header comment what changed. Runtime and bundled dependencies stay
permissive.

## Commits and pull requests

* Conventional Commits, `<type>(<scope>): <subject>`, imperative, lower case,
  no trailing period, subject at most 50 characters. PR titles become the
  release notes.
* Types: `feat`, `fix`, `perf`, `refactor`, `revert`, `docs`, `test`, `build`,
  `ci`, `chore`. Scopes: `hal`, `display`, `touch`, `audio`, `jlp`, `widgets`,
  `layout`, `net`, `ci`, `deps`.
* One logical change per PR. Squash-merge every PR.
* No version bumps in feature or fix PRs; release-please owns `version.txt`.
* Never commit or push unless the owner asks.
* Before opening a PR, review the committed diff yourself, then run
  CodeRabbit: `cr review --agent --committed --base-commit "$(git merge-base origin/main HEAD)"`
  and keep the output (`coderabbit-cli` skill).
* Whether CodeRabbit reviews on its own depends on the plan and the repo's
  visibility (docs.coderabbit.ai, "Plans"; in November 2026 the open-source
  plan reviewed repos under 10 stars only on request). Comment
  `@coderabbitai review` on the PR to start one.

## Pull requests to other repos

espOS, the cockpit, the designer, the WASM repo and the registry have their own
`AGENTS.md`; read it first. Their shared rules: Conventional Commits with a
subject of at most 50 characters, squash-merge, no version bumps, and **no AI
attribution anywhere** (no `Co-Authored-By`, no "Generated with"). Land espOS
changes first, then bump the submodule here in a separate PR.

## Comments and docs

Comments say why, not what. No development narrative ("step 3", "added in PR
#N") in comments or docs; that is git history. Docs follow espOS's style: lead
with what the reader does or gets, short sentences, a number or file name
rather than an adjective. Write **Signal K** with a space and **espOS** with a
small e.
