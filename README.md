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
| `<topic>` (standaard `revspace/flipdot`) | naar het display | tekst. Regels scheid je met `\n`, pagina's met `\n\n`. Eén regel krijgt het grote font, twee regels het kleine. |
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

- **ESP TX = GPIO15** (na `Serial.swap()`), 1200 baud 7E2, naar de IBIS-ingang van de controller.
- **Terugkoppeling (optioneel):** ESP RX = GPIO13 aan de antwoordkant van de controller (CPU-TXD / testpunt TP TXD), **via een spanningsdeler naar 3,3 V**. Zonder deze verbinding meldt de ESP het display als `offline` en elke tekst als `timeout`; het display zelf werkt dan gewoon.
- Schakelaar S2 op de controller moet in de stand staan waarin de seriële ontvangst aan staat. S1 bepaalt of ASFP tekst automatisch omzet naar Hoofdletter-aan-het-begin.

## Bouwen

```sh
pio run -t upload
```

If you want a license: MIT License
