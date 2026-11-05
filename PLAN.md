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
| Code aus dem Cockpit | erlaubt; das Cockpit steht seit 2026-11-01 unter Apache-2.0 (`c3eaeed`, PR #198) |
| Lizenz dieses Repos | Apache-2.0, auch für die übernommenen Cockpit-Dateien |
| Repo | `espos-m5-tough` im eigenen GitHub-Account (`Colug0`); privat bis zum Lizenzwechsel, öffentlich seit 2026-11-01 (vom Eigner freigegeben) |
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

Gemessen am 2026-11-01, Firmware 0.1.0 (Phase 1), espOS 0.26.0, ein Tough
(Chip-Rev. v3.1, ILI9342C). Interner RAM = `MALLOC_CAP_INTERNAL | 8BIT`:

| Phase | intern frei | größter Block | verbraucht |
|---|---|---|---|
| Start (`heap boot`) | 222,9 KB | 94,2 KB | – |
| AXP192, Display, Touch, LVGL, UI-Task (`heap ui`) | 179,1 KB | 77,8 KB | 43,9 KB (Zeichenpuffer 25,6 KB, UI-Stack 16 KB) |
| Lautsprecher (`heap audio`) | 172,6 KB | 77,8 KB | 6,4 KB |
| espOS: httpd, WLAN mit Portal, Signal K, OTA, mDNS (`heap espos`) | 72,4 KB | 61,4 KB | 100,3 KB |
| nach 60 s, Portal offen, ohne Signal K-Verbindung (`heap running`) | 71,5 KB | 61,4 KB | Tiefstwert 64,1 KB |
| nach 60 s, im WLAN, Signal K-Stream über TLS zum Halpi2 (`heap running`) | 74,3 KB | 59,4 KB | Tiefstwert 55,0 KB |

Der Tiefstwert mit Signal K entsteht beim Verbindungsaufbau: die TLS-Handshakes
kosten kurzzeitig rund 9 KB mehr. Vom PSRAM sind 4 MB eingeblendet (Grenze des
klassischen ESP32), davon 3,87 MB frei; LVGL belegt seinen 256-KB-Pool. Der
espOS-Alarm liegt bei 12 KB frei bzw. 8 KB größter Block.

**Phase 2 (JLP-Player, 2026-11-01).** Mit Player, Signal K und dem Mirror-Modus
des Designers fiel der interne RAM zunächst kurzzeitig auf 0,7–3,7 KB frei
(`lowMemory`-Alarm). Gefunden mit einer Wächter-Task, die alle 25 ms misst; die
Einbrüche dauerten unter 100 ms und fielen zwischen alle seltener gemessenen
Punkte. Ursachen und Abhilfen:

| Ursache | Abhilfe | Wirkung |
|---|---|---|
| TLS-Handshakes parallel zum Stream brauchen je 24 KB am Stück | `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` (TLS-Puffer im PSRAM) | kein `tlsMemory` mehr |
| Zeichenpuffer 2 × 20 Zeilen | 2 × 10 Zeilen | +12,8 KB |
| 10 statische WLAN-RX-Puffer | 6 (= Block-Ack-Fenster) | +6,4 KB |
| UI-Task-Stack 16 KB, gemessen 6,3 KB genutzt | 12 KB | +4 KB |
| Netzwerk-Spitzen: eingehende Pakete mehrerer Verbindungen gleichzeitig | 16 statt 32 dynamische RX-Puffer, TCP-Fenster 2880 statt 5760 | Tiefstwert beim Verbindungsaufbau 37,9 statt 3,7 KB |
| JPEG-Encoder nimmt seine Arbeitspuffer (>35 KB) bevorzugt intern | `--wrap` der beiden `_inner`-Allokatoren auf PSRAM zuerst (`main/jpeg_psram.c`) | Mirror-Modus ohne Einbruch, Kodieren 146–217 statt 115 ms |
| (verworfen) WiFi/lwIP-Puffer im PSRAM | `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=n` bleibt | schlechter: 30 KB frei, Tiefstwert 0,7 KB |

Ergebnis mit Signal K, Designer-Layout (zwei Screens, Anker-Widgets) und
Mirror-Modus über Minuten: **55,5 KB frei, größter Block 32,8 KB, Tiefstwert
seit Boot 32,5 KB**, keine Speicherwarnung. Stacks gemessen: Zonenabruf 6 KB,
Layoutabruf 5 KB von je 8 KB (mit TLS-Handshake).

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

- [x] Lizenz der Cockpit-Dateien geklärt (Apache-2.0 seit 2026-11-01)
- [x] GitHub-Repo `espos-m5-tough` im eigenen Account anlegen, privat
- [x] Nach Dirks Lizenzwechsel: SPDX-Köpfe der übernommenen Dateien auf Apache-2.0
- [x] Repo öffentlich stellen (vom Eigner freigegeben, 2026-11-01)
- [x] Lokales Git-Repo (`main`), `LICENSE` (Apache-2.0), `README.md`
- [x] `AGENTS.md` + `CLAUDE.md` (`@AGENTS.md`), `.gitattributes` (`eol=lf`),
      `.gitignore` (`*.pem`, `sdkconfig.local`, `build*/`, `managed_components/`)
- [x] `.coderabbit.yaml` nach dem Cockpit, mit `ignore_title_keywords: "chore: release"`
- [x] Aus `espos-bathy-bridge`: `pr-title.yml`, `label-by-title.yml`,
      `.github/release.yml`, Dependabot für Actions
- [x] Erster Commit, privates Repo `Colug0/espos-m5-tough` angelegt
- [x] Push nach `origin/main`
- [x] Repo-Einstellung „Allow GitHub Actions to create and approve pull requests“
- [x] CodeRabbit-App für das Repo freigeschaltet (Login über GitHub)
- [x] Signierschlüssel erzeugt: RSA-3072, in WSL unter
      `~/keys/espos-m5-tough/signing_key.pem` (0600), `signingKeyId` `7bcd3eec7a8b2f68`
- [ ] Sicherung des Schlüssels an einem zweiten Ort (Passwortmanager oder USB-Stick)
- [x] Schlüssel als Secret `SIGNING_KEY_PEM` abgelegt (2459 Byte). Der erste
      signierte Release-Build muss ihn bestätigen: `espsecure signature-info-v2`
      auf dem Image muss `7bcd3eec7a8b2f68` zeigen
- [x] Branch-Schutz für `main`: nur per PR, Checks `firmware (esp32)` und
      `title is a Conventional Commit` Pflicht, lineare Historie, kein Force-Push
- [x] WSL2: Systempakete, ESP-IDF v6.0.3 (`install.sh esp32`), `usbipd-win`
- [x] Skills `espos-firmware` und `espos-dev` installiert
- [x] Skill `coderabbit-cli` installiert; die `cr`-CLI selbst fehlt noch, Reviews
      laufen deshalb im PR

`ci.yml` und `track-espos.yml` kommen mit Phase 1: Beide brauchen das
espOS-Submodule und eine Firmware, die sich bauen lässt. Ohne sie wäre `main`
von Anfang an rot.
- [x] Halpi2: HMI Designer und signalk-espos-manager installiert (`environment.mode` kommt an)

### Phase 1: Hardware-Bringup

- [x] espOS-Submodule (v0.26.0), `.idf-version`, `version.txt`, `scripts/build.sh`
- [x] `ci.yml` (Ziel `esp32`, Warnungs-Gate) und `track-espos.yml` aus bathy-bridge
- [x] Prologue mit `16mb.csv`, Ziel `esp32`, PSRAM-Einstellungen
- [x] AXP192: LCD-Strom (LDO2), Backlight (LDO3), Resets (GPIO4/GPIO1), Lautsprecher (GPIO2)
- [x] Display über `esp_lcd` (ILI9342C, wahlweise ILI9342E per Kconfig)
- [x] CHSC6540-Touchtreiber
- [x] LVGL 9.6 und UI-Task, LVGL-Heap als 256-KB-Pool im PSRAM
- [x] NS4168 als `espos_audio::AudioDriver`, Testton-Button
- [x] Build ohne Warnungen, Image 1,6 MB (24 % des App-Slots)
- [x] Auf dem Gerät geprüft: Bild, Farben, Helligkeit, Ton
- [x] Touch-Koordinaten geprüft: Ecken 1,1 und 319,239, keine Kalibrierung nötig
- [x] WLAN über das Setup-Portal, Signal K-Zugriff am Halpi2 freigegeben, Stream verbunden
- [x] Lautstärke an Bord bewertet: der Cockpit-Ton war zu leise, jetzt 2,4 kHz bei
      vollem Pegel (#5)
- [x] Speichertabelle pro Startphase eingetragen
- [x] Speicher mit WLAN, Signal K-Stream und Freigabe gemessen (74 KB frei, Tiefstwert 55 KB)
- [ ] ILI9342C/E auf dem Bus erkennen statt per Kconfig wählen

**Fertig, wenn:** Setup-Portal, Signal K-Freigabe, eine LVGL-Testseite mit Touch
und ein Testton laufen, und die Speichertabelle hier steht.

### Phase 2: JLP-Kern

- [x] `main/jlp/` aus dem Cockpit (`c3eaeed`) übernommen; Voice und Stream hinter
      `JLP_VOICE`/`JLP_STREAM` (beide 0), N2K nicht übernommen (`main/jlp/README.md`)
- [x] `/hello` (320×240, Widget-Liste, Firmware `m5-tough-jlp-<version>`), `/layout`,
      `/screen`, `/healthz`, `/screenshot` (JPEG und BMP), `/beep`,
      mDNS `_signalk-player._tcp` auf Port 8081
- [x] Layout im eigenen NVS-Bereich `layout` (eigene Partitionstabelle
      `partitions-16mb.csv` = espOS-16mb.csv + 128 KB), Default-Layout für 320×240
- [x] Alle Widgets des Cockpits außer Voice und Stream: `label`, `value`, `toggle`,
      `arc`, `bar`, `bargroup`, `button`, `notifications`, `anchor`, `anchor_track`,
      `slider` (auch `@brightness`), dazu Alarm-Overlay, Chime, Idle-Dimmer
- [x] Status- und Alarm-Overlay für 320×240 angepasst
- [x] Build ohne Warnungen, Image 1,9 MB (28 % des App-Slots)
- [x] Per USB geflasht (neue Partitionstabelle), Speicher gemessen und
      entschärft (Tabelle oben)
- [x] Designer auf dem Halpi2 verbindet sich, Layout gepusht (zwei Screens,
      Anker-Widgets), Layout übersteht Neustarts, Mirror-Modus läuft
- [x] Verbindungsabbrüche nach Neustart gefunden: der MikroTik behält die alte
      WLAN-Sitzung („reconnecting“). Abhilfe: die Tough meldet sich nach der
      ersten Verbindung jedes Starts einmal ab und wieder an
- [x] Issue für espOS: Neuanmeldung nach Neustart an MikroTik-Routern
      ([signalk-espOS/espOS#289](https://github.com/signalk-espOS/espOS/issues/289));
      der Workaround in `main/app_main.cpp` fällt weg, sobald espOS es löst

**Fertig, wenn:** Der Designer findet das Gerät, und ein geschobenes Layout zeigt
Live-Daten vom Halpi2.

### Phase 3: Die Use Cases

- [x] `anchor`, `anchor_track` zeigen Daten vom Halpi2
- [x] `@drop_here` am 3. November 2026 mit Freigabe getestet (PUT 200, das
      Anker-Plugin übernimmt den Punkt; die Wache danach in der Anker-App neu setzen)
- [x] `notifications`-Liste, Alarm-Overlay für 320×240, Chime: echte Test-Alarme
      (Warnung, Alarm, Notfall) vom Halpi2 mit Ton und Quittieren (#5)
- [x] `@audio_mute` am Gerät geprüft: Overlay ja, Ton nein
- [x] `@brightness` über AXP192 LDO3 (manuell): 9 Stufen von 2,5 bis 3,3 V, die
      5-%-Stufe reicht nachts
- [x] Idle-Dimmer und Nachtdimmung über `environment.mode` bei Nacht geprüft
      (60 s Leerlauf: Backlight aus, Antippen: zurück auf die eingestellte Helligkeit)
- [x] `/screenshot` (JPEG und BMP) für den Mirror-Modus, `POST /screen`

**Fertig, wenn:** Ein Tough-Layout mit Anker, SoC, Wind und Notifications läuft
eine Nacht lang am Halpi2 ohne Neustart.

- [x] Nachttest 2./3. November 2026: 12,5 h ohne Neustart und ohne Core-Dump,
      WLAN und Signal K-Stream durchgehend verbunden (0 WLAN-Abbrüche, 0
      Sendefehler, 302 865 Frames empfangen), alle Health-Werte normal, interner
      RAM-Tiefstwert 37 KB wie im Tagbetrieb

### Phase 4: Release und Flotte

- [ ] release-please (`"initial-version": "0.1.0"`), espOS `release-firmware.yml`
      mit `flash_tables: "16mb"`
- [x] `espos_start_opts_t.board` fest auf `"M5Stack Tough"`
- [ ] Registry: `boards/m5stack-tough.json` und `projects/<id>.json`
- [ ] OTA über signalk-espos-manager testen
- [ ] README für Bootseigner (Hardware, 12-V-Anschluss, Flasher, Setup, Updates)

### Phase 5: Später

- [ ] `wind`-Widget (Firmware, Designer, WASM)
- [ ] NMEA 0183 über den RS485-Anschluss (`espos_nmea0183`), falls RS485 später gebraucht wird
- [ ] Uhrzeit aus der RTC BM8563
- [x] Eigene Nachthelligkeit `cockpit.night_bright` (0 = aus), die gilt, solange
      `environment.mode` auf `night` steht; der `@brightness`-Slider verstellt die
      gerade aktive, Alarme bleiben gedimmt (die Hardware kann nicht dunkler als
      die 5-%-Stufe, 2,5 V an LDO3)
- [x] Notifications je Stufe in der Firmware: `notifications.levels.<stufe>` mit
      `show`, `sound` (aus / wie Quelle / immer) und `repeat_s`
- [ ] Notifications je Stufe im Designer (PR an signalk-espos-hmi-designer, bis
      dahin lokaler Build auf dem Halpi2) und in `JLP-PROTOCOL.md` und im Cockpit
- [x] Statusleiste bei Bedarf: ein Layout mit `status_overlay: false` nutzt die
      vollen 240 px, Wischen vom oberen Rand blendet die Leiste für
      `cockpit.status_peek_s` (Vorgabe 5 s) über den Widgets ein, ohne Signal K
      steht sie dauerhaft
- [x] Tab-Leiste als Einblendung: `tab_strip: "overlay-bottom"` gibt den
      Seiten die volle Höhe (240 px mit `status_overlay: false`, sonst
      212 px), Wischen vom unteren Rand blendet die Leiste ein;
      waagerechtes Wischen blättert in jedem Layout mit mehreren Seiten (mit
      Dirk abgestimmt in espos-p4-cockpit #209)
- [ ] Tab-Leiste als Einblendung im Cockpit, im Designer und in der
      WASM-Vorschau (PRs an die Repos)
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
