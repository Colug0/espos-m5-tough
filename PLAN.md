# Plan: espOS-Firmware für den M5Stack Tough

Stand: 2026-11-01. Ein lebendes Dokument: Entscheidungen und Phasen werden hier
nachgezogen, sobald sich etwas ändert.

## Ziel

Der M5Stack Tough wird ein kleines Multifunktionsdisplay am Signal K-Server.
Er zeigt Zustände an (Ankeralarm, Batterie-SoC, Wind) und gibt Notifications
aus: Anzeige und Ton ab einer einstellbaren Mindest-Schwere. Die Seiten entstehen im
**espOS HMI Designer** und werden per Push aufs Gerät geschickt, ohne neu zu
flashen.

## Getroffene Entscheidungen

| Thema | Entscheidung |
|---|---|
| Basis | espOS als Submodule am neuesten Release-Tag (zurzeit v0.26.0), ESP-IDF v6.0.3 |
| UI | JLP-Gerät (JSON Layout Player) nach `JLP-PROTOCOL.md` des P4-Cockpits, LVGL 9 |
| Code aus dem Cockpit | erlaubt (Absprache mit Dirk Wahrheit); Dirk stellt später auf Apache-2.0 um |
| Lizenz dieses Repos | Apache-2.0, sobald die übernommenen Cockpit-Dateien es auch sind (siehe Risiken) |
| Repo | `espos-m5-tough` im eigenen GitHub-Account; privat, bis Dirk die Cockpit-Lizenz umgestellt hat (geplant 2026-11-02) |
| Firmware-Name | CMake `project(m5_tough)`. Er wird nie mehr geändert, weil OTA ihn prüft |
| Einbau | unter Deck, Versorgung über 12 V an der Klemme, RS485 vorerst ungenutzt |
| Helligkeit | manuell (Regler `@brightness`) und automatisch (Nachtdimmung über `environment.mode`, Dimmen bei Inaktivität) |
| Halpi2 | Testserver. **Jede Kommunikation mit dem Halpi2 wird vorher vom Eigner genehmigt**, besonders alles, was dort etwas ändert (PUTs wie Anker lichten, Plugin-Installation, Konfiguration) |
| Upstream | Änderungen, die andere auch brauchen, gehen als PR an Designer, Cockpit, WASM und Registry |
| Werkzeuge | GitHub, CodeRabbit (PR-Review und `cr`-CLI vor dem PR), release-please |
| Notifications | Standard des Cockpits: Overlay und Ton ab `notifications.min_state`, Ton nach Schwere, `@audio_mute` am Gerät. Keine Regeln pro Pfad |
| Entwicklung | WSL2 (Ubuntu) unter Windows für Build und Flash. Kein eigener Test-Server vorerst |

## Architektur

```
Signal K-Server (Halpi2)
  ├─ espOS HMI Designer  ── POST /layout, GET /hello, /screenshot ──┐
  ├─ signalk-espos-manager (OTA, Fleet-Key, environment.mode)       │
  └─ Anker-Alarm-Plugin, Batterie, Wind ...                         │
            ▲  WebSocket-Deltas, PUT                                ▼
M5Tough-Firmware
  ├─ espOS: WLAN, Portal, Signal K-Client, mDNS, httpd, OTA, Logs
  ├─ jlp/   (aus dem Cockpit übernommen und gekürzt)
  │    layout_manager, widget_factory, subject/zone/notifications registry,
  │    alert_overlay, status_overlay, idle_dimmer, chime, net/*
  └─ tough_hal/  (neu)
       AXP192, ILI9342C über esp_lcd, CHSC6540-Touch, NS4168-AudioDriver,
       UI-Task (ui::post/after/every), Backlight
```

Die Dateistruktur von `jlp/` bleibt so nah am Cockpit wie möglich. Dann lässt
sich der Kern später als gemeinsame Komponente herauslösen (Phase 5), und die
WASM-Vorschau des Designers rendert für beide Geräte denselben Code.

Regeln aus dem Cockpit, die hier genauso gelten:

1. **Never blank the helm.** Ein Layout wird unsichtbar aufgebaut und erst dann
   umgeschaltet. Ein Fehler lässt das alte Layout stehen.
2. **Nur die UI-Task ruft LVGL auf.** Alles andere geht über `ui::post`.
3. **Kein optimistischer Schalter.** Toggles folgen dem Signal K-Echo.
4. **Blockierende REST-Aufrufe** an Signal K laufen in eigenen Worker-Tasks.
5. **Das Wire-Format wird nur erweitert.** Neue Felder sind optional; alles
   andere erhöht `schema`.

## Hardware (M5Stack Tough, SKU K034)

| Teil | Wert | Geprüft |
|---|---|---|
| SoC | ESP32-D0WDQ6-V3 (Rev. 3, nötig für espOS-Signierung), 240 MHz, dual core | Doku |
| Speicher | 16 MB Flash, 8 MB PSRAM, 520 KB SRAM | Doku |
| Display | 2,0" IPS 320×240, ILI9342C, SPI 40 MHz: MOSI G23, SCK G18, MISO G38, CS G5, DC G15, kein RST-Pin | Doku + bbn (`In_eSPI_Setup.h`); `Config.h` der Library (CS 14 / DC 27) ist ein Rest und wird nicht benutzt |
| LCD-Reset | AXP192 GPIO4 | bbn (`SetLCDRSet`) |
| Backlight | AXP192 **LDO3**, 2500–3300 mV (Helligkeit = Spannung) | bbn (`hw_brightness.h`); die Doku nennt DC3, bbn nutzt LDO3 im Betrieb |
| LCD-Logik + SD-Strom | AXP192 LDO2, 3300 mV | bbn |
| Touch | CHSC6540, I2C 0x2E auf G21/G22, INT G39, Reset über AXP | Doku + Library |
| PMIC | AXP192, I2C 0x34 auf G21/G22 | Library |
| RTC | BM8563, I2C 0x51 | Doku |
| Audio | NS4168 I2S: BCK G12, LRCK G0, DATA G2; Enable über AXP (`SetSpkEnable`); Mikrofon-Daten G34 (PDM) | bbn (`hw_sound.h`) |
| RS485 | G27 / G19, dort auch 6–24 V Versorgung | Richtung offen |
| microSD | SPI, CS G4 | Doku |
| USB | CH9102 | Doku |

Quellen: die M5Stack-Doku, die Library `m5stack/M5Tough` (v0.0.1) und
[bareboat-necessities/bbn-m5stack-tough](https://github.com/bareboat-necessities/bbn-m5stack-tough)
(MIT, Arduino + LVGL 8, läuft auf Booten; nutzt den Fork `mgrouch/M5Tough`). Die
AXP192-Initialisierung aus diesem Fork (`AXP192::begin()`) ist die Vorlage für
`tough_hal`. Offen sind nur noch die RS485-Richtung und das Bit für den
Lautsprecher-Enable; beides klärt der Schaltplan (`Tough-Schematics-PDF.pdf`).
espOS nutzt kein Arduino, die Libraries dienen nur als Nachschlagewerk.

bbn legt die LVGL-Puffer in den PSRAM. Das geht dort, weil TFT_eSPI die Pixel
ohne DMA schiebt. Mit `esp_lcd` und DMA brauchen wir interne Puffer (unten).

### Speicher: der Engpass

* Der SPI-DMA des klassischen ESP32 liest nicht aus dem PSRAM. Die LVGL-
  Zeichenpuffer liegen intern (etwa 2 × 320×20 Pixel, rund 25 KB).
* Task-Stacks dürfen auf diesem Chip nicht in den PSRAM.
* `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` muss herunter, sonst bleibt der PSRAM
  fast leer (Skill `espos-memory-budget`).
* Ein zweiter Webserver auf 8081 kostet RAM. Zu prüfen: die JLP-Routen über
  `espos_httpd_register()` auf den espOS-Server legen.
* Ab dem ersten Build wird der freie interne RAM pro Startphase gemessen und
  hier eingetragen.

## Use Cases und Widgets

| Anzeige | Widget | Signal K-Pfade |
|---|---|---|
| Ankeralarm | `anchor` (Kompassrose, Radius), `anchor_track`; Buttons RAISE / `@drop_here` erst nach Freigabe (sie ändern den Zustand am Halpi2) | hoekens-anchor-alarm liefert alle Pfade, die die Widgets brauchen: `navigation.anchor.{state,position,maxRadius,currentRadius,bearingTrue,apparentBearing}`, Alarm über `notifications.navigation.anchor` |
| Batterie-SoC | `value`, `bar` oder `arc`, Zonenfarben aus den Metadaten | aus Signal K, z. B. `electrical.batteries.<id>.capacity.stateOfCharge` (Verhältnis, `scale: 100`); genaue Pfade im Pfad-Picker des Designers |
| Wind | zuerst `arc` und `value`; später ein eigenes `wind`-Widget (Rose mit AWA-Zeiger) | aus Signal K, z. B. `environment.wind.angleApparent`, `speedApparent` |
| Notifications | `notifications`-Liste, Alarm-Overlay, Ton | `notifications.*` |

Kleines Display: Statusleiste 28 px und Tab-Leiste 56 px würden 35 % der Höhe
belegen. Layouts für den Tough setzen `tab_strip_height` auf etwa 32.

## Notifications

Wie im Cockpit, ohne Änderung am Protokoll:

* Overlay und Ton ab `notifications.min_state` (`alert`, `warn`, `alarm`,
  `emergency`), eingestellt im Notifications-Dialog des Designers.
* Der Ton richtet sich nach der Schwere: ein Piepton bei warn, zwei bei alarm,
  drei schnelle bei emergency. Er wird erzeugt, nicht gespeichert.
* ACK auf dem Display setzt die Notification über ein Delta auf `normal`.
* Ein `toggle` mit `bind: "@audio_mute"` schaltet den Ton am Gerät ab und
  bleibt über einen Neustart erhalten.

Ton-Regeln pro Pfad sind zurückgestellt.

## Phasen

### Phase 0: Fundament

- [ ] Mit Dirk klären: Lizenzwechsel der Cockpit-Dateien vor der
      Veröffentlichung, oder eine schriftliche Erlaubnis bis dahin
- [ ] GitHub-Repo `espos-m5-tough` im eigenen Account anlegen, privat
- [ ] Nach Dirks Lizenzwechsel: Repo öffentlich, SPDX-Köpfe der übernommenen Dateien auf Apache-2.0
- [x] Lokales Git-Repo (`main`), `LICENSE` (Apache-2.0), `README.md`
- [x] `AGENTS.md` + `CLAUDE.md` (`@AGENTS.md`), `.gitattributes` (`eol=lf`),
      `.gitignore` (`*.pem`, `sdkconfig.local`, `build*/`, `managed_components/`)
- [x] `.coderabbit.yaml` nach dem Cockpit, mit `ignore_title_keywords: "chore: release"`
- [x] Aus `espos-bathy-bridge`: `pr-title.yml`, `label-by-title.yml`,
      `.github/release.yml`, Dependabot für Actions
- [ ] Erster Commit und Push (nach Freigabe)
- [ ] Repo-Einstellung „Allow GitHub Actions to create and approve pull requests“
- [ ] CodeRabbit-App für das Repo freischalten
- [x] Signierschlüssel erzeugt: RSA-3072, in WSL unter
      `~/keys/espos-m5-tough/signing_key.pem` (0600), `signingKeyId` `7bcd3eec7a8b2f68`
- [ ] Sicherung des Schlüssels an einem zweiten Ort (Passwortmanager oder USB-Stick)
- [ ] Schlüssel als Secret `SIGNING_KEY_PEM` ablegen (nach dem Anlegen des Repos)
- [ ] Branch-Schutz (Skill `oss-branch-protection`), sobald CI läuft
- [x] WSL2: Systempakete, ESP-IDF v6.0.3 (`install.sh esp32`), `usbipd-win`
- [x] Skills `espos-firmware` und `espos-dev` installiert
- [ ] Skills aus `sailingnaturali` installieren (`coderabbit-cli`, `oss-branch-protection`)

`ci.yml` und `track-espos.yml` kommen mit Phase 1: Beide brauchen das
espOS-Submodule und eine Firmware, die sich bauen lässt. Ohne sie wäre `main`
von Anfang an rot.
- [ ] Halpi2: HMI Designer und signalk-espos-manager installieren (macht der Eigner selbst oder nach Freigabe)

### Phase 1: Hardware-Bringup

- [ ] espOS-Submodule, `.idf-version`, `version.txt`, `scripts/build.sh`
- [ ] `ci.yml` (Ziel `esp32`, Warnungs-Gate) und `track-espos.yml` aus bathy-bridge
- [ ] Prologue mit `16mb.csv`, Ziel `esp32`, PSRAM-Einstellungen
- [ ] AXP192: LCD-Strom, Backlight, Reset, Lautsprecher-Enable
- [ ] Display über `esp_lcd` (ILI9342C), Farbreihenfolge und Inversion prüfen
- [ ] CHSC6540-Touchtreiber
- [ ] LVGL 9 und UI-Task
- [ ] NS4168 als `espos_audio::AudioDriver`, Testton (ist der 1-W-Lautsprecher
      an Bord laut genug?)
- [ ] Speichertabelle pro Startphase

**Fertig, wenn:** Setup-Portal, Signal K-Freigabe, eine LVGL-Testseite mit Touch
und ein Testton laufen, und die Speichertabelle hier steht.

### Phase 2: JLP-Kern

- [ ] `jlp/` aus dem Cockpit übernehmen, ohne Voice, Mikrofon, Stream und N2K
- [ ] `/hello` (320×240, Widget-Liste, Firmware `m5-tough-x.y.z`), `/layout`,
      `/healthz`, mDNS `_signalk-player._tcp`
- [ ] Layout in LittleFS, Default-Layout für 320×240
- [ ] Widgets `label`, `value`, `bar`, `arc`, `toggle`, `button`, `slider`

**Fertig, wenn:** Der Designer findet das Gerät, und ein geschobenes Layout zeigt
Live-Daten vom Halpi2.

### Phase 3: Die Use Cases

- [ ] `anchor`, `anchor_track`, `@drop_here`
- [ ] `notifications`-Liste, Alarm-Overlay für 320×240, Chime, `@audio_mute`
- [ ] `@brightness` über AXP192 LDO3 (manuell), Idle-Dimmer und Nachtdimmung über
      `environment.mode` (automatisch; der Wert kommt vom signalk-espos-manager)
- [ ] `/screenshot` (erst BMP, dann JPEG) für den Mirror-Modus, `POST /screen`

**Fertig, wenn:** Ein Tough-Layout mit Anker, SoC, Wind und Notifications läuft
eine Nacht lang am Halpi2 ohne Neustart.

### Phase 4: Release und Flotte

- [ ] release-please (`"initial-version": "0.1.0"`), espOS `release-firmware.yml`
      mit `flash_tables: "16mb"`
- [ ] `espos_start_opts_t.board` fest auf `"M5Stack Tough"`
- [ ] Registry: `boards/m5stack-tough.json` und `projects/<id>.json`
- [ ] OTA über signalk-espos-manager testen
- [ ] README für Bootseigner (Hardware, 12-V-Anschluss, Flasher, Setup, Updates)

### Phase 5: Später

- [ ] `wind`-Widget (Firmware, Designer, WASM)
- [ ] NMEA 0183 über den RS485-Anschluss (`espos_nmea0183`), falls RS485 später gebraucht wird
- [ ] Uhrzeit aus der RTC BM8563
- [ ] Mit Dirk: den JLP-Kern als gemeinsame Komponente herauslösen

## PRs an andere Projekte

| Repo | Inhalt |
|---|---|
| `signalk-espOS/signalk-espos-hmi-designer` | Status- und Tab-Leistenhöhe aus `/hello` statt fester Konstanten; Vorlagen für kleine Displays |
| `dirkwa/espos-p4-cockpit` | später das `wind`-Widget |
| `dirkwa/espos-p4-cockpit-wasm` | Vorschau für 320×240 und neue Widgets |
| `signalk-espOS/registry` | Board und Projekt |

Regeln dort: Conventional Commits, Betreff klein und höchstens 50 Zeichen,
Squash-Merge, keine Versionssprünge, keine KI-Attribution.

## Risiken und offene Punkte

* **Lizenz in der Übergangszeit.** Übernommene Dateien tragen noch
  `LicenseRef-Source-Available-No-Redistribution`. In einem öffentlichen Repo
  sähen andere Nutzer diese Lizenz. Deshalb: Repo privat lassen, bis Dirk
  umgestellt hat, oder vorher umstellen lassen.
* **RAM** auf dem klassischen ESP32 (siehe oben). Abhilfe: früh messen.
* **Pins:** Display, Backlight und Audio sind über bbn geklärt; RS485-Richtung
  und Lautsprecher-Enable-Bit vor der Nutzung im Schaltplan prüfen.
* **Tests am Bordserver:** Tests laufen am Halpi2, und PUTs aus Buttons (Anker
  lichten, Schalter) wirken dort echt. Deshalb: jede Verbindung, jeder Request
  und jede Änderung am Halpi2 nur nach Freigabe durch den Eigner. Erste Tests nur
  mit Anzeige-Widgets; Buttons und Toggles erst, wenn ausdrücklich freigegeben.
* **Lautstärke** des 1-W-Lautsprechers bei Motorgeräusch: in Phase 1 testen.
* **WASM-Vorschau** zeigt erst nach einem PR an der WASM-Seite die
  Tough-Darstellung; bis dahin ist der Mirror-Modus die echte Vorschau.
* **Skills sind auf espOS 0.24.0** geprüft, espOS ist bei 0.26.0. Zahlen und
  Defaults vor dem Übernehmen nachsehen.
