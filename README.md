# flipdot-ret-mqtt

> [!WARNING]
> De uitbreidingen in deze versie (WiFiManager, testbeelden en terugkoppeling) zijn geschreven door een AI-tool (Claude Code).
> Ze compileren, maar zijn **niet getest op echte hardware**.
>
> De oude versie, die gegarandeerd werkt (alleen MQTT-tekst, vaste WiFi-instellingen), staat hier:
> [commit 516d74e](https://github.com/sebastius/flipdot-ret-mqtt/tree/516d74efc74584d3be5404fb560bb7d9deb0702e).

ESP8266-firmware (NodeMCU) die MQTT-berichten als IBIS-telegram naar een LAWO-flipdotdisplay (AZR 802/24 met ASFP-firmware) stuurt.
Achtergrond over de controller staat in [sebastius/lawo-flipdot](https://github.com/sebastius/lawo-flipdot).

## MQTT

| Topic | Richting | Inhoud |
|---|---|---|
| `<topic>` (standaard `revspace/flipdot`) | naar het display | tekst. Regels scheid je met `\n`. Na een pagina van twee regels volgt `\n\n`, na een pagina van één regel `\n\n\n`. Anders valt het eerste teken van de volgende pagina weg (getest in de [emulator](https://sebastius.github.io/lawo-flipdot/emulator/web/)). Eén regel krijgt het grote font als hij past, twee regels het kleine. |
| `<topic>/test` | naar het display | testbeeld: `0` zwart, `1` geel, `2` dambord, `3` omgekeerd dambord |
| `<topic>/query` | naar het display | `status` of `version` |
| `<debugtopic>` (standaard `revdebug/flipdot`) | van de ESP | `hello world` bij het verbinden |
| `<debugtopic>/ack` | van de ESP | `ok` of `timeout` na elke tekst |
| `<debugtopic>/display` | van de ESP | `online` / `offline` (retained); wordt elke 60 s opgevraagd |
| `<debugtopic>/version` | van de ESP | versie van de displayfirmware, bijvoorbeeld `F00293` (retained) |
| `<debugtopic>/raw` | van de ESP | onbekende antwoorden en meldingen |

Teksten langer dan 238 tekens worden afgekapt. ASFP negeert een telegram van meer dan ongeveer 249 tekens zonder melding.

## Instellen (WiFiManager)

- **Zonder werkende WiFi** start de ESP een access point `Flipdot-XXXXXX` (het display toont de naam). Verbind ermee en open 192.168.4.1.
- **Met WiFi** is dezelfde instellingenpagina bereikbaar op het IP-adres dat bij het opstarten op het display verschijnt, via menu *Setup*.
- Instelbaar: MQTT-server, poort, luistertopic, debugtopic en het IBIS-displayadres (1–15). De instellingen worden opgeslagen in LittleFS (`/config.json`).

## Aansluiting

De ESP hangt direct aan de 80C31 op de AZR 802/24. De IBIS-businterface (optocoupler en inverter) wordt niet gebruikt.

![Aansluiting ESP8266 op de 80C31: GPIO15 direct naar P3.0/RXD, P3.1/TXD via spanningsdeler 47k/100k naar GPIO13](docs/rx-spanningsdeler.svg)

- **ESP TX = GPIO15** (na `Serial.swap()`), 1200 baud 7E2, **direct naar P3.0/RXD (pin 10)** van de 80C31.
  De uitgang van de 74HC14 (IC1A) die normaal RXD aanstuurt, is losgekoppeld, anders vechten de twee om die lijn.
  3,3 V is genoeg, want de 80C31 ziet een spanning boven ongeveer 1,9 V als "hoog".
- **Terugkoppeling (optioneel):** ESP RX = GPIO13 aan **P3.1/TXD (pin 11)**, oftewel testpunt TP TXD, **via een spanningsdeler naar 3,3 V**.
  Zonder deze verbinding meldt de ESP het display als `offline` en elke tekst als `timeout`; het display zelf werkt dan gewoon.

  De ESP8266 kan niet tegen 5 V. De logica van de controller werkt op 5 V. R1/R2 brengen het hoge niveau terug naar ongeveer 3,4 V.
  De weerstanden zijn bewust hoog gekozen, omdat de TXD-pin van de 80C31 een "hoog" maar zwak vasthoudt (enkele tientallen µA).
  De massa van de ESP gaat naar de logische massa van de controller, niet naar de geïsoleerde IBIS-kant.
  Een BSS138-niveauomzetter mag ook.
- Schakelaar S2 op de controller moet in de stand staan waarin de seriële ontvangst aan staat. S1 bepaalt of ASFP tekst automatisch omzet naar Hoofdletter-aan-het-begin.

## Bouwen

```sh
pio run -t upload
```

If you want a license: MIT License
