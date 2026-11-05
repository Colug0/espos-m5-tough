# espos-m5-tough

Firmware that turns an [M5Stack Tough](https://docs.m5stack.com/en/core/Tough)
into a small multi-function display for a Signal K server. It shows status
(anchor watch, battery state of charge, wind) and Signal K notifications, with
an alert overlay and a tone from a configurable minimum severity.

The screens are designed in the
[espOS HMI Designer](https://github.com/signalk-espOS/signalk-espos-hmi-designer)
and pushed to the display over the network, with no reflash. The firmware is
built on [espOS](https://github.com/signalk-espOS/espOS) and plays the same JSON
layouts as [espos-p4-cockpit](https://github.com/dirkwa/espos-p4-cockpit).

**Status:** in development, nothing to flash yet. The plan is in
[PLAN.md](PLAN.md).

## Licence

Apache-2.0, see [LICENSE](LICENSE). The number fonts in `main/jlp/fonts/`
are generated from Montserrat and stay under the SIL Open Font License 1.1
([OFL.txt](main/jlp/fonts/OFL.txt)).
