# espos-m5-tough

Firmware that turns an [M5Stack Tough](https://docs.m5stack.com/en/core/Tough)
into a small multi-function display for a Signal K server. It shows values,
gauges, bars, an anchor watch and Signal K notifications, with an alert overlay
and a chime per severity. Pages switch with a swipe, the status and tab strips
can be hidden and brought back with a swipe, and number readouts grow with
their tile up to the whole screen.

The screens are designed in the
[espOS HMI Designer](https://github.com/signalk-espOS/signalk-espos-hmi-designer)
and pushed to the display over the network, with no reflash. The firmware is
built on [espOS](https://github.com/signalk-espOS/espOS) and plays the same JSON
layouts as [espos-p4-cockpit](https://github.com/dirkwa/espos-p4-cockpit).

**Status:** in use on a boat. Releases are on the
[releases page](https://github.com/Colug0/espos-m5-tough/releases); the plan
for what comes next is in [PLAN.md](PLAN.md).

## What you need

* An M5Stack Tough (K034: ESP32, 16 MB flash, 320x240 touch display).
* A Signal K server on the same network.
* To design your own screens: the `signalk-espos-hmi-designer` plugin from the
  Signal K app store. Without a layout the display shows a default page.

## Flash it

Each release has two images: `m5-tough-esp32-v<version>-merged.bin` for a
first flash over USB and `m5-tough-esp32-v<version>-ota.bin` for updates.

* **Web flasher:** once the firmware is listed in the
  [espOS registry](https://github.com/signalk-espOS/registry), the
  [web flasher](https://signalk-espos.github.io/signalk-espos-manager/flash/)
  offers it under the M5Stack Tough. It needs Chrome or Edge.
* **esptool:** download the merged image of the latest release and write it
  from address 0, with the Tough on USB (on Windows the port is a `COMx`; the
  Tough's CH9102 USB bridge may need its driver):

  ```sh
  esptool --chip esp32 -p /dev/ttyACM0 -b 460800 write-flash 0x0 m5-tough-esp32-v<version>-merged.bin
  ```

A merged image replaces everything on the device, its WiFi settings included.
Update a running display over the air instead.

## Set it up

1. **WiFi.** With no network configured, the display opens a setup network
   called `espOS-<id>`. Join it with a phone or laptop; the setup page opens by
   itself, or open `http://192.168.4.1/`. Pick your network and enter its
   password. The display joins it and the setup network goes away.
2. **Signal K.** The display finds the server over mDNS and asks it for
   access. Approve the request in the Signal K admin page under
   **Security → Access Requests**. The status strip at the top shows `sk:ok`
   once the data streams.
3. **Web UI.** The device's own page is at `http://espos-<id>.local/` (or its
   IP address): settings, logs, updates.

## Design the screens

Open the HMI Designer from the Signal K admin page, enter the display's
address with port 8081 (`http://<display-ip>:8081`; use the IP if the server
cannot resolve `.local` names) and press **Connect**. The canvas takes the
Tough's 320x240 size. Lay out the widgets, then **Push**: the display switches
to the new layout at once and keeps it across restarts.

## Updates

On the device's web UI, open the OTA page and use **From a URL** with the
`ota.bin` link of a newer release, or update from the
[signalk-espos-manager](https://github.com/signalk-espOS/signalk-espos-manager)
plugin. The display only takes images signed with this project's key, and
goes back to the previous firmware if the new one does not come up. Settings
and the layout are kept.

## MikroTik access points

After a power cut or a crash, a MikroTik access point (RouterOS 7) can keep
the display's old connection and deliver nothing to it for minutes. Turn on
`wifi.fresh_assoc` in the device's web UI under WiFi: the display then drops
its first association after each start and joins again, about two seconds per
boot. Other access points do not need it. Up to v0.5.0 the firmware did this
on every boot by itself and had no setting for it; after updating from such a
version on a MikroTik network, turn the setting on. An OTA update restarts
cleanly, so the access point lets go of the old connection; after a USB flash
the display can take a few minutes to become reachable for that once.

## Building

ESP-IDF v6.0.3, built through espOS's build wrapper. [AGENTS.md](AGENTS.md)
has the commands, the board facts and the rules for contributions.

## Licence

Apache-2.0, see [LICENSE](LICENSE). The number fonts in `main/jlp/fonts/`
are generated from Montserrat and stay under the SIL Open Font License 1.1
([OFL.txt](main/jlp/fonts/OFL.txt)).
