# ESP32 Agent Dashboard

Un piccolo schermo touch sulla scrivania per vedere cosa stanno facendo le sessioni di Claude Code sul Mac. Tocca una card per vedere stato, contesto, token e ultimo prompt. In alto trovi la quota residua dei limiti Claude e Codex.

Il display è un **ESP32-2432S028R** (Cheap Yellow Display, 2,8", touch resistivo XPT2046). Il programma sul Mac legge le sessioni locali e invia i dati alla scheda tramite **micro USB**. Il Wi-Fi è opzionale. Il Mac deve rimanere acceso e il server deve essere in esecuzione.

## Cosa serve

- [ESP32-2432S028R su AliExpress](https://s.click.aliexpress.com/e/_c3GyE0zh). Controlla che sia la variante **R, 2,8", ILI9341 e touch resistivo**: schede simili possono avere pin diversi.
- Cavo **micro USB dati**. Su questa revisione la porta USB-C potrebbe non alimentarsi con un cavo C-to-C.
- Mac con Arduino IDE 2.x e Python 3.
- Claude Code installato; Codex è opzionale per la percentuale di utilizzo.
- Facoltativa: [cover stampabile su MakerWorld](https://makerworld.com/it/models/2171220-slim-minimal-case-for-esp32-cyd-2-4-2-8#profileId-2354976). Scegli il profilo 2,8" e controlla la posizione delle porte della tua scheda.

## Installazione semplice via USB

1. In Arduino IDE installa la piattaforma **esp32 by Espressif Systems** e le librerie **TFT_eSPI**, **XPT2046_Touchscreen** e **ArduinoJson**. Versioni usate qui: TFT_eSPI 2.5.43, XPT2046_Touchscreen 1.4, ArduinoJson 7.4.3.
2. Copia [`cyd_dash/User_Setup.h`](cyd_dash/User_Setup.h) al posto di `User_Setup.h` nella libreria TFT_eSPI. Sul Mac è spesso in `~/Documents/Arduino/libraries/TFT_eSPI/`. Salva prima una copia del file originale.
3. Collega la scheda al Mac con il cavo micro USB dati.
4. Nel Terminale, dalla cartella del progetto, esegui:

   ```sh
   ./flash.sh upload cyd_dash
   server/run.sh --serial auto
   ```

Lascia aperto il Terminale con il server. Il primo comando compila e carica il firmware; il secondo aggiorna lo schermo ogni 3 secondi. Per vedere solo le sessioni senza leggere i limiti degli account, usa `server/run.sh --serial auto --no-usage`.

Se `arduino-cli` non è dentro Arduino IDE nella posizione predefinita, imposta `ARDUINO_CLI` al percorso del tuo eseguibile. `./flash.sh ports` mostra le porte seriali disponibili.

## Wi-Fi opzionale

Crea un file `.env` nella radice del progetto:

```env
WIFI_SSID=NomeRete
WIFI_PASS=password
DASH_HOST=192.168.1.10
DASH_PORT=8765
```

`DASH_HOST` è l'IP locale del Mac, non l'IP della scheda. Sul Mac puoi leggerlo con `ipconfig getifaddr en0`. Ricarica il firmware con `./flash.sh upload cyd_dash`, poi avvia `server/run.sh`. La scheda prova il Wi-Fi all'accensione e, se non riesce a connettersi, usa la USB. Non pubblicare `.env` o `cyd_dash/secrets.h`.

## Cosa vedi

- **Griglia:** fino a 16 sessioni in due pagine. Scorri lateralmente o tocca le frecce in basso.
- **Dettaglio:** stato, tool in uso, contesto, token in uscita, cache, turni, ultimo prompt e pulsante per portare in primo piano l'editor.
- **Usage:** percentuale residua delle finestre di utilizzo Claude e Codex. È una **percentuale di quota**, non un conteggio assoluto dei token rimasti. Tocca CL/CX in alto per aprirla.

Le sessioni e i transcript Claude Code sono letti dal Mac. Il server consulta i limiti tramite le credenziali locali di Claude/Codex e invia al display solo percentuali ed eventuali messaggi di errore, mai i token di accesso. Non esporre la porta HTTP `8765` su Internet.

## File del progetto

- `cyd_dash/cyd_dash.ino`: firmware del display.
- `cyd_dash/User_Setup.h`: configurazione TFT_eSPI per la scheda usata.
- `server/dash_server.py`: lettura sessioni, limiti e API locale.
- `server/run.sh`: avvio del server con riconoscimento della porta USB.
- `flash.sh`: compilazione e caricamento tramite Arduino CLI.

## Se qualcosa non va

- **Display bianco o colori invertiti:** verifica modello della scheda e `User_Setup.h`.
- **Nessuna porta USB:** prova un cavo dati e `./flash.sh ports`.
- **Tocchi sfalsati:** regola `T_XMIN/T_XMAX/T_YMIN/T_YMAX` in `cyd_dash.ino`.
- **Limiti non disponibili:** avvia senza `--no-usage`; macOS potrebbe chiedere accesso al Portachiavi.
