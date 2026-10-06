# BerryMaps — progetto di un'app mappe nativa per BlackBerry Q5 (BB10)

> Nome provvisorio. Documento di progetto, ottobre 2026. Nessun codice ancora scritto.
> Obiettivo: app Cascades **autonoma** (niente server, niente BerryCore) che usa Google Maps Platform
> direttamente dal telefono, restando nelle quote gratuite.

---

## 1. Obiettivi e principi

**Cosa deve fare (in ordine di priorità)**
1. Mappa fluida e navigabile (pan, zoom, pinch), stradale + satellite.
2. "Dove sono": punto blu con cerchio di precisione e direzione (bussola).
3. Ricerca luoghi/indirizzi con suggerimenti mentre scrivi (tastiera fisica del Q5).
4. Percorsi (auto, a piedi, bici, mezzi) con linea sulla mappa ed elenco indicazioni.
5. Navigazione guidata: istruzione successiva grande, ricalcolo se esci dal percorso, Active Frame.
6. Integrazione con BB10: aprire indirizzi da Contatti/Calendario/link, condividere una posizione.

**Principi**
- **Costo zero**: ogni chiamata API è contata e limitata; tetti giornalieri nella console Google.
- **Batteria prima di tutto**: GPS acceso solo quando serve, mai in background fuori dalla navigazione.
- **Nativo BB10**: Cascades, tastiera fisica, Active Frame, invocazioni di sistema, tema scuro.
- **Nessuna dipendenza esterna** a runtime: niente server CachyOS, niente BerryCore.

---

## 2. Fatti verificati (ricerca + header NDK)

### 2.1 Hardware Q5 / ambiente
| Voce | Valore | Conseguenza |
|---|---|---|
| Schermo | 3,1" 720×720, ~330 ppi | Tessere **@2x (512 px)**, altrimenti le scritte della mappa sono illeggibili |
| SoC | **MSM8960** (Snapdragon S4 Plus), 2× Krait **1512 MHz** (misurato), Adreno 225, 2 GB RAM LPDDR2 | La GPU muove bene le immagini; decodificare PNG grandi costa CPU → limitare decodifiche (dettagli §15) |
| GNSS | GPS + GLONASS, motore Qualcomm (`qct_gps`) | A-GPS via XTRA funzionante (vedi §6.2 e §15) |
| Batteria | 2180 mAh | Navigazione = schermo acceso + GPS + dati: è lo scenario critico |
| Tastiera | QWERTY fisica, niente frecce | Scorciatoie a lettera singola |
| SDK | NDK 10.3.1 (`C:\bbndk\ndk\target_10_3_1_995`), Qt 4.8, Cascades | Niente QtQuick, niente `Connections` (vedi memoria MiniBrowser) |

### 2.2 Google Maps Platform (prezzi da marzo 2025)
- Niente più credito da 200 $: **quota gratuita mensile per SKU**.
  - **Map Tiles API (tessere 2D): 100.000/mese gratis**.
  - Altri SKU *Essentials*: 10.000/mese; *Pro*: 5.000/mese; *Enterprise*: 1.000/mese.
- Le quote **non si sommano** tra API: ognuna ha la sua.
- Serve comunque un account di fatturazione con carta.

### 2.3 Termini d'uso — si applicano i termini **EEA**
Con indirizzo di fatturazione in Italia (progetto creato dopo l'8 luglio 2025) vale il
*Google Maps Platform EEA Terms of Service*. Differenze importanti rispetto ai termini globali:
- I termini **globali** vietano di usare i contenuti Maps con **text-to-speech** e di creare navigazione
  "sostanzialmente simile a Google Maps per Android". Nei termini **EEA** questi divieti **non compaiono**:
  esiste invece una pagina **"Safety requirements for Real-Time Driving and Navigation Applications"**,
  cioè la navigazione in tempo reale è permessa, a condizioni di sicurezza.
- Requisiti di sicurezza da rispettare (riassunto):
  1. Informazioni leggibili "a colpo d'occhio" quando il veicolo è in movimento; niente elementi superflui.
  2. Interazioni complesse **bloccate o semplificate in movimento** (es. digitare una ricerca).
  3. Monitorare la qualità del GPS: se la precisione peggiora (gallerie, canyon urbani) **mostrare un
     indicatore chiaro** e sospendere le indicazioni precise di manovra finché non torna buona.
  4. Non presentare dati vecchi come "in tempo reale".
  5. Il percorso attivo deve essere ben distinto (colore/spessore) dalla mappa e dalle alternative.
  6. (Il ricalcolo ogni 15 s è richiesto solo quando si mescolano percorsi/mappe di terzi — non il nostro caso.
     Noi ricalcoliamo comunque a ogni deviazione.)
- **Cache**: vietata salvo quanto permesso; per Map Tiles bisogna **rispettare `Cache-Control`**
  (`max-age`, `stale-while-revalidate`, `must-revalidate`, `private`). **Niente pre-fetch, niente download
  per uso offline.**
- **Attribuzione** obbligatoria: logo Google Maps (16–19 dp, non modificato, con margini 10 dp/5 dp) o testo
  "Google Maps", più la stringa copyright restituita dall'API (in basso a destra).
- **Privacy posizione**: salvare la posizione dell'utente solo con consenso esplicito e revocabile →
  schermata di consenso al primo avvio, opzione per cancellare la cronologia.
- Da ricontrollare al momento dell'attivazione: i termini cambiano senza preavviso.

### 2.4 Map Tiles API — dettagli tecnici
- `POST https://tile.googleapis.com/v1/createSession?key=KEY` con JSON:
  `{"mapType":"roadmap","language":"it-IT","region":"IT","scale":"scaleFactor2x","highDpi":true,"imageFormat":"png"}`
  (anche `satellite`, `terrain`; `layerTypes: ["layerTraffic"]` per il traffico; `styles` per uno stile scuro).
- Risposta: `session`, `expiry` (circa **2 settimane**), `tileWidth`, `tileHeight`, `imageFormat`.
- Tessere: `GET https://tile.googleapis.com/v1/2dtiles/{z}/{x}/{y}?session=…&key=…` (zoom 0–22).
- Attribuzione: `GET https://tile.googleapis.com/tile/v1/viewport?session=…&key=…&zoom=&north=&south=&east=&west=`
  → `copyright` + `maxZoomRects` (evita di chiedere zoom inesistenti, es. sul mare).

### 2.5 GPS su BB10 — cosa offre l'NDK (verificato negli header)
Due livelli disponibili:
- **QtLocationSubset** (`QGeoPositionInfoSource`, `QGeoSatelliteInfoSource`): semplice; proprietà
  specifiche BB10 tramite `setProperty()`: `provider` (`gnss`/`network`), `fixType`
  (`gps_autonomous`, `gps_ms_based`, `gps_ms_assisted`, `cellsite`, `wifi`), `slpURL`, `qtaPDEURL`,
  `reset` (`cold`/`warm`/`hot`), `canRunInBackground`. Esempio ufficiale: *Cascades Location Diagnostics*.
- **`location_manager.h`** (API C di basso livello `lm_*`), più controllo:
  - `lm_loc_req_set_period`, `_accuracy`, `_response_time`, `_provider_type` (HYBRID/GNSS/NETWORK),
    `_fix_type`, **`lm_loc_req_set_slp_url`** (server A-GPS SUPL).
  - Flag preziosi per la batteria:
    - `LM_LOC_REQ_FLAG_STATIONARY_DETECTION`: usa sensori a basso consumo per capire che sei fermo e
      **sospende gli aggiornamenti GPS**, riprendendo da solo quando ti muovi.
    - `LM_LOC_REQ_FLAG_LAST_KNOWN_FIX_FIRST`: restituisce subito l'ultima posizione nota mentre calcola
      quella nuova (avvio istantaneo, meno energia).
    - `LM_LOC_REQ_FLAG_PASSIVE`: riceve posizioni solo se qualcun altro ha acceso il GPS (costo zero).
    - `LM_LOC_REQ_FLAG_BACKGROUND`: continua in standby (serve anche il permesso app).
    - `LM_LOC_REQ_FLAG_REPORT_SAT`: satelliti visibili mentre cerca il fix (per la UI "ricerca segnale").
- **Wi-Fi scan** (`wifi/wifi_service.h`: `wifi_get_scan_results`, BSSID, frequenza, segnale) e
  **cella** (`bb::device::CellularNetworkInfo`: `cellId`, LAC/TAC, MCC/MNC) disponibili → fallback con la
  Geolocation API di Google (vedi §6.3).
- Bussola: `sensor/sensor.h` (azimut) per la freccia di direzione da fermi. Sul Q5 esistono
  `/dev/sensor/mag`, `/dev/sensor/compass`, `rotVectMag` (verificato via SSH): il magnetometro c'è
  anche se l'inventario hardware non lo elenca → da provare che dia letture sensate.
- **Nessuna API di sintesi vocale** nell'NDK (cercato: nessun header TTS/speech).
- `bb::platform::MapInvoker` / `LocationMapInvoker` / `RouteMapInvoker`: sono gli invocatori con cui le app
  di sistema aprono "le mappe" → la nostra app può registrarsi per riceverli (vedi §9).

### 2.6 Codice riutilizzabile già esistente
- `MiniBrowser/client/src/tls/` (da BBport): `TlsNetworkAccessManager` (sottoclasse di
  `QNetworkAccessManager`, mbedTLS, TLS 1.2), cache delle sessioni TLS per host, limite di richieste
  concorrenti, gestione chunked e redirect.
- `cacert.pem` contiene già **GTS Root R1/R3/R4** e GlobalSign R4 → i certificati Google si verificano.
- **Limite**: ogni richiesta apre una connessione nuova (`Connection: close`) e un thread → va bene per
  ricerca/percorsi, **non per le tessere** (decine di richieste in pochi secondi). Serve un pool keep-alive (§4).
- `package.ps1 -Install` per build + installazione via SSH (come MiniBrowser).

---

## 3. Architettura

```
QML (Cascades)                         C++ (Qt 4.8)
───────────────                        ──────────────────────────────────────────
main.qml  NavigationPane               AppController      stato globale, ciclo di vita app
 ├ MapPage                             MapEngine          proiezione, griglia tessere, zoom, pan
 │  ├ MapView.qml (griglia+overlay)    TileStore          cache RAM (LRU) + disco + HTTP cache
 │  ├ barra ricerca / FAB posizione    TileFetcher        pool keep-alive mbedTLS verso tile.googleapis.com
 │  └ attribuzione Google              OverlayRenderer    linea percorso/pin disegnati con QPainter per tessera
 ├ SearchPage (suggerimenti, risultati) PlacesClient      Autocomplete/Text Search/Details (Places API New)
 ├ PlacePage (dettaglio, "Indicazioni") RoutesClient      computeRoutes (Routes API v2), decodifica polyline
 ├ RoutePage (modalità, elenco passi)   GeocodeClient     indirizzo ↔ coordinate (pin lasciato col dito)
 ├ NavigationPage (guida)               LocationService   wrapper lm_* / QtLocation, politiche di consumo
 ├ SettingsPage                         Navigator         map-matching, avanzamento passi, deviazioni, ricalcolo
 └ ActiveFrame (cover)                  QuotaGuard        contatori giornalieri/mensili per SKU, blocchi
                                        ApiKeyStore       chiave API (offuscata nel .bar)
                                        Settings          QSettings
```

- Linguaggio: C++ per tutto ciò che è logica/rete/calcolo, QML solo per l'interfaccia.
- Comunicazione C++ → QML: proprietà `Q_PROPERTY` + segnali, connessi in `onCreationCompleted`
  (`controller.sig.connect(fn)`), **mai** `Connections`.
- Thread: rete e decodifica immagini fuori dal thread UI; il thread UI riceve solo oggetti pronti.

---

## 4. Rete

### 4.1 Pool keep-alive per le tessere (componente nuovo, il più importante)
Problema: con il client attuale ogni tessera = connessione TCP + handshake TLS + thread. Con 9–25
tessere per schermata significa latenza alta e **molto consumo radio/CPU**.

Soluzione: `TileFetcher` con **2–4 connessioni persistenti** (HTTP/1.1 keep-alive) verso
`tile.googleapis.com`, ognuna in un thread fisso con coda di richieste:
- Riusa le funzioni mbedTLS di `tlsnetworkreply.cpp` (handshake, verifica CA, cache sessione), ma senza
  `Connection: close`: legge `Content-Length`/chunked e manda la richiesta successiva sulla stessa connessione.
- Coda con **priorità**: tessere visibili > anello attorno > tutto il resto; quando l'utente si sposta,
  le richieste diventate inutili vengono **scartate** prima di partire.
- Riconnessione automatica se il server chiude (timeout inattività ~60 s lato Google: va misurato).
- Dopo ~30 s senza richieste, chiudere le connessioni (la radio può tornare in risparmio).
- Opzionale più avanti: HTTP/2 non disponibile con il nostro stack → restare su HTTP/1.1.

Le altre API (Places, Routes, Geocoding) usano il `TlsNetworkAccessManager` esistente così com'è.

### 4.2 Cache HTTP conforme
Per ogni tessera si salvano su disco `ETag`/`Last-Modified` e la scadenza calcolata da `Cache-Control`.
- Entro `max-age`: si usa senza rete.
- Scaduta ma entro `stale-while-revalidate`: si mostra subito e si rivalida in background.
- `must-revalidate`: niente uso dopo la scadenza senza rivalidazione.
- Rivalidazione con `If-None-Match` → risposta `304` (piccola, ma potrebbe contare come richiesta: verificare).
- Il valore reale di `max-age` delle tessere Google va **misurato** (determina quante chiamate risparmiamo).

### 4.3 Robustezza
- Timeout: connessione 10 s, risposta 15 s; tre tentativi con backoff (1 s, 3 s, 9 s).
- Rete assente: la mappa mostra le tessere in cache (solo quelle ancora valide) e un banner "offline".
- Cambio rete (Wi-Fi ↔ dati): chiudere e riaprire il pool (`QNetworkConfigurationManager` / netstatus BPS).
- Orologio del telefono sbagliato → la verifica TLS fallisce: messaggio chiaro "controlla data e ora".

---

## 5. Motore mappa

### 5.1 Proiezione e matematica
- Web Mercator (EPSG:3857), coordinate "mondo" in pixel a zoom `z`: `size = 256 · 2^z` punti logici.
- Tessera `x = floor((lon+180)/360 · 2^z)`, `y = floor((1 − ln(tan φ + sec φ)/π)/2 · 2^z)`.
- Tessere @2x: immagine 512 px fisici = 256 punti logici → su 330 ppi la densità è come su un telefono "retina".
- Stato della mappa in `double`: centro (lat, lon), zoom frazionario (durante il pinch), rotazione = 0.

### 5.2 Rendering — tre strategie (rischio tecnico n.1)
Cascades **non ha un canvas** né una vista mappa usabile (la `bb::cascades::maps::MapView` usava i server
BlackBerry/HERE, oggi non affidabili). Le opzioni:

**A. ScrollView "virtuale" (preferita da provare per prima)**
- Un `ScrollView` con `scrollMode: Both` e pinch-to-zoom nativo contiene un `Container` grande (es. 3×3
  schermate) con la griglia di `ImageView` posizionate in `AbsoluteLayout`.
- Vantaggio: **inerzia, rimbalzo e pinch nativi**, fluidi perché animati dal thread di rendering.
- Quando lo scroll si ferma (`onViewableAreaChanged` + fine movimento) si **ricentra**: si spostano le tessere
  e si riporta lo scroll al centro nello stesso frame (va verificato che non si veda un salto).
- Lo zoom del ScrollView (`contentScale`) si converte, a fine gesto, nel livello intero più vicino.

**B. Gestione manuale del tocco**
- `onTouch` (Down/Move/Up) sposta un `Container` padre con `translationX/Y` (nessun ricalcolo di layout).
- `PinchHandler` per `scaleX/scaleY` con `pivotX/pivotY` sul punto medio delle dita.
- Inerzia da implementare a mano (animazione `TranslateTransition` con easing calcolato dalla velocità finale).
- Più controllo, più lavoro; fallback se A dà problemi.

**C. OpenGL ES 2.0 in una finestra Screen sotto Cascades (`ForeignWindowControl`)** — la soluzione "da
app mappe vera", dettagliata in §16. In sintesi: la mappa è disegnata dalla GPU in una finestra nativa con
Z-order negativo; Cascades "buca" la sua UI e disegna barra di ricerca, pulsanti e schede **sopra** la mappa.
Zoom continuo, linea del percorso disegnata dalla GPU, nessun limite degli ImageView.

Strategia: prima A (veloce da fare), ma con `MapEngine`/`TileStore` **indipendenti dal renderer**, così
passare a C non richiede di riscrivere rete, cache e proiezione.

Nei casi A e B:
- `implicitLayoutAnimationsEnabled: false` su tutti i contenitori della mappa (altrimenti ogni spostamento
  di tessera viene animato).
- **Pool di ImageView riciclate** (es. 25): non si creano/distruggono oggetti durante il pan, si cambia
  posizione e `imageSource`.
- Le tessere si caricano da file con **percorso assoluto `file:///…`**: secondo la documentazione BlackBerry
  è l'unico modo in cui Cascades carica le immagini **in modo asincrono** (con `asset:///` o percorsi relativi
  il caricamento blocca). In alternativa `bb::ImageData` da memoria se il caricamento da file risulta lento.
- `loadEffect: ImageViewLoadEffect.None` sulle tessere: di default ogni immagine appena caricata fa una
  dissolvenza (animazione implicita = ridisegno a 60 FPS, costa batteria e fa "lampeggiare" la mappa).
- Cascades tiene una **cache di texture per URL**: la stessa tessera ricaricata è istantanea e non occupa
  memoria in più. Non è documentato quando la svuota → misurare la memoria durante pan lunghi (rischio §12).
- `ImageTracker` per sapere quando una tessera è pronta o fallita (la doc avverte che le immagini possono
  non caricarsi per limiti di memoria GPU).
- Durante lo zoom si mostrano le tessere del livello precedente ingrandite finché arrivano le nuove
  (niente quadrati vuoti).
- **Mappa sempre a nord in alto**: le scritte sono "cotte" nelle tessere, ruotare la mappa ruoterebbe il testo.

### 5.3 Quante tessere servono
- Area mappa ≈ 720×600 px fisici → con tessere da 512 px servono al massimo **3×3 = 9** visibili.
- Anello di precaricamento (solo a riposo e solo per tessere adiacenti a quelle visibili): fino a 5×5 = 25.
  Questo è "caricamento della vista corrente", non pre-fetch offline: si limita all'immediato intorno.
- Stima uso tipico, consultazione: apertura 9 + 10 spostamenti × ~3 + 4 zoom × ~9 ≈ **75 tessere**.
- Stima navigazione: a zoom 16 una tessera copre ~430 m (a 45° di latitudine); 25 km di percorso ≈ 60
  tessere lungo la strada × ~3 di larghezza ≈ **180 tessere per 30 minuti**.
- 100.000/mese ≈ **3.300 al giorno**: ampio margine. Con la cache il consumo reale sarà inferiore.

### 5.4 Memoria e disco
- RAM: una tessera 512×512 RGBA ≈ 1 MB decodificata → LRU di **40 tessere ≈ 40 MB** (il Q5 ha 2 GB, ma le app
  BB10 vengono chiuse se esagerano: stare sotto ~150 MB totali).
- Disco: `data/tiles/{tipo}/{z}/{x}/{y}.png` + indice (SQLite o file `.meta`) con scadenza ed ETag;
  limite configurabile (default **200 MB**), pulizia LRU all'avvio e ogni 10 minuti.
- Le tessere scadute vanno eliminate o rivalidate, non mostrate (rispetto dei termini).

### 5.5 Overlay
- **Linea del percorso**: disegnata con `QPainter` (antialiasing, bordo scuro + linea colorata) in un'immagine
  trasparente **per tessera** (512×512) solo per le tessere che il percorso attraversa → `bb::ImageData` →
  `ImageView` sopra la tessera. Così si muove insieme alla mappa senza ridisegnare a ogni frame.
  Semplificazione Douglas–Peucker della polyline in base allo zoom.
- **Pin** (risultati ricerca, destinazione): `ImageView` posizionate in assoluto, ancorate alla punta.
- **Posizione utente**: punto blu + cerchio di precisione (ImageView scalata in base ai metri/pixel) +
  freccia di direzione ruotata (`rotationZ`) da bussola (fermo) o rotta GPS (in movimento > 2 m/s).
  Punto **grigio** quando la posizione è vecchia o imprecisa (requisito di sicurezza 3).
- **Attribuzione**: logo/testo "Google Maps" in basso a sinistra, copyright dal viewport in basso a destra,
  aggiornato a fine movimento (con debounce di ~1 s e cache per zoom/area: è una chiamata in più, da non sprecare).

### 5.6 Tipi di mappa
- Stradale (PNG, migliore per il testo), satellite (JPEG, più leggero), rilievo, livello traffico
  opzionale (sessione separata con `layerTraffic`, `overlay: true`).
- **Tema scuro** con `styles` (solo roadmap) → sessione dedicata; utile di notte in navigazione.
  Cambio automatico giorno/notte con il sensore di luce BH1761 (§15), con isteresi per non alternare di continuo.
- Ogni combinazione = una sessione diversa: salvarle tutte con la loro scadenza, ricrearle quando manca
  meno di un giorno o al primo errore 401/403.

---

## 6. Posizione (GPS) su BB10

### 6.1 API scelta
- **`location_manager.h` (lm_*)** per il servizio vero: dà accesso a stationary detection, last-known,
  passive, SLP URL. Lavora con file descriptor: integrarlo nel loop Qt con un `QSocketNotifier` sui fd
  restituiti da `lm_get_fds`, oppure in un thread dedicato con `lm_wait_reply`.
- Fallback: `QGeoPositionInfoSource` se `lm_*` dà problemi (stesse proprietà via `setProperty`).
- Permesso in `bar-descriptor.xml`: `<permission>access_location_services</permission>`;
  per la navigazione con schermo spento/app ridotta: `<permission>run_when_backgrounded</permission>`.
- Controllare all'avvio che i servizi di localizzazione siano attivi nelle Impostazioni; se no, invocare
  la pagina delle impostazioni di sistema con un messaggio.

### 6.2 A-GPS dopo lo spegnimento dei server BlackBerry
- **Stato verificato sul Q5 (5 ottobre 2026)**:
  - **Qualcomm XTRA funziona**: il servizio `gps_xtra` ha aggiornato i dati oggi e ha già programmato il
    prossimo aggiornamento (~3 giorni); i server `xtrapath1.izatcloud.net` / `xtra1.gpsonextra.net`
    rispondono. Quindi le orbite previste dei satelliti ci sono già → primo fix tipicamente in decine di
    secondi all'aperto, non minuti. **Il problema A-GPS è in gran parte già risolto dal sistema.**
  - L'effemeride estesa BlackBerry (SGEE, `http://eph.blackberry.com/...`) è **morta** (nessuna risposta;
    cartella `/var/location/ee` vuota dal 2018).
  - SUPL: `supl_server_url` vuoto, client configurato `supl_version 1` e **`tls_version TLS1.0`** →
    `supl.google.com:7275` (TLS moderno) probabilmente rifiuterà; `:7276` in chiaro potrebbe funzionare.
    SUPL diventa un'ottimizzazione opzionale, non un requisito.
  - Nel location manager `STATIONARY_FILTER_ENABLED=true` (sospensione da fermi già disponibile).
  - La localizzazione è attualmente **disattivata** nelle impostazioni del telefono (`location_on:false`):
    l'app deve accorgersene e chiedere di attivarla.
- Senza assistenza, il primo fix "a freddo" può richiedere **minuti** (deve scaricare effemeridi dai
  satelliti a 50 bit/s).
- BB10 permette di indicare un server SUPL: `lm_loc_req_set_slp_url` / proprietà `slpURL`, con
  `fixType = gps_ms_based`. Google offre un server SUPL pubblico: **`supl.google.com:7275`** (TLS) /
  `:7276` (in chiaro), usato da quasi tutti gli Android.
- **Da verificare sul telefono** (Fase 0): se il motore Qualcomm accetta l'URL e quanto scende il TTFF.
  Misura: tempo al primo fix con `reset=cold` + autonomo vs `gps_ms_based` + supl.google.com.
- Privacy: la richiesta SUPL rivela a Google la cella attuale (accettabile, è lo stesso di Android).

### 6.3 Posizione rapida al chiuso (fallback rete)
- Le posizioni Wi-Fi/cella di BB10 si appoggiavano a database BlackBerry: probabilmente **non funzionano più**
  (da verificare: `fixType=wifi`/`cellsite`).
- Piano B: **Google Geolocation API** (SKU Essentials, 10.000/mese): si inviano BSSID+segnale delle reti
  Wi-Fi visibili (`wifi_get_scan_results`) e la cella (`CellularNetworkInfo`) → posizione con precisione
  20–2000 m in ~1 s. Usata solo all'avvio se non c'è un fix GPS recente, e al massimo ogni 5 minuti.
- Requisito: la scansione Wi-Fi potrebbe richiedere permessi aggiuntivi o non funzionare con Wi-Fi spento
  (da verificare).

### 6.4 Profili di posizione (il cuore del risparmio energetico)
| Stato | Sorgente | Periodo | Flag | Note |
|---|---|---|---|---|
| App aperta, mappa ferma | GNSS | 5 s | stationary detection, last-known-first | Il punto blu si aggiorna lentamente |
| Utente preme "dove sono" | GNSS (+ Geolocation se nessun fix) | 1 s per 30 s | last-known-first | Poi torna a 5 s |
| Ricerca/schede, mappa non visibile | nessuna (o passive) | — | passive | Zero costo |
| Navigazione a piedi | GNSS | 2 s | stationary detection | |
| Navigazione auto/bici | GNSS | 1 s | — | Serve precisione per le manovre |
| App ridotta (Active Frame), non in navigazione | **spento** | — | — | |
| App ridotta, in navigazione | GNSS | 1 s | background | Active Frame con prossima manovra |
| Schermo spento, non in navigazione | **spento** | — | — | |

- Fermare il GPS **subito** quando l'app va in `thumbnail`/`invisible` (segnali di `bb::Application`)
  se non si sta navigando; riaccenderlo su `fullscreen`.
- Smoothing della posizione: filtro semplice (media pesata sulla precisione) per evitare il punto che "balla";
  in navigazione, aggancio alla strada del percorso (map-matching, §7).

### 6.5 Qualità del segnale (requisito di sicurezza)
- Precisione orizzontale > 50 m, o nessun fix da più di 5 s in navigazione → banner "Segnale GPS debole",
  punto grigio, istruzioni "precise" sospese (si mostra solo "Prosegui su …").
- In galleria: dead-reckoning semplice (ultima velocità e direzione lungo il percorso) segnalato come stimato.

---

## 7. Ricerca, percorsi e navigazione

### 7.1 Ricerca (Places API New)
- **Autocomplete (New)** mentre si scrive: dopo 3 caratteri, debounce 350 ms, `locationBias` sulla mappa
  visibile, **session token** (una sessione = dai primi caratteri alla scelta del luogo).
  Con sessione conclusa da una Place Details, le richieste di autocomplete dovrebbero non essere addebitate
  separatamente (verificare la regola attuale nella documentazione di fatturazione).
- **Place Details (New)** del luogo scelto con **field mask minimo** (`id,displayName,formattedAddress,location`,
  poi `nationalPhoneNumber,regularOpeningHours,websiteUri` solo se l'utente apre la scheda completa).
- **Text Search (New)** per "Invio" senza scegliere un suggerimento (es. "farmacia"): field mask minimo;
  i campi come nome/indirizzo/posizione rientrano nello SKU Pro (5.000/mese).
- Header obbligatorio `X-Goog-FieldMask`: **mai** `*` (costa lo SKU più caro).
- Cronologia ricerche e preferiti in locale (luoghi salvati: solo `place_id` + nome scelto dall'utente;
  i termini permettono di conservare i place ID).

### 7.2 Percorsi (Routes API v2)
- `POST https://routes.googleapis.com/directions/v2:computeRoutes`, `languageCode: it-IT`, `units: METRIC`.
- Field mask: `routes.duration,routes.distanceMeters,routes.polyline.encodedPolyline,routes.legs.steps.navigationInstruction,routes.legs.steps.distanceMeters,routes.legs.steps.polyline.encodedPolyline,routes.legs.steps.startLocation`.
- `routingPreference: TRAFFIC_UNAWARE` → SKU Essentials (10.000/mese). Con traffico (`TRAFFIC_AWARE`) diventa
  Pro (5.000/mese): opzione nelle impostazioni, default disattivato.
- `computeAlternativeRoutes: true` per mostrare 2–3 alternative (stesso costo).
- Modalità: DRIVE, WALK, BICYCLE, TWO_WHEELER, TRANSIT (per i mezzi i passi includono linee e orari).
- Decodifica della polyline codificata (algoritmo standard, ~30 righe C++).

### 7.3 Navigazione
- **Map-matching**: proiezione della posizione sul segmento più vicino del percorso; avanzamento lungo la
  polyline; distanza alla prossima manovra.
- **Deviazione**: distanza dal percorso > max(30 m, 2× precisione) per 3 fix consecutivi → ricalcolo.
  Limite: massimo 1 ricalcolo ogni 20 s e 60 all'ora (protezione quota).
- **Arrivo**: entro 30 m dalla destinazione → "Sei arrivato", GPS torna al profilo normale.
- **UI di guida** (requisiti di sicurezza): istruzione grande in alto (freccia + distanza + via), mappa che
  segue la posizione, in basso ETA/distanza; niente tastiera e niente ricerca **mentre ci si muove**
  (> 10 km/h) → si sbloccano da fermi.
- **Schermo**: tenerlo acceso solo in navigazione (`ScreenIdleMode::KeepAwake` sulla finestra), ripristinare all'uscita.
- **Active Frame**: copertina con freccia + "300 m · Via Roma" aggiornata a ogni passo (economico: niente mappa).
- **Avvisi senza voce** (Fase 3): vibrazione (`bb::device::VibrationController`) a 500 m / 100 m dalla
  manovra + suono di sistema; LED (`bb::device::Led`) opzionale.
- **Voce** (Fase 5, opzionale): l'NDK non ha TTS. Opzioni:
  1. **eSpeak NG** compilato per QNX ARMv7 con il compilatore dell'NDK (C puro, senza dipendenze, ha
     l'italiano; voce robotica ma comprensibile; ~2–3 MB nel .bar). Riproduzione PCM con `QAudioOutput`
     o `bb::multimedia::MediaPlayer` su file WAV temporaneo.
  2. Frasi pre-registrate ("Tra 200 metri", "gira a destra"...) combinate: suona più naturale ma copre solo
     le manovre standard, non i nomi delle vie.
  I termini EEA non vietano il TTS dei contenuti (i termini globali sì): ricontrollare prima di attivarlo.

---

## 8. Batteria — budget e strategie

Valori indicativi da **misurare** con un log integrato (build di debug: `bb::device::BatteryInfo::level()`
ogni minuto + stato app, scritto nel file di log come in MiniBrowser).

| Componente | Peso stimato | Strategia |
|---|---|---|
| Schermo | alto (il più grande in navigazione) | tema scuro di notte, KeepAwake solo in navigazione |
| GPS attivo | medio | profili §6.4, stationary detection, spegnimento in background |
| Radio dati | medio, con "coda" di qualche secondo dopo ogni trasferimento | raggruppare le richieste (pool keep-alive, coda a priorità), cache, niente polling |
| CPU (decodifica PNG, TLS) | medio a picchi | keep-alive (meno handshake), LRU in RAM, niente ridisegni inutili |
| GPU (pan) | basso | animazioni native Cascades |

Regole fisse:
- Nessun timer periodico quando la mappa è ferma (zero risvegli inutili).
- Nessuna attività in background fuori dalla navigazione: l'app ridotta si "congela" da sola.
- Il ricalcolo del percorso non è periodico: solo su deviazione.
- Obiettivo da verificare: navigazione in auto di 1 ora ≤ 20–25 % di batteria; consultazione di 10 minuti ≤ 3 %.

---

## 9. Integrazione con BB10

- **Ricevere "apri sulla mappa"** da Contatti, Calendario, Email: registrare un `invoke-target` con lo stesso
  filtro dell'app Mappe di sistema. Il filtro esatto (azione `bb.action.OPEN`/`bb.action.NAVIGATETO`, MIME
  tipo `application/vnd.rim.map.action-v1` o simile) va **letto dal manifest sul telefono**
  (`/apps/sys.*maps*/META-INF/MANIFEST.MF`), come fatto per gli http in MiniBrowser — non fidarsi di memoria
  o di agenti esterni.
- Registrarsi anche per URI `geo:` e per i link `https://maps.google.com/…`, `https://www.google.com/maps/…`,
  `https://maps.app.goo.gl/…` (questi ultimi sono redirect: seguirli e leggere le coordinate).
  Attenzione al conflitto con MiniBrowser registrato su tutti gli http/https: il filtro più specifico deve vincere
  (verificare come BB10 sceglie tra target, altrimenti mostra il menu di scelta).
- **Condividere**: invoke `bb.action.SHARE` con testo "Nome luogo — https://maps.google.com/?q=lat,lon".
- **Active Frame** (§7.3) e **Hub** no (inutile).

---

## 10. Interfaccia e design

### 10.1 Linee guida
- Stile BB10 nativo: `NavigationPane`, `ActionBar` in basso, menu azioni (⋮), tema **scuro** di default
  (`<cascadesTheme>dark</cascadesTheme>` nel bar-descriptor).
- Schermo quadrato 720×720: la mappa occupa tutto; barra di ricerca sovrapposta in alto (altezza ~90 px);
  ActionBar con 3 azioni: **Cerca**, **Posizione**, **Livelli**; menu: Preferiti, Impostazioni, Info/Attribuzioni.
- Testo minimo 7–8 pt (≈ 30 px) in navigazione per la leggibilità a colpo d'occhio.

### 10.2 Tastiera fisica (Q5)
Sulla mappa (nessun campo di testo attivo):
| Tasto | Azione |
|---|---|
| `S` o inizio digitazione con `Spazio` | Apre la ricerca |
| `I` / `O` | Zoom in / zoom out |
| `M` | Centra sulla mia posizione |
| `L` | Cambia livello (stradale/satellite/rilievo) |
| `T` | Traffico on/off |
| `D` | Indicazioni verso il luogo selezionato |
| `F` | Preferiti |
| `Invio` | Apre la scheda del pin selezionato |
| `Backspace` | Indietro / chiude scheda |
In ricerca: digitazione diretta, `Invio` = Text Search; il Q5 non ha trackpad né frecce, quindi i
risultati si scorrono col dito. Implementazione: `shortcuts: [ Shortcut { key: "i" … } ]` sulle `Page`/`ActionItem`;
le scorciatoie si disattivano quando un campo di testo ha il focus.

### 10.3 Schermate
1. **Mappa**: barra ricerca, pulsante posizione (stato: spento / centrato / segue), attribuzione Google.
2. **Ricerca**: campo + suggerimenti (nome in grassetto, indirizzo sotto, distanza a destra), cronologia
   quando il campo è vuoto.
3. **Scheda luogo** (`Sheet` dal basso a metà altezza): nome, indirizzo, distanza, azioni: Indicazioni,
   Salva, Condividi, Chiama, Sito.
4. **Percorso**: segmented control modalità (auto/piedi/bici/mezzi), alternative con tempo e km,
   elenco passi; pulsante "Avvia".
5. **Navigazione**: banner istruzione, mappa che segue, barra ETA, pulsante "Termina".
6. **Impostazioni**: tipo mappa default, traffico nei percorsi (Pro), dimensione cache, unità, voce,
   consenso posizione, contatori uso API (oggi / questo mese per SKU), cancella cronologia.
7. **Primo avvio**: consenso posizione (requisito privacy), spiegazione batteria.

### 10.4 Pin lasciato col dito
Pressione lunga sulla mappa → pin + Reverse Geocoding (Geocoding API, 10.000/mese) per l'indirizzo.

---

## 11. Chiave API, costi e sicurezza

- Progetto Google Cloud dedicato, fatturazione con indirizzo in Italia (termini EEA).
- API abilitate: **Map Tiles API, Places API (New), Routes API, Geocoding API, Geolocation API**. Nient'altro.
- **Restrizione per API** sulla chiave (solo quelle cinque). Le restrizioni per app Android/iOS o IP non sono
  applicabili a un'app BB10 → la protezione vera sono i tetti.
- **Tetti giornalieri** nella pagina Quote di ciascuna API (proposta):
  Map Tiles 3.000/giorno · Places 150/giorno · Routes 300/giorno · Geocoding 300/giorno · Geolocation 300/giorno.
  Superato il tetto Google risponde con un errore invece di addebitare.
- **Avviso di budget** a 1 € sull'account di fatturazione.
- Nell'app: `QuotaGuard` conta le chiamate per SKU (giorno/mese, salvate in QSettings) e si ferma un po'
  prima dei tetti, con messaggio chiaro.
- La chiave nel .bar è estraibile: offuscarla (XOR + split) serve solo contro l'estrazione banale; il
  vero limite sono i tetti. Mai committare la chiave in chiaro in un repository pubblico.
- Rotazione: chiave in un file di configurazione nella cartella dati dell'app, sostituibile via SSH senza
  ricompilare.

---

## 12. Problemi previsti e mitigazioni

| # | Problema | Probabilità | Impatto | Mitigazione |
|---|---|---|---|---|
| 1 | Pan non fluido in Cascades (ricentratura visibile, decodifica lenta) | Media | Alto | Prototipo A in Fase 1 prima di tutto il resto; pool ImageView; tessere JPEG se il PNG è lento; piano C (OpenGL, §16) già progettato |
| 2 | Primo fix GPS lento | **Bassa** (XTRA funziona, verificato) | Medio | last-known-first; Geolocation API come posizione iniziale; SUPL solo come extra |
| 2b | Cache di texture Cascades che cresce durante pan lunghi | Media | Medio | Misurare con `pidin`/`showmem`; se cresce, URL limitati e passaggio a C |
| 2c | Magnetometro assente o non calibrato | Bassa | Basso | Freccia solo da rotta GPS in movimento; invito a calibrare (gesto a 8) |
| 3 | Posizione Wi-Fi/cella di BB10 morta | Alta | Medio | Geolocation API di Google |
| 4 | Keep-alive con mbedTLS più complesso del previsto | Media | Medio | Partire con 4 connessioni "close" + cache sessioni TLS (già funziona), poi ottimizzare |
| 5 | Memoria (tessere decodificate) → app chiusa dal sistema | Bassa | Alto | LRU 40 tessere, gestire `LowMemoryWarningLevel` svuotando la cache RAM |
| 6 | Orologio errato → TLS fallisce | Bassa | Alto | Messaggio specifico |
| 7 | Cambi a prezzi/termini Google | Media | Medio | Tetti giornalieri; architettura con client API separati, sostituibili |
| 8 | `max-age` delle tessere molto basso → meno risparmio dalla cache | Media | Basso | Misurare; la quota da 100k resta abbondante |
| 9 | Conflitto invoke con MiniBrowser per i link Google Maps | Media | Basso | Filtri più specifici; test sul telefono |
| 10 | Nessun TTS nativo | Certa | Medio | Vibrazione/suoni in v1, eSpeak NG dopo |
| 11 | Scritte della mappa minuscole o enormi | Bassa | Medio | Scegliere 2x vs 4x dopo prova; `highDpi` |
| 12 | Batteria in navigazione oltre l'obiettivo | Media | Medio | Log batteria, tema scuro, profili GPS, misure reali |
| 13 | Perdita della chiave API | Bassa | Basso (con tetti) | Restrizioni + tetti + rotazione via file |
| 14 | Ricerca in movimento vietata dai requisiti | — | — | Bloccata sopra 10 km/h (anche in v1) |

---

## 13. Piano di sviluppo per fasi

**Fase 0 — Verifiche sul telefono (1–2 sessioni)**
- [ ] Creazione progetto Google Cloud, chiave, restrizioni e tetti (azione dell'utente).
- [ ] Test con `curl`/Python dal PC: createSession, una tessera (misurare dimensione PNG/JPEG, `Cache-Control`),
      Autocomplete, computeRoutes.
- [ ] **App diagnostica `BerryProbe`** (Cascades minimale, scrive tutto nel file di log come MiniBrowser;
      serve un'app perché via SSH i binari compilati non si possono eseguire):
      - GL: `glGetString(GL_RENDERER/VERSION/EXTENSIONS)`, `GL_MAX_TEXTURE_SIZE`, presenza di
        `GL_OES_compressed_ETC1_RGB8_texture`, `GL_AMD_compressed_ATC_texture`, `GL_OES_rgb8_rgba8`,
        `GL_EXT_texture_format_BGRA8888`; tempo di `glTexImage2D` per 512×512 RGB565 e RGBA8888.
      - Decodifica di tessere reali scaricate: libimg (PNG, JPEG Scalado, uscita RGB565) vs `QImage`,
        tempo medio per tessera.
      - GPS: TTFF a freddo (`reset=cold`), tiepido, caldo; con XTRA (default) e con `slpURL`
        `supl.google.com:7276`; `fixType=wifi/cellsite` (rimlocp) funziona ancora?; stationary detection.
      - Sensori: letture `mag`/`compass` (azimut sensato? serve calibrazione?), luce ambiente.
      - Memoria: RAM libera all'avvio e dopo aver caricato 40 tessere in ImageView.
- [ ] Leggere il manifest dell'app Mappe di sistema per il filtro di invocazione.
- Criterio: sappiamo quale A-GPS usare e quanto pesano le tessere.

**Fase 1 — Motore mappa (il rischio più alto)**
- [ ] Progetto Cascades `BerryMaps/client` con TLS da MiniBrowser, `package.ps1` copiato e adattato.
- [ ] Sessioni Map Tiles, TileStore (RAM + disco + Cache-Control), fetch con il client esistente.
- [ ] Prototipo pan A (ScrollView) e B (touch manuale), scegliere il migliore; pinch; zoom con I/O.
- [ ] Attribuzione Google.
- Criterio: pan e zoom fluidi sul Q5, tessere corrette, cache funzionante.

**Fase 2 — Posizione**
- [ ] LocationService su `lm_*` con profili §6.4 e cambi di stato app.
- [ ] Punto blu, cerchio precisione, bussola, pulsante "dove sono"; Geolocation API come fallback.
- [ ] Log batteria nella build di debug.

**Fase 3 — Ricerca e luoghi**
- [ ] Autocomplete con session token, Details, Text Search, schede luogo, preferiti, cronologia.
- [ ] Pressione lunga + reverse geocoding. Scorciatoie da tastiera.

**Fase 4 — Percorsi e navigazione**
- [ ] computeRoutes, polyline per tessera, alternative, elenco passi.
- [ ] Navigator: map-matching, deviazioni, ricalcolo, arrivo; UI di guida; Active Frame; vibrazione;
      blocco input in movimento; avviso segnale debole.
- [ ] Pool keep-alive per le tessere (ottimizzazione batteria).

**Fase 5 — Rifinitura e extra**
- [ ] Integrazione invoke (Contatti/Calendario/link), condivisione.
- [ ] Tema scuro mappa, traffico, satellite/rilievo.
- [ ] Voce con eSpeak NG (opzionale).
- [ ] QuotaGuard con pagina contatori.

---

## 14. Da decidere / da verificare

1. Nome definitivo dell'app.
2. Tessere 2x (default proposto) o 4x.
3. Traffico nei percorsi attivo di default? (SKU Pro: 5.000/mese invece di 10.000.)
4. Voce: sì/no e quale approccio.
5. Valori reali da misurare: `max-age` tessere, dimensione media tessera, TTFF con/senza SUPL, consumo
   batteria per profilo.

---

## 15. Hardware del Q5 — misurato sul telefono (SSH, 5 ottobre 2026)

Fonti: `pidin info`, `/pps/services/hw_info/inventory`, `/usr/lib/graphics/msm8960/graphics.conf`,
`/pps/services/geolocation/settings/*`, `/dev/sensor/`. (Identificativi del telefono volutamente omessi.)

| Componente | Dato reale | Note per il progetto |
|---|---|---|
| SoC | Qualcomm **MSM8960** rev 3.2.1.1, scheda **R093** | Le schede tecniche dicono 1,2 GHz: il sistema riporta 1512 MHz |
| CPU | 2× **Krait** 1512 MHz, FPU (VFPv4 + NEON) | Due soli core: UI, render thread Cascades, rete e decodifica si contendono la CPU → max **1 thread di decodifica** + 1–2 di rete |
| RAM | 2048 MB LPDDR2 (Hynix); **~1330 MB liberi** a riposo | Budget app consigliato ≤ 150–200 MB |
| Memoria | eMMC 8 GB (Toshiba), spazio usato basso | Cache tessere 200 MB ok |
| GPU | **Adreno 225** (driver `GSLKernel-A225`, firmware `pm4/pfp_microcode_a225`), ~25 GFLOPS a 400 MHz | OpenGL ES **1.1 e 2.0**, EGL **1.4** (niente ES 3.0 nonostante gli header GLES3 nell'NDK) |
| Blitter 2D | core **C2D** (`/dev/kgsl-2D0`, `kgsl-2D1`), `blit-config = c2d` | La composizione delle finestre (Cascades + eventuale finestra GL) la fa il blitter 2D, non la GPU 3D |
| Display | **720×720 @ 60 Hz**, formati `rgb565 rgba8888 rgbx8888 nv12`, framebuffer `rgba8888` | Una finestra GL può usare **RGB565** (metà banda) o RGBX8888 |
| Accelerometro | ST **LIS3DSH** | Ha macchine a stati interne per il rilevamento movimento a basso consumo: è ciò che usa la "stationary detection" |
| Giroscopio | ST **R3GD20** (famiglia L3GD20) | Utile per la direzione stabile in combinazione con la bussola (`rotVectMag`) |
| Magnetometro | nodi `/dev/sensor/mag`, `compass`, `rotVectMag`, `rotMatrixMag` presenti | Bussola possibile (da provare: calibrazione) |
| Luce/prossimità | ROHM **BH1761** | **Tema notte automatico** della mappa con il sensore di luce (letture rare, `skipDuplicates`) |
| GNSS | provider `qct_gps` (Qualcomm), più `rimlocp` (posizione da rete RIM) | XTRA attivo e aggiornato, SGEE BlackBerry morto, SUPL non configurato (TLS 1.0, v1) |
| Tastiera | QWERTY fisica, niente trackpad | §10.2 |
| Reti | GSM/HSPA+/LTE, Wi-Fi | Geolocation API con Wi-Fi + cella come fallback |

Librerie di sistema utili trovate (telefono e NDK):
- **libimg** (`img/img.h`) con codec PNG e **JPEG Scalado** (decoder nato per le fotocamere, veloce):
  può decodificare direttamente in **RGB565** (`IMG_FMT_PKLE_RGB565`) → metà memoria e caricamento GPU più rapido.
- **libpng 1.6**, **zlib**, **SQLite 3** (indice della cache tessere), **FreeType + HarfBuzz** (testo nella
  finestra GL, se servisse), plugin Qt `qjpeg`.
- **Skia** (`libskia-qnx`, `libgrskia`) è presente ma **senza header** nell'NDK → non utilizzabile in modo
  sicuro; per le linee si usano `QPainter` (strategie A/B) o la GPU (strategia C).

---

## 16. Grafica nativa — confronto e progetto della versione OpenGL

### 16.1 Cosa dice la documentazione BlackBerry (archivio developer.blackberry.com)
- **Animazioni**: ogni animazione Cascades (anche un `ActivityIndicator`) fa ridisegnare la scena a ~60 FPS
  → niente animazioni continue; per "sto ricevendo la posizione" usare un'icona statica.
- Cascades **smette di disegnare** i nodi non visibili e l'app quando va in background, ma conviene fermare
  le animazioni a mano.
- **App ridotta ad Active Frame**: viene messa in *Stopped* appena l'utente apre un'altra app o si spegne lo
  schermo, a meno di `run_when_backgrounded` → in navigazione serve il permesso; fuori navigazione è un vantaggio.
- **Immagini**: caricamento asincrono solo con percorso assoluto `file:///`; cache di texture per URL;
  evitare immagini duplicate; nine-slice per sfondi; niente pixel trasparenti inutili.
- **QML**: compilarlo come risorsa nel binario (avvio più veloce), caricare solo la pagina che serve
  (`ComponentDefinition`/`ControlDelegate`), non bloccare mai il thread UI.
- **OpenGL ES**: double buffering sempre; un contesto EGL per thread; evitare `glFlush`/`glGet*`/`glGetError`
  a metà frame; impostare i parametri della texture **prima** di caricarla; frame rate costante; minimizzare
  cambi di stato e draw call. Su Adreno: compressione **ATC** e **3Dc** (più ETC1, da verificare con le estensioni).
- **Sensori**: `QSensor::skipDuplicates` o `sensor_set_rate()` / `sensor_set_skip_duplicates()`.

### 16.2 Le tre strategie a confronto
| | A. ScrollView + ImageView | B. Touch manuale + ImageView | C. OpenGL ES 2.0 + ForeignWindowControl |
|---|---|---|---|
| Fluidità pan | Ottima (fisica nativa) | Buona (inerzia fatta a mano) | Ottima (60 FPS, controllo totale) |
| Zoom | Pinch nativo, poi "scatto" al livello intero | Scala del contenitore | **Continuo** (zoom frazionario vero) |
| Linea percorso | QPainter per tessera (CPU) | Idem | **GPU**: triangle strip con antialias nello shader, ridisegno gratis a ogni zoom |
| Memoria tessere | RGBA 1 MB/tessera, cache texture Cascades non controllabile | Idem | **RGB565 512 KB/tessera**, LRU sotto il nostro controllo |
| Batteria | Ridisegno solo quando qualcosa si muove (gestito da Cascades) | Idem | Ridisegno **solo su richiesta** (lo decidiamo noi); a mappa ferma zero frame |
| Overlay UI | Tutto Cascades | Tutto Cascades | Cascades sopra la finestra GL (la "buca") |
| Complessità | Bassa | Media | **Alta** (EGL, ciclo di vita finestra, input inoltrato, thread GL) |
| Rischi | Ricentratura visibile; cache texture | Inerzia non naturale | Opacità non supportata sul controllo; trasformazioni Cascades sulla finestra → artefatti; perdita del contesto in background |

**Raccomandazione**: MVP con **A** (giorni, non settimane) per avere presto un'app usabile; se la prova sul
Q5 mostra scatti, ricentrature visibili o memoria che cresce, passare a **C**. La navigazione guidata
(Fase 4) beneficia molto di C (zoom continuo, linea fluida), quindi C è probabile come obiettivo finale.

### 16.3 Progetto della strategia C (OpenGL ES 2.0)
**Finestra e contesto**
- `screen_create_window_type(SCREEN_CHILD_WINDOW)` nel gruppo della finestra principale Cascades
  (`Application::instance()->mainWindow()->groupId()`), `SCREEN_PROPERTY_ZORDER` **negativo** (sotto Cascades),
  `SCREEN_USAGE_OPENGL_ES2`, formato **RGBX8888** (o RGB565 se le misure lo giustificano), **2 buffer**.
- `ForeignWindowControl` in QML a tutto schermo con `updatedProperties` per tenere sincronizzate
  posizione/dimensione; sopra, in Cascades: barra di ricerca, pulsanti, schede, attribuzione Google.
- EGL 1.4: `eglChooseConfig` (ES2, RGB888/565, senza depth), `eglSwapInterval(1)` (vsync 60 Hz).
- **Thread di rendering dedicato** (QThread) con il proprio contesto EGL; il thread UI gli manda solo
  "stato camera" (centro, zoom, rotazione) e "tessere pronte".

**Input**
- I tocchi arrivano a Cascades (la finestra GL è sotto): un `Container` trasparente sopra il
  `ForeignWindowControl` riceve `onTouch` e `PinchHandler`, calcola pan/zoom e li passa al motore.
- Inerzia: velocità dagli ultimi 100 ms di movimento, decelerazione esponenziale (attrito ~0,95 per frame),
  frame generati solo finché la velocità è > 0.

**Tessere**
- Thread di decodifica unico: libimg → buffer **RGB565** (roadmap) o JPEG Scalado → RGB565 (satellite).
- Caricamento GPU: `glTexImage2D(GL_RGB, GL_UNSIGNED_SHORT_5_6_5)`, 512×512 (potenza di 2 → mipmap possibili
  per lo zoom indietro senza sfarfallio), parametri impostati prima del caricamento. **Massimo 2–3 upload per
  frame** per non perdere fotogrammi.
- LRU GPU di ~60 tessere (≈ 30 MB in RGB565) + LRU CPU dei file compressi.
- Durante lo zoom si disegnano le tessere del livello superiore/inferiore già presenti (niente buchi).
- Un solo shader per le tessere (quad con coordinate di texture); un VBO statico per il quad unitario,
  posizione via uniform → una draw call per tessera (9–25 per frame: trascurabile).

**Linea del percorso e simboli**
- Polyline del percorso convertita in **triangle strip** con giunzioni arrotondate, in coordinate Mercator
  (VBO caricato una volta per percorso); lo shader la trasforma con la camera → nessun ricalcolo a ogni zoom.
- Antialiasing dei bordi nello shader (distanza dal centro della linea → alpha), bordo scuro + colore.
- Punto blu, cerchio di precisione e pin come sprite con texture; testo (nomi dei pin) con FreeType solo se
  necessario, altrimenti etichette in Cascades sopra la mappa.

**Ciclo di vita e batteria**
- Rendering **a richiesta**: un frame solo quando cambia qualcosa (gesto, inerzia, tessera arrivata, posizione
  nuova). A mappa ferma: zero frame, GPU spenta.
- `thumbnail` / `invisible` → fermare il thread di rendering; in navigazione con app ridotta si aggiorna solo
  l'Active Frame (testo), mai la mappa GL.
- Rilasciare le texture su `LowMemoryWarningLevel`; ricreare superficie/contesto se il sistema li invalida.

**Limiti noti di ForeignWindowControl (dagli header NDK)**
- `opacity` sul controllo o sui padri **non ha effetto** (si può solo nascondere con `visible`).
- La finestra Screen può essere solo un rettangolo allineato agli assi: niente rotazioni/scale Cascades su
  di essa (artefatti). Le trasformazioni vanno fatte dentro OpenGL.
- La finestra deve unirsi al gruppo Cascades **dopo** la creazione del controllo.

### 16.4 Testo e leggibilità a 330 ppi
- Le tessere @2x con `highDpi` hanno testo pensato per schermi ad alta densità: su 720×720 a 3,1"
  dovrebbero risultare leggibili senza ingrandimenti. Verificare con tessere reali in Fase 0 (alternativa
  4x a zoom −1, più dati).
- Dimensioni UI in unità di design Cascades (`ui.du()`), non in pixel fissi.

---

## 17. Risultati misurati con BerryProbe (5 ottobre 2026, Wi-Fi di casa)

### 17.1 Google Map Tiles API
| Misura | Valore | Conseguenza |
|---|---|---|
| Tessera stradale 2x **PNG** (512×512) | **27–39 KB** | Molto leggera: ~300 KB per una schermata piena (9 tessere) |
| Tessera stradale 2x **JPEG** | 87–101 KB | **3× più pesante del PNG**: per lo stradale si usa PNG, sempre |
| Tessera satellite JPEG (senza `scale`) | 32 KB, **256×256** | Per il satellite chiedere `scaleFactor2x` (da misurare) |
| `Cache-Control` | `private, max-age=86400, must-revalidate, no-transform` | **Cache valida 24 ore**; poi obbligo di rivalidare |
| `ETag` + `If-None-Match` | risposta **304**, 0 byte, ~360 ms | Rivalidazione economica in dati (se conta come richiesta per la quota: da verificare nella console) |
| `createSession` | 1255 ms la prima, ~330 ms le successive | Sessione da creare all'avvio e riusare (scade dopo ~2 settimane) |
| Tempo per tessera, connessione nuova + ripresa sessione TLS | **400–650 ms** | Senza keep-alive 9 tessere in fila = ~5 s: il **pool keep-alive (§4.1) è indispensabile**, insieme a 3–4 richieste parallele |
| Attribuzione (viewport) | `"copyright": "Dati mappa ©2026 …"` + `maxZoomRects` (19 sulla terraferma) | Testo da mostrare in basso a destra |

### 17.2 Decodifica (CPU, 512×512)
- Tessere Google PNG reali: **QImage 7–19 ms** (minimo 6 ms), **libimg → RGB565 8–35 ms** (minimo 8 ms).
  Molta variabilità (picchi oltre 100 ms) dovuta alla contesa con l'interfaccia e allo scaling di frequenza della CPU.
- Conversione QImage → RGB16: altri 10–18 ms → per OpenGL conviene libimg direttamente in RGB565.
- PNG "grandi" (300 KB, RGB pieno) sono 4–10× più lenti: le tessere Google sono piccole e paletted, quindi veloci.
- Stima: una schermata (9 tessere) si decodifica in **~100–200 ms** con un solo thread.

### 17.3 GPU (Adreno 225)
- OpenGL ES 2.0, `GL_MAX_TEXTURE_SIZE` 4096, ETC1/ATC/3Dc, NPOT, VAO, mapbuffer disponibili.
- Upload 512×512: **RGB565 ~4 ms**, RGBA8888 5–20 ms (con picchi fino a 80 ms sotto carico) → **RGB565 confermato**.
- `glGenerateMipmap` 7–28 ms: mipmap solo se servono davvero (zoom indietro), non per ogni tessera.

### 17.4 Cascades ImageView (strategia A)
- 40 tessere distinte caricate in **1,4–1,7 s** (~40 ms l'una, decodifica inclusa, in parallelo all'UI).
- Memoria: **+48 MB con 40 tessere a schermo** (~1,2 MB per tessera, RGBA).
- Dopo la rimozione: la cache di texture trattiene ~12 MB, poi torna alla base → **cache limitata, nessuna crescita
  incontrollata**. Il secondo caricamento degli stessi file non è molto più rapido (1,4 s contro 1,7 s): la cache non
  li tiene tutti.
- Base del processo: ~145–157 MB (include le librerie Cascades condivise); RAM libera di sistema ~1,2 GB.
- **Conclusione: la strategia A è sostenibile** (25 tessere ≈ 30 MB). Resta da verificare la fluidità del pan.

### 17.4b Sensori (bussola, magnetometro, luce)
- Backend Qt Sensors presenti e funzionanti: `bbCompass`, `bbMagnetometer`, `bbLightSensor`.
- **Magnetometro**: in condizioni normali 34–44 µT (campo terrestre in Italia ≈ 46 µT) → corretto.
  Vicino a un magnete (custodia con chiusura magnetica, altoparlante, portatile) ha letto **~550 µT**: in quel caso
  la bussola resta **bloccata** sull'ultimo valore e dichiara `calibrationLevel` 0,1.
- **Bussola**: lontano da interferenze segue la rotazione in modo regolare (86° → 262° in 7 s); il livello di
  calibrazione però oscilla tra 1,0 e 0,1 anche con campo normale.
- **Luce**: 3–406 lux; in casa con luce normale solo 13–27 lux.
- Decisioni:
  1. Direzione della freccia: **rotta GPS in movimento** (> 2 m/s), bussola solo da fermi.
  2. Se il campo magnetico totale è fuori da ~20–70 µT o `calibrationLevel` < 0,5 → freccia nascosta o grigia +
     invito a calibrare (gesto a 8); mai mostrare una direzione bloccata come valida.
  3. Tema notte automatico: **orario del tramonto (calcolato da posizione e data) + luce** come conferma,
     non la sola soglia in lux (in interni bui scatterebbe di giorno).

### 17.4c GPS (all'aperto, QtLocationSubset → `bb::qtplugins::position::GeoPositionInfoSourceBb`)
| Modalità | Risultato |
|---|---|
| **Predefinita** (nessun provider, tutti i metodi = ibrido) | **Nessun fix in 180 s** con fino a 16 satelliti visibili e 0 "in uso": il provider ibrido aspetta la rete, che non risponde più |
| **`provider=gnss`, `fixType=gps_autonomous`** | **Fix in 0,9 s**, precisione 4–10 m, 7 satelliti in uso su 21 (ricevitore già "caldo" dal test precedente + dati XTRA) |
| **Reset a freddo** + autonomo | **Nessun fix in 180 s** (12 satelliti visibili, 0 in uso): senza dati di assistenza il primo fix supera i 3 minuti |
| SUPL (prima versione) | Fix in 45 s, ma **test non valido**: la proprietà va scritta `slpUrl`, non `slpURL` (ignorata) |
| **SUPL corretto** (`slpUrl=supl.google.com:7276`, `gps_ms_based`, reset a freddo) | **Nessun fix in 180 s**, solo 2 satelliti visibili per tutto il test → **nessun beneficio osservato** (client SUPL del Q5: v1 / TLS 1.0). Cielo probabilmente più coperto che negli altri test, ma la decisione non cambia: **SUPL scartato, si usa XTRA** |
| **Cella** (`provider=network`, `cellsite`) | **Nessun fix in 180 s** |
| **Wi-Fi** (`provider=network`, `wifi`) | **Nessun fix in 180 s** |

Proprietà dichiarate dalla sorgente BB10 (dal dump): `period`, `accuracy`, `responseTime`, `canRunInBackground`,
`provider`, `fixType`, `appId`, `appPassword`, `pdeUrl`, `slpUrl`, **`stationaryDetectionEnabled`**, `reset`,
`replyErrorCode`, `replyErrStr`, `locationServicesEnabled` → la stationary detection è disponibile anche da Qt,
senza scendere all'API C `lm_*`.

Decisioni:
1. **Mai la modalità predefinita/ibrida**: sempre `provider=gnss` esplicito con `fixType=gps_autonomous`
   (l'assistenza arriva da XTRA, gestito dal sistema; SUPL non serve).
2. **Mai reset a freddo** nell'app: cancella i dati di assistenza e porta il primo fix oltre i 3 minuti.
3. La **localizzazione da rete di BB10 è morta** (cella e Wi-Fi): per la posizione rapida al chiuso serve la
   Google Geolocation API (§6.3), che passa da "ipotesi" a **necessaria**.
4. Si può usare QtLocationSubset invece di `lm_*`: espone già `stationaryDetectionEnabled`, `period`, `accuracy`.
5. Batteria durante ~12 min di test GPS all'aperto: 100% → 99%, temperatura 29 → 37 °C. Misura troppo grossolana
   per un consumo orario: servono test più lunghi.

### 17.4d Fase 1 — pool keep-alive sul campo (BerryMaps 0.1.0.4)
- 186 tessere in ~1 minuto di uso, tutte HTTP 200; 3 connessioni aperte **una volta sola** (handshake ~390 ms),
  poi riusate fino a **66 richieste** ciascuna; chiuse da sole dopo 30 s di inattività.
- Tempo per tessera: **mediana 147 ms** su connessione riusata (p90 200 ms) contro 400–650 ms con connessione
  nuova → **~4× più veloce**, ~20 tessere/s con 3 connessioni in parallelo.
- Peso medio 49,5 KB (più dei 27–39 KB misurati a Roma: zone e zoom più "densi").
- Consumo quota: ~190 tessere per un minuto di esplorazione intensa → il tetto di 2800/giorno regge ~15 minuti
  di pan continuo su zone nuove; le zone già viste nelle 24 h non costano nulla grazie alla cache.

### 17.5 Decisioni prese dai numeri
1. Stradale in **PNG**, mai JPEG; satellite in JPEG a 2x.
2. Cache su disco con scadenza 24 h + rivalidazione con ETag.
3. Pool keep-alive (§4.1) promosso a priorità della Fase 1, non ottimizzazione successiva.
4. Per la strategia C: libimg → RGB565 → `glTexImage2D` RGB565, massimo 2–3 upload per frame (~4 ms l'uno).
5. Bussola solo da fermi e con campo magnetico plausibile; tema notte da orario del tramonto + luce (§17.4b).
6. Ancora da misurare: GPS (all'aperto), consumo batteria.

---

## 18. Mappa di base OpenStreetMap (CARTO) + servizi Google (decisione del 5 ottobre 2026)

- **Mappa di base predefinita: CARTO Voyager @2x** (dati OpenStreetMap), chiave gratuita, 5 milioni di tessere/mese
  per uso non commerciale. Tessera 512×512 PNG a palette ~31 KB, `Cache-Control: public, max-age=15552000`
  (**180 giorni**), ETag. Senza chiave arrivano tessere-filigrana da 2 KB. Attribuzione obbligatoria:
  "© OpenStreetMap contributors, © CARTO".
- `tile.openstreetmap.org` scartato: solo 256 px (illeggibile o sfocato a 330 ppi), cache minima 7 giorni,
  offline vietato.
- **Google resta solo per i servizi**: ricerca (Places), coordinate (Geocoding, Geolocation), percorsi (Routes).
  **Le tessere Google sono state tolte del tutto** (BerryMaps 0.1.0.6, su richiesta): niente Map Tiles API,
  niente sessioni; la versione 0.1.0.6 cancella al primo avvio le tessere Google rimaste in cache.
- **Termini Google EEA**: l'uso dei servizi Google con mappe di terzi è permesso "a discrezione del cliente", che
  risponde di eventuali disallineamenti. Restano: niente salvataggio di nomi/indirizzi/recensioni dei luoghi,
  attribuzione "Google Maps" accanto ai contenuti Places.
- **Navigazione guidata**: con percorso Google su mappa non Google i requisiti di sicurezza EEA chiedono verifica
  dell'allineamento della linea, ricalcolo almeno ogni 15 s e validazione delle istruzioni. Senza tessere Google,
  **questi requisiti vanno implementati nella Fase 4** (map-matching della linea sulle strade visibili, ricalcolo
  periodico ≥ ogni 15 s durante la guida, controllo che le istruzioni corrispondano alla mappa) — pesa sulla quota
  Routes: ~240 ricalcoli/ora di guida, da tenere sotto i tetti giornalieri.
- Implementazione (BerryMaps 0.1.0.6): solo CARTO, cache in `tiles/carto`, pool keep-alive verso
  `basemaps.cartocdn.com`, chiave in `/accounts/1000/shared/misc/berrymaps_cartokey.txt`, attribuzione fissa.

---

## 19. Fase 2 — posizione (BerryMaps 0.1.0.7 → 0.1.0.10, confermata funzionante il 5 ottobre 2026)

- `LocationService` (QtLocationSubset): `provider=gnss`, `fixType=gps_autonomous`, 1 s mentre la mappa segue,
  5 s altrimenti; GPS acceso solo con app a schermo intero e schermo acceso (`fullscreen/thumbnail/invisible/
  asleep/awake`).
- Pulsante "dove sono" + tasto **M**: accende e segue; un trascinamento o un pinch smettono di seguire; una nuova
  pressione mentre segue (con fix già presente) spegne il GPS.
- Punto blu, cerchio di precisione, cono di direzione (rotta GPS sopra 2 m/s, bussola da fermi solo se calibrata e
  con campo 20–70 µT); punto grigio se la posizione ha più di 15 s o precisione peggiore di 50 m.
- Tessere e simboli in un unico strato spostato da pan e lancio, così il punto si muove con la mappa.
- **Lezioni dal telefono**:
  1. `stationaryDetectionEnabled` attivo dall'avvio **impediva il primo fix** a telefono fermo → ora si attiva solo
     dopo il primo fix (stop/imposta/riavvia la richiesta).
  2. Toccare il pulsante più volte durante la ricerca la riavviava → prima del fix un tocco non spegne il GPS.
  3. `replyErrorCode` è un enum `bb::location::PositionErrorCode::Type`: `QVariant::toInt()` restituisce sempre 0 →
     va letto con `value<PositionErrorCode::Type>()`, altrimenti l'avviso "fermo" non viene mai riconosciuto.
  4. Le stringhe non ASCII vanno costruite con `QString::fromUtf8` (Qt 4 legge i `const char*` come Latin-1).
- Misura: primo fix in 22,5 s, precisione 14 m (dopo i reset a freddo della Fase 0).

---

## 20. Linee guida UI di BlackBerry 10 applicate (da BlackBerry 10 UI Guidelines, archivio 2016)

Principi rilevanti per BerryMaps e come sono applicati (BerryMaps 0.1.0.11):
| Linea guida | Applicazione |
|---|---|
| "Content is king": pochi controlli, soprattutto sui telefoni con tastiera (schermo piccolo) | Mappa a tutto schermo, action bar in sovrimpressione (`ChromeVisibility.Overlay`) |
| Viste immersive: l'action bar sparisce e torna con un tocco | `TapHandler` sulla mappa alterna `Overlay` / `Hidden` |
| Gesti invece di pulsanti ("zoom allargando le dita, non con un pulsante") | Tolti i pulsanti + e −; restano pinch, doppio tocco, tasti I/O |
| Action bar: max 3 azioni, una **signature** per l'azione più frequente | Signature "Posizione" (tasto M). Con la ricerca (Fase 3) la signature diventa **Cerca** (tasto S) |
| Scorciatoie consigliate: T in cima, C componi, **S cerca, I/O zoom**; "le scorciatoie sono solo scorciatoie" | I/O/M, ognuna con alternativa a schermo |
| Menu applicazione (swipe dall'alto) per azioni globali: Aiuto a sinistra, Impostazioni a destra, max 5 | `HelpActionItem` "Info" (versione, gesti, tasti, attribuzioni); Impostazioni quando ce ne saranno |
| Menu contestuale (pressione lunga) per le azioni su un elemento | Fase 3: pressione lunga sulla mappa → "Cosa c'è qui", indicazioni, condividi |
| Errori: in linea se possibile, altrimenti toast di 3 s; suggerire la soluzione; non annunciare i successi | Errori di rete → `SystemToast` con soluzione (max 1 ogni 30 s); banner solo per problemi permanenti |
| Attività: indicatore solo oltre i 3 s di attesa, in basso, non modale | `ActivityIndicator` sopra l'action bar se le tessere visibili mancano da più di 3 s |
| Stile 10.3: base nera/bianca, piatto, un colore primario per azioni ed elementi attivi | Tema scuro, primario `#1a73e8` (`themeSupport.setPrimaryColor`), stesso blu del punto posizione |
| Unità di design: Q5 = bucket 9 (1 du = 9 px); bersagli di tocco personalizzati ≥ 101×101 px | Spaziature in `ui.du()`; icone action bar 81×81 |
| Active Frame 720×720: 310×211 px | Per ora la copertina predefinita (schermata ridotta); in navigazione: prossima manovra |
| Elenchi di risultati che crescono dal basso (raggiungibili col pollice) | Da applicare ai risultati di ricerca (Fase 3) |

---

## 21. Fase 3 — ricerca (BerryMaps 0.1.0.13, da provare sul telefono)

- `PlacesClient`: Autocomplete (New) con token di sessione, attesa 300 ms, minimo 2 caratteri, ricerca orientata
  sul centro della mappa (cerchio 50 km); la risposta superata da una query più recente viene scartata.
- Place Details (New) con field mask `id,formattedAddress,location` (**Essentials**); il nome viene dal
  suggerimento (`displayName` farebbe passare a Pro). La chiamata dettagli chiude la sessione di fatturazione.
- Invio = primo suggerimento (niente Text Search, che è Pro). Pressione lunga = Geocoding inverso.
- Limiti giornalieri nell'app: 300 autocomplete, 150 dettagli, 150 geocoding.
- Nessuno storico di nomi/indirizzi salvato (termini); attribuzione "Google Maps" su suggerimenti e scheda.
- Chiave nell'header `X-Goog-Api-Key` (Places); per Geocoding sta nell'URL ma il log TLS toglie la query.
- UI (linee guida §20): **Cerca** è la signature (tasto S), Posizione (M) sulla barra, Condividi nel menu azioni
  (link OpenStreetMap); suggerimenti che crescono dal basso verso il campo di testo; scheda del luogo sopra
  l'action bar con distanza dalla posizione attuale.

---

## 22. Fase 4 — percorsi e navigazione (BerryMaps 0.1.0.16, da provare sul telefono)

- `RouteClient`: Routes API v2 `computeRoutes` (auto con `TRAFFIC_UNAWARE` = Essentials, a piedi, bici), field mask
  minimo, istruzioni in italiano; geometria = concatenazione delle polyline dei passi (ogni passo sa dove inizia).
  Limite app 600 richieste/giorno. Verificato dal PC: Duomo → Centrale 15 min, 3,6 km, 23 passi.
- Linea del percorso disegnata con `QPainter` in immagini 512×512 allineate alle tessere (solo quelle attraversate),
  in uno strato tra mappa e punto blu; contorno scuro + centro blu (ben distinta dalla mappa).
- `Navigator`: aggancio della posizione al segmento più vicino, prossima manovra e distanza, tempo/distanza
  rimanenti e ora di arrivo; vibrazione prima della manovra (80 m auto, 25 m a piedi/bici); arrivo entro 25 m.
- Requisiti di sicurezza EEA (percorso Google su mappa non Google):
  - **ricalcolo almeno ogni 15 s** in auto quando ci si muove (> 3 m/s), più ricalcolo se fuori percorso
    (3 fix oltre max(35 m, 2× precisione)), con almeno 10 s tra due ricalcoli;
  - GPS vecchio/impreciso → banner arancione "indicazioni sospese", nessuna distanza precisa;
  - schermata di guida leggibile a colpo d'occhio, niente action bar né ricerca durante la guida;
  - **non implementato**: la verifica continua che la linea combaci con la geometria delle strade della mappa
    (requisito 3.2(a)): con tessere raster OSM non c'è una geometria stradale da confrontare. Mitigazione: ricalcolo
    ogni 15 s e aggancio al GPS. Da tenere presente.
- Navigazione: schermo sempre acceso (`ScreenIdleMode::KeepAwake`), GPS attivo anche con app ridotta
  (`run_when_backgrounded`), mappa che segue a zoom 17.
- Mancano: voce, Active Frame con la prossima manovra.

### 22.1 Mezzi pubblici (BerryMaps 0.1.0.18, da provare)
- `travelMode: TRANSIT`, field mask con `routes.legs.steps.travelMode,routes.legs.steps.transitDetails`.
  Verificato dal PC (Duomo → Bicocca): M3 Linea Gialla → Comasina 22:25 Duomo → 22:33 Zara (6 fermate), poi M5;
  colori ufficiali delle linee (`transitLine.color`), orari locali (`localizedValues`), fermate, direzione.
- Mappa: tratte sui mezzi nel colore della linea, tratti a piedi a puntini grigi.
- Riepilogo "27 min · M3 → M5 · parte alle 22:25"; Passi con orari e fermate.
- Guida: a piedi verso la fermata "Raggiungi la fermata X: M3 → Comasina alle 22:25"; a bordo "scendi a Zara (alle
  22:33)", mostrato anche senza GPS (metropolitana); **nessun ricalcolo automatico** in modalità Mezzi.
- Orari = quelli previsti dall'orario pubblicato da Google al momento della richiesta (non in tempo reale garantito).

### 22.2 Partenza e orario personalizzati (BerryMaps 0.1.0.19, da provare)
- "Da:" nel pannello del percorso: posizione GPS (default), un luogo cercato (ricerca in modalità partenza, pulsante
  "Qui" = torna al GPS) oppure un punto scelto con pressione lunga mentre il percorso è aperto (indirizzo via geocoding).
- Mezzi: Adesso / Partenza alle… / Arrivo entro… con `DateTimePicker`; inviato come `departureTime`/`arrivalTime`
  RFC 3339 UTC. Verificato dal PC: partenza 08:00 → M3 08:04, M5 08:18; arrivo entro 08:00 → M3 07:28, M5 07:43-07:49.
- L'orario non viene inviato per auto/piedi/bici (con `TRAFFIC_UNAWARE` non avrebbe effetto).
- La guida usa sempre il GPS: con una partenza diversa dalla posizione attuale, in auto/piedi/bici il primo
  ricalcolo "fuori percorso" riparte dalla posizione reale.

---

## 23. Indicazioni vocali e Active Frame (BerryMaps 0.1.0.20, da provare)

- BB10 non ha sintesi vocale: `VoiceGuide` manda il testo al server di **BerryAssistant** (`POST /v1/tts`, token
  Bearer, Piper voce italiana *it_IT-paola-medium*) e riceve Ogg/Opus (~10 KB a frase); libopus lo decodifica in WAV
  e lo riproduce il `MediaPlayer` di sistema. Misure sul server: 0,36 s alla prima sintesi, < 1 ms dalla cache
  (ultime 200 frasi), 0,2 s attraverso il tunnel Cloudflare. Server e token: i file di BerryAssistant sul telefono
  (`berryassistant_server.txt`, `berryassistant_token.txt`). Senza server la guida funziona, solo muta.
- Abbreviazioni sciolte prima della sintesi (P.za → Piazza, V.le → Viale, C.so → Corso, SS12 → Strada Statale 12…).
- Annunci (una volta per manovra, anche con i ricalcoli ogni 15 s: chiave = istruzione + distanza dalla meta):
  avvio; "Tra 500 metri, …" (auto, 200–550 m) o "Tra 150 metri, …" (piedi/bici, 60–170 m); istruzione alla manovra
  (auto 120 m, piedi/bici 30 m); mezzi: "Alla fermata X prendi Metropolitana 3 direzione Y, alle 22:25" e a bordo
  "Scendi a Z, tra N fermate"; "Ricalcolo il percorso"; "Segnale GPS debole"; "Sei arrivato a destinazione".
- Pulsante **Silenzia/Voce** nella barra della guida (ricordato tra un avvio e l'altro).
- **Active Frame** durante la guida (`SceneCover`, 310×211 sul Q5): freccia, distanza, istruzione, tempo e arrivo;
  arancione con GPS debole; fuori dalla guida torna l'anteprima normale.
- Termini Google EEA: nessun divieto di leggere ad alta voce le istruzioni (il divieto TTS è solo nei termini globali).

---

## Fonti
- Prezzi e quote: [Pricing categories](https://developers.google.com/maps/billing-and-pricing/pricing-categories),
  [SKU details](https://developers.google.com/maps/billing-and-pricing/sku-details),
  [Blog Google: fino a 10.000 chiamate gratuite](https://mapsplatform.google.com/resources/blog/start-building-today-with-up-to-10-000-monthly-free-calls-per-product/),
  [Woosmap — analisi 2026](https://www.woosmap.com/blog/google-maps-api-key-free)
- Map Tiles API: [2D Tiles overview](https://developers.google.com/maps/documentation/tile/2d-tiles-overview),
  [Session tokens](https://developers.google.com/maps/documentation/tile/session_tokens),
  [Policies](https://developers.google.com/maps/documentation/tile/policies)
- Places / Routes: [Places usage and billing](https://developers.google.com/maps/documentation/places/web-service/usage-and-billing),
  [Text Search (New)](https://developers.google.com/maps/documentation/places/web-service/text-search),
  [computeRoutes](https://developers.google.com/maps/documentation/routes/reference/rest/v2/TopLevel/computeRoutes)
- Termini: [Google Maps Platform Terms](https://cloud.google.com/maps-platform/terms),
  [EEA Terms](https://cloud.google.com/terms/maps-platform/eea),
  [Safety requirements (navigazione)](https://cloud.google.com/terms/maps-platform/eea-safety-requirements)
- BB10: [Cascades Location Diagnostics](https://blackberry.github.io/Cascades-Samples/locationdiagnostics.html),
  [ForeignWindowControl](https://developer.blackberry.com/native/reference/cascades/bb__cascades__foreignwindowcontrol.html),
  header NDK 10.3.1: `location_manager.h`, `wifi/wifi_service.h`, `bb/device/CellularNetworkInfo.hpp`,
  `bb/platform/MapInvoker.hpp`
- A-GPS: [Google SUPL server (android-platform)](https://groups.google.com/g/android-platform/c/w_VnB8suU_A),
  [SUPL e privacy (WirelessMoves)](https://blog.wirelessmoves.com/2014/08/supl-reveals-my-identity-and-location-to-google.html)
- BerryCore (valutato e scartato come dipendenza): [GitHub sw7ft/BerryCore](https://github.com/sw7ft/BerryCore)
- Hardware e grafica: [Adreno (Wikipedia)](https://en.wikipedia.org/wiki/Adreno),
  [AnandTech — Adreno 225 / MSM8960](https://www.anandtech.com/show/4940/qualcomm-new-snapdragon-s4-msm8960-krait-architecture/3),
  [Notebookcheck — Adreno 225](https://www.notebookcheck.net/Qualcomm-Adreno-225.116266.0.html),
  [libviews (Cascades + OpenGL)](https://github.com/rsperanza/libviews),
  [Cascades-Samples helloforeignwindow](https://github.com/blackberry/Cascades-Samples/blob/master/helloforeignwindow/src/helloforeignwindowapp.cpp),
  [OpenGLES-Samples BlackBerry](https://github.com/blackberry/OpenGLES-Samples/blob/master/OpenGLES2-ProgrammingGuide/Common/src/esUtil.c)
- Documentazione BlackBerry archiviata (web.archive.org, 2016): `best_practices/performance/performance.html`,
  `best_practices/performance/battery.html`, `graphics_multimedia/opengl_es/best_practices.html`
- Header NDK 10.3.1: `bb/cascades/controls/{imageview,foreignwindowcontrol,scrollview}.h`,
  `bb/cascades/resources/{image,imagetracker}.h`, `img/img.h`, `GLES2/`, `EGL/`, `screen/`
- Telefono (SSH): `pidin info`, `/pps/services/hw_info/inventory`, `/usr/lib/graphics/msm8960/graphics.conf`,
  `/pps/services/geolocation/settings/*`, `/pps/services/gps_xtra/status`, `/dev/sensor/`
