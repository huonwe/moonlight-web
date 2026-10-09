# Moonlight Web Booster — étude de faisabilité et plan

> Un petit composant natif, optionnel, installé sur la machine **cliente**, à qui
> la page MoonlightWeb pourrait confier le décodage, le traitement ou l'affichage
> de la vidéo. L'interface reste la page ; sans Booster, rien ne change.
> Étude demandée par Bruno le 09/10/2026. **Prévue après la v0.4.0** : ce
> document ne lance aucun développement, aucun POC, aucun banc.
>
> **Statut au 09/10/2026** : étude seule. Aucun code, aucun composant, aucun
> installateur. Chaque phase du §10 attend le « go » de Bruno.
>
> Légende : **[D]** fait documenté (source officielle, §14) ; **[C]** constat du
> code ou des bancs de ce dépôt (chemin ou section cités) ; **[H]** hypothèse à
> mesurer.

## 0. Verdict

**Faisable sous une seule forme, et pas celle qu'on imagine d'abord.**

- **Le Booster ne peut pas rendre ses images à la page sans les copier par la
  mémoire du CPU.** Aucune API web n'importe une surface GPU venue d'un autre
  processus : un `VideoFrame` se construit depuis une image du DOM, un autre
  `VideoFrame` ou un tampon CPU ; WebGPU n'importe que `HTMLVideoElement` et
  `VideoFrame` [D]. Le chemin « décodé par le Booster, affiché par Chrome »
  coûte donc relecture GPU → CPU, transfert local, copie dans la page, envoi
  au GPU de Chrome. Soit 4 à 10 ms et 0,7 à 1,5 Go/s de copies en 1440p120
  (§4), là où le décodeur matériel du navigateur coûte aujourd'hui 0,4 à 5 ms
  sans copie [C]. **Ce chemin est écarté d'avance.**
- **La seule forme qui peut gagner : le Booster décode *et affiche lui-même*,
  dans une fenêtre native posée sur la zone vidéo de la page** (le
  « présentateur superposé », §6). La page garde l'interface, les entrées,
  l'audio, le transport et le repli. Ce que cette forme peut reprendre, c'est le
  poste que le navigateur ajoute après le dessin : **15 à 25 ms entre le dessin
  dans le canevas et le bureau composé**, mesurés au POC Ultra [C,
  `ultra-lan-poc.md` §6.5, §6.9]. Aucun présentateur dans Chrome, ni le plein
  écran, ne les a rendus [C, §6.9].
- **Premier obstacle technique : la superposition elle-même.** Poser une
  fenêtre native, transparente aux clics et jamais active, exactement sur la
  vidéo d'une fenêtre de navigateur, sans que Chrome se croie masqué et
  ralentisse la page (§3.5), en suivant plein écran, multi-écran, DPI et
  changements de bureau. C'est faisable sous Windows, faisable avec plus de
  pièges sous macOS, faisable sous X11 et KDE Wayland. **C'est impossible sous
  GNOME Wayland** : le protocole ne laisse aucun client se placer ni se poser
  au-dessus d'un autre [D].
- **Second obstacle : la porte d'entrée.** Depuis 2026, une page publique qui
  joint `127.0.0.1` déclenche une demande d'autorisation, dans Chrome pour
  `fetch` (142) puis WebSocket et WebTransport (147), et dans Firefox (149 en
  strict, déploiement général à partir de 151) [D]. Safari bloque encore
  `http://127.0.0.1` depuis une page HTTPS selon les versions [D]. Le Booster
  coûtera donc au moins **une autorisation par origine**, ou une extension.
- **Le gain n'est pas démontré.** Le seul repère natif au même outil, Steam en
  HEVC, affiche le clic **au même moment que MoonlightWeb** (57,9-58,5 ms contre
  57,3-58,2 ms) [C, §6.5]. Seul le PyroWave de Steam fait mieux (42,6-49,2 ms),
  sans qu'on sache quelle part revient au codec et quelle part à sa
  présentation. **Le plan commence donc par une borne sans Booster (B0, §10) :
  si un client natif sobre ne bat pas MoonlightWeb d'au moins une image
  d'affichage au même outil, on arrête là.**

| Plateforme | Verdict | Pourquoi |
|---|---|---|
| Windows 10/11 | **Oui, en premier** | D3D11VA/D3D12 Video, DXGI flip + tearing, fenêtre superposée maîtrisée (§7.1) |
| macOS | **Oui, ensuite, sous réserve** | VideoToolbox + `CAMetalLayer` sans copie ; superposition fragile en plein écran natif (Spaces) ; distribution non notariée aujourd'hui (§7.2) |
| Linux X11, KDE Wayland | **Oui, en dernier** | VA-API / Vulkan Video + Vulkan ; superposition possible (X11, `wlr-layer-shell` sur KWin) (§7.3) |
| Linux GNOME Wayland | **Non** pour la superposition | xdg-shell interdit placement et empilement ; seul recours : une fenêtre Booster à soi, donc un second client (§5, D) |
| Android | **Non** depuis une page | pas de fenêtre au-dessus du navigateur sans permission d'overlay système ; la voie réaliste est l'appli native (`moonlight-web-android-tv`, en pause) (§7.4) |
| iOS / iPadOS | **Non** | aucun processus d'arrière-plan joignable, aucune superposition ; seule voie : une app native (§7.4) |

**Gain estimé** (§9) : sur HEVC/H.264, entre **0 et 2 images d'affichage**
(0 à 16 ms à 120 Hz) en médiane, à prouver par B0 et B1 ; avec PyroWave, en plus,
**~3 ms de décodage** (la file WebGPU disparaît). Hors latence, deux gains
réels mais secondaires : des codecs que le navigateur refuse (HEVC sous
Firefox, décodage matériel sous Chrome Linux, HEVC 4:4:4 10 bits) et une
présentation HDR native.

**Coût de maintenance** : élevé pour ce que ça couvre. Un binaire natif par
OS et par architecture, trois piles de décodage et trois piles de
présentation, une fenêtre superposée qui dépend du comportement de chaque
navigateur, une signature et une mise à jour par OS, et un protocole
page ↔ Booster versionné (§8, §11). C'est l'équivalent d'un petit client
natif sans interface.

**Recommandation** : ne rien engager avant B0. Si B0 passe, faire B1 (labo
superposé, Windows seul, hors produit) avant toute ligne dans le produit.

## 1. La question et le périmètre

- **Garde-fous de Bruno** : la page reste le seul client ; pas d'installation
  obligatoire ; pas d'Electron ni de runtime desktop complet sans raison
  technique solide ; le Booster est optionnel et sa panne rend la main au
  pipeline web sans rupture.
- **Hors périmètre** : l'hôte (capture, encodage, transport côté hôte),
  la v0.4.0, l'appli Android TV (en pause), l'audio. L'audio reste au
  mécanisme natif du navigateur (règle du projet).
- **Objectif mesurable** : moins de latence au photon (clic → pixel sur le
  bureau composé du client) et moins d'à-coups, pas un décodage plus court
  pris isolément.

## 2. Le pipeline client aujourd'hui (constats du code)

### 2.1 Le chemin d'une image

| Étape | Où | Fil | Constat |
|---|---|---|---|
| Transport | `webrtc-dc-udp` par défaut, puis `dc-tcp`, `media-udp/tcp`, `wss` | — | [C] `backend/src/streaming/TransportPriorities.h:15-52`, `frontend/js/ui/StreamView.js:903-916` |
| Remontage de l'image | `WebRtcDataChannel._onVideoChunk`, en-tête de 17 octets | **principal** | [C] `frontend/js/api/WebRtcDataChannel.js:1092-1553` |
| Piste RTP (POC U1.4, coupée) | `RTCRtpScriptTransform` dans un worker, « route audio » | worker | [C] `frontend/js/api/RtpVideo.js`, `rtpVideoTransformWorker.js` |
| Tri, pertes, péremption | `handleVideoFrame` | principal | [C] `StreamView.js:7044-7101` |
| Décodage | WebCodecs, `prefer-hardware` si `isConfigSupported`, `optimizeForLatency:true`, file max 8 | principal (worker optionnel, coupé par défaut) | [C] `StreamView.js:2608-3063`, `:1674`, `:600-633` |
| Rendu | Canvas2D `desynchronized` par défaut ; WebGL2 FSR1/SGSR ; WebGPU (HDR, upscalers) ; `<video>` pour les TV | principal | [C] `frontend/js/stream/renderers/createRenderer.js:43-90`, `StreamView.js:635-727` |
| Cadence | dessin au décodage (« tearing », défaut sur Chromium bureau), sinon rAF ; la plus fraîche gagne | principal | [C] `StreamView.js:4360-4580`, `BrowserDetect.js:409-432` |
| Composition | Chrome (viz) puis DWM / WindowServer / compositeur Wayland | hors page | aucune API ; **15-25 ms** mesurés [C, `ultra-lan-poc.md` §6.5] |

PyroWave (Ultra, banc seulement) : décodeur WGSL sur WebGPU vers un
`OffscreenCanvas`, puis `VideoFrame` vers le présentateur Canvas2D ; repli
WebGL2 [C, `frontend/js/stream/ultra/`].

### 2.2 Où le temps passe déjà (mesures du dépôt)

| Poste | Valeur | Source |
|---|---|---|
| Décodage HEVC matériel dans la page | 0,4-5 ms (780M : 0,7 ms médiane) | [C] `ultra-lan-poc.md` §6.6, §6.18 |
| File et dessin Canvas2D | 0,2-0,3 ms | [C] §6.18 |
| Dessin → bureau composé | **15-25 ms** (58 ms au photon contre 33-43 ms lus dans le canevas) | [C] §6.5, §6.9 |
| Présentateurs et plein écran dans Chrome | tous entre 58,8 et 65,7 ms : aucun ne rend ce poste | [C] §6.9 (présentateur réellement actif non vérifié) |
| Métronome de Chrome sur une piste vidéo RTP | 7,5-7,8 ms en moyenne | [C] §6.11 |
| WebGPU, `onSubmittedWorkDone` | 3,5-3,7 ms pour un envoi vide | [C] `click-waits.md` |
| PyroWave dans la page, 1440p, 780M | 6,1-6,4 ms envoi → fin, dont 3,15 ms de GPU | [C] §6.18 |
| PyroWave natif (référence), 1080p | RTX 0,17 ms, Arc 1,16 ms | [C] §6.13 |
| Repères natifs au même outil (RTX, 120 Hz) | Steam HEVC 57,9-58,5 ms ; Steam PyroWave 42,6-49,2 ms ; MoonlightWeb HEVC 57,3-58,2 ms | [C] §6.5 |

Lecture :
- Le décodage du navigateur n'est **pas** le goulot en HEVC/H.264 : moins de
  1 ms sur un iGPU récent. Un décodeur natif n'a presque rien à reprendre là.
- Le poste que le natif peut viser, c'est la composition (15-25 ms) et, pour
  PyroWave, la file WebGPU (~3 ms au-delà du GPU).
- Le repère Steam HEVC tempère tout : un client natif n'est pas plus rapide
  par nature. Steam a peut-être sa propre file. **Il faut une borne native
  sobre (B0).**
- Les médianes tombent sur des marches de 8,3 ms (l'écran à 120 Hz) [C, §6.5] :
  le gain se compte en images d'affichage, pas en millisecondes fines.

### 2.3 Ce que le Booster ne doit pas casser

- La chaîne de repli des transports et des codecs (`app.js:2639`,
  `_computeCodecFallbackTarget` `StreamView.js:3467`).
- Les réparations de pertes : liste des images perdues renvoyée à l'hôte
  natif, `decoderRidesOutGaps()` faux sur Apple [C, `BrowserDetect.js:220-262`].
- La remontée de profondeur de file à l'hôte (`DecodeQueueSignal`,
  `StreamView.js:3712`), le `VsyncGrid`, l'« Auto » (`CadenceStepper`) : ils
  lisent l'état du décodeur et du dessin dans la page. Avec le Booster, ces
  signaux doivent venir de lui.
- Les sondes (`mwLatency`, `FrameLog`, `ContentAgeProbe`) : elles lisent le
  canevas. Elles ne verront plus l'image affichée.
- Le flux commun des invités et les sessions partagées : rien ne change côté
  hôte tant que la page reste le seul pair (§6.4).

## 3. Ce qu'une page peut et ne peut pas faire avec un programme local

### 3.1 Joindre le Booster depuis la page

**D'où vient la page** [C] : en LAN direct, `https://<IP du LAN>[:port]` ou
`https://localhost` (certificat autosigné) ; par le rendez-vous (le défaut, LAN
compris), `https://stream.moonlightweb.top/<id>`, une origine publique servie
par un service worker [C, `bootstrap/sw.js`, `bootstrap/v1/boot.js:615`].

| Canal | Chrome / Edge | Firefox | Safari |
|---|---|---|---|
| `fetch` vers `http://127.0.0.1` depuis HTTPS | permis par le contenu mixte (le loopback est « potentiellement digne de confiance ») [D] ; **demande « réseau local » depuis Chrome 142** [D] | permis ; demande « services de l'appareil » (149 en strict, 151+ pour tous) [D] | **bloqué ou non selon la version** (régression iOS 18, bogue WebKit 279249) [D] |
| `ws://127.0.0.1` | idem ; **soumis à la demande depuis Chrome 147** [D] | idem [D] | idem fetch [D] |
| WebTransport + `serverCertificateHashes` | possible, certificat ECDSA P-256 de 14 jours au plus ; soumis à la demande (147) [D] | possible [D] | **[H]** support à vérifier |
| WebRTC vers le loopback | hors demande en 2026 [D] | **[H]** | **[H]** |
| Extension + Native Messaging | possible ; une extension par navigateur [D] | possible [D] | possible, mais par une extension d'app Xcode [D] |
| Schéma d'URL (`mwbooster://…`) | lance le programme après une confirmation du navigateur [D] | idem [D] | idem [D] |

Conséquences :
- **Une page publique ne joint plus le loopback sans autorisation.** La
  demande est par origine et mémorisée. Pour le rendez-vous, c'est une
  origine (`stream.moonlightweb.top`), donc une demande une fois. En LAN direct,
  une par IP d'hôte.
- Dans le rendez-vous, la page tourne sous un service worker. La demande
  doit partir **de la page** : une requête d'un worker n'obtient pas
  l'autorisation seule [D].
- WebRTC échappe à la demande aujourd'hui. **Ne pas bâtir dessus** : c'est
  annoncé comme provisoire et ce serait contourner l'intention de la règle.
- Les politiques d'entreprise (Chrome `LocalNetworkAccess…`, Firefox
  `LocalNetworkAccess`) peuvent pré-autoriser ou interdire [D].

### 3.2 Native Messaging : un canal de contrôle, pas de données

- JSON préfixé de sa longueur sur stdin/stdout ; **1 Mo au plus par message
  de l'hôte natif vers l'extension**, 64 Mio dans l'autre sens [D].
- La page ne parle pas au programme : page → script de contenu ou
  `externally_connectable` (Chrome) → service worker de l'extension →
  programme [D]. Trois sauts d'IPC et du JSON : trop lent pour des images
  vidéo, assez pour un jeton et un numéro de port.
- Coût : une extension publiée sur trois magasins (Chrome Web Store, AMO,
  App Store pour Safari), un manifeste d'hôte natif par navigateur et par OS
  (registre sous Windows, dossiers sous macOS/Linux) [D].
- Intérêt : pas de demande « réseau local », authentification forte (le
  programme reçoit l'origine de l'extension en argument [D]).

### 3.3 Démarrer le Booster sans service permanent

- Un schéma d'URL déclaré à l'installation (`HKCU\Software\Classes` sous
  Windows, `CFBundleURLTypes` sous macOS, `.desktop` + `x-scheme-handler` sous
  Linux) [D]. La page ouvre `mwbooster://start?…` sur un geste de l'utilisateur ;
  le navigateur demande confirmation (case « toujours autoriser » dans Chrome).
- Le Booster écoute alors sur le loopback, sert une session, et **quitte seul**
  à la fermeture du canal (délai court).
- Pas de service, pas de démarrage avec la session, pas de droits
  administrateur.

### 3.4 Ce qui n'existe pas

- **Importer une texture ou un handle GPU d'un autre processus** : aucune API
  (WebCodecs, WebGPU, WebGL) [D]. `SharedArrayBuffer` ne traverse pas les
  processus.
- **Placer une fenêtre native dans la page** : impossible. Seule une
  fenêtre *au-dessus* du navigateur est possible, gérée par le Booster.
- **Connaître la position de l'élément vidéo à l'écran** de façon exacte :
  `screenX` + rectangle de l'élément + `devicePixelRatio` ignorent la hauteur
  des barres du navigateur. Le Booster doit la trouver lui-même (§6.2).
- **Une caméra virtuelle** (Booster → `getUserMedia`) : possible, mais la
  chaîne de capture de Chrome ajoute ses copies, sa file et une demande
  « caméra ». Écartée.

### 3.5 La fenêtre superposée et l'« occlusion » de Chrome

- Sous Windows, Chrome calcule si sa fenêtre est **masquée** par d'autres ; un
  onglet masqué est traité comme en arrière-plan : **rendu arrêté, JavaScript
  ralenti** [D]. Les fenêtres transparentes ne comptent pas comme masquantes
  [D], mais le critère exact de « transparente » n'est pas documenté.
- Si la superposition couvre toute la fenêtre en plein écran et que Chrome la
  juge opaque, la page (transport, entrées, audio) serait ralentie. **Risque
  n°1 à lever en B1** [H].

## 4. Le chemin « le Booster décode, la page affiche », chiffré

Taille d'une image décodée en NV12 (1,5 octet par pixel) :

| Format | Image | 60 i/s | 120 i/s | 240 i/s |
|---|---|---|---|---|
| 1080p | 3,1 Mo | 187 Mo/s | 373 Mo/s | 746 Mo/s |
| 1440p | 5,5 Mo | 332 Mo/s | 664 Mo/s | 1,33 Go/s |
| 2160p | 12,4 Mo | 746 Mo/s | 1,49 Go/s | — |

Étapes par image, toutes **[H]** (ordres de grandeur, à ne mesurer que si
quelqu'un rouvre ce chemin) :

| Étape | Coût par image (1440p) |
|---|---|
| Copie GPU → mémoire lisible, attente de la fin du décodage | 0,5-2 ms, plus un point de synchronisation |
| Envoi local (WebSocket ou WebTransport) : noyau, processus réseau de Chrome, processus de rendu | 1-3 ms, deux à trois copies |
| `new VideoFrame(buffer)` ou `writeTexture` | 0,5-1,5 ms |
| Envoi vers le GPU de Chrome | 1-2 ms (iGPU : partage de bande mémoire avec le jeu de l'écran) |
| **Total** | **~3-8 ms**, plus 0,7-1,5 Go/s de bande mémoire |

Contre 0,4-5 ms aujourd'hui sans copie [C]. Le résultat passe ensuite par
la même composition de Chrome (15-25 ms) : **ce chemin ne peut que perdre.**
Seule exception concevable : un codec que le navigateur ne décode pas du tout
(§9.3), en basse résolution. Ce n'est pas un objectif.

## 5. Les architectures comparées

| | Ce que c'est | Gain possible | Coût | Verdict |
|---|---|---|---|---|
| **A. Web seul (aujourd'hui) / PWA** | la page, installable en PWA | — (la PWA a le même compositeur) | nul | référence ; reste le défaut |
| **B. Booster décodeur → page** | le Booster décode, renvoie des images CPU | négatif (§4) | moyen | **écarté** |
| **C. Booster présentateur superposé** | le Booster décode et affiche au-dessus de la page ; la page garde UI, entrées, audio, transport | la composition (0-2 images), PyroWave natif | élevé | **recommandé, sous les portes B0/B1** |
| C1 | … flux codé relayé par la page (loopback) | idem | le plus simple | première forme |
| C2 | … le Booster reçoit lui-même le flux de l'hôte (pair WebRTC) | + supprime la pile réseau de Chrome (métronome RTP, SCTP de Chrome) | très élevé : appairage, ICE, rendez-vous, slots hôte | plus tard, seulement si C1 gagne et que le transport domine |
| **D. Booster client natif lancé par la page** | sa propre fenêtre, ses entrées, son audio | le maximum (≈ moonlight-qt) | très élevé ; **deuxième client** | contraire aux garde-fous ; seul recours sous GNOME Wayland |
| **E. Coquille native + WebView système** | une fenêtre native qui héberge la même page dans WebView2 / WKWebView / WebKitGTK, la vidéo native dessous | comme C, sans les pièges de superposition (même fenêtre) | élevé ; WebKitGTK et WKWebView ont d'autres limites (WebCodecs, WebGPU, WebHID) | **alternative n°1** si la superposition échoue |
| **F. Electron** | Chromium + Node embarqués | aucun sur la composition (même Chromium) ; un module natif peut dessiner une surface à côté, comme E | 100+ Mo par OS, Chromium à suivre | écarté sans raison nouvelle |
| **G. Tauri** | forme outillée de E (WebView2, WKWebView, WebKitGTK) | comme E | comme E, plus Rust | variante de E, à trancher si E s'ouvre |
| **H. Service local permanent** | un démon qui écoute en continu | aucun en plus | surface d'attaque constante | écarté ; lancement à la demande (§3.3) |
| **Extension + Native Messaging** | un canal, pas une architecture | évite la demande « réseau local » | trois magasins | option pour C (§8.2) |

Pourquoi C plutôt que E : C ne change rien pour qui n'installe rien, et la
page reste servie par l'hôte ou le rendez-vous, dans le vrai Chrome (WebHID,
WebCodecs, WebGPU tels qu'on les connaît). E réintroduit une application à
distribuer, avec un moteur web qui change d'un OS à l'autre. Mais E résout
tout ce qui rend C fragile (placement, occlusion, Wayland). **Si B1 montre que
la superposition est trop fragile, E est l'alternative à évaluer, pas D.**

## 6. L'architecture recommandée (C1)

### 6.1 Schéma

```
 Hôte ──(WebRTC, inchangé)──► Page MoonlightWeb (Chrome)
                               │  UI, entrées, audio, transport, repli
                               │  ① détecte le Booster (§8)
                               │  ② relaie les images codées (unités d'accès)
                               ▼  ws://127.0.0.1:<port>  (binaire, 1 message = 1 image)
                         Booster (processus utilisateur, lancé à la demande)
                               │  ③ décode (D3D11VA / VideoToolbox / VA-API·Vulkan)
                               │  ④ présente : flip model, tearing, dernière image gagne
                               ▼
                         Fenêtre superposée, sans focus, transparente aux clics,
                         calée sur la zone vidéo de la page
                               │
                               └─► retours vers la page : image affichée (id, horodatage),
                                   profondeur de file, erreurs → signaux vers l'hôte
```

### 6.2 Ce que fait chaque côté

**La page** (changements futurs, derrière une clé cachée) :
- Garde tout le pipeline actuel, prêt. Avec le Booster, elle **cesse de
  décoder** et relaie chaque image codée remontée (`onVideo`, déjà en JS
  pour le DataChannel et la route audio [C]).
- La piste `webrtc-media` (lecture par `<video>`) n'expose pas les images
  codées : avec le Booster, elle passe par l'Encoded Transform comme U1.4 [C],
  ou le Booster reste inactif sur ce transport.
- Envoie au Booster : rectangle de la vidéo, `devicePixelRatio`, plein écran
  ou non, visibilité de l'onglet, codec et paramètres de décodage.
- Reçoit : « image N affichée à T », profondeur de file, erreur de décodage.
  Elle en tire les signaux actuels vers l'hôte (pertes, `DecodeQueueSignal`,
  demande d'IDR).
- Repli : canal fermé, erreur ou silence de 250 ms → la page reconfigure son
  décodeur, demande une IDR (déjà prévu à la reconfiguration [C,
  `StreamView.js:2938-2948`]) et reprend le dessin. Coût : une IDR et
  ~1 aller-retour [H].

**Le Booster** :
- Un exécutable, sans interface hors d'une icône facultative. C++ comme le
  backend, pour reprendre ce qui existe : parsing SPS/PPS, PyroWave de
  référence (`third_party/pyrowave`), libdatachannel pour C2 [C].
- Décode dans un processus à droits réduits (le flux vient du réseau ; le
  navigateur, lui, décode dans un processus GPU isolé).
- Trouve la fenêtre du navigateur et sa zone de contenu :
  Windows par le processus et la classe de la fenêtre de rendu ; macOS par
  `CGWindowList` (limites de fenêtre, sans le titre) ; X11 par `_NET_*`.
  **V1 : plein écran seulement**. L'écran entier est alors la zone, et le
  placement devient trivial. La fenêtre agrandie vient après, si la V1 tient.
- Présente « la dernière image gagne », au plus une image en file, et respecte
  la règle du projet : latence d'abord, cadencement en option.
- Masque sa fenêtre dès que la page le demande (menu, onglet caché,
  sortie du plein écran), et la page reprend la main.

**Entrées** : elles restent au navigateur. La superposition ne prend jamais le
focus (`WS_EX_NOACTIVATE`) et laisse passer les clics (`WS_EX_TRANSPARENT`,
`ignoresMouseEvents`, forme d'entrée vide sous X11) [D]. Le verrouillage du
pointeur et le clavier restent ceux de Chrome. Le curseur système passe
au-dessus de toute fenêtre.

**Audio** : reste dans la page. La vidéo arrivant plus tôt, la
synchronisation labiale se décale de ce gain (quelques ms) [H] : sans effet en
jeu, à surveiller en vidéo.

### 6.3 Ce que le Booster peut prendre en plus, plus tard

- **PyroWave natif** : décodeur Vulkan de référence, ou portage D3D12 du HLSL
  de l'hôte [C, `ultra-lan-poc.md` §6.14]. La file WebGPU disparaît, et la
  limite des 4:4:4/HDR du navigateur aussi.
- **HDR** : swapchain HDR10/scRGB, `CAMetalLayer` EDR, sans le tone mapping
  par WebGPU [C, `_hdrMode`].
- **Upscalers** (FSR1, NIS) en natif, sans le coût WebGPU [C, wiki §15.2].

### 6.4 C2, le Booster pair de l'hôte (option)

Le Booster rejoint la session comme second pair de l'hôte, pour la vidéo
seule, comme un invité du flux commun. C'est le seul moyen d'échapper au
métronome RTP et à la pile SCTP de Chrome. Prix : authentification
(clés MW-BIND), ICE, rendez-vous, et les invariants des sessions partagées
côté hôte (`quit()` du jumeau, take-over) [C, notes de projet]. À n'ouvrir
que si B3 montre que le transport, et non la présentation, domine.

## 7. Choix par plateforme

### 7.1 Windows (cible de la V1)

- **Décodage** : D3D11 Video (D3D11VA) pour H.264, HEVC (dont 4:4:4 et 10 bits
  si le GPU le décode), AV1 [D]. D3D12 Video possible mais plus coûteux à
  écrire, sans gain attendu sur un décodage déjà < 1 ms [C]. Media Foundation :
  à éviter (sa MFT ajoute de la file) [H].
- **Présentation** : swapchain `FLIP_DISCARD` + `ALLOW_TEARING`, objet
  d'attente (`GetFrameLatencyWaitableObject`), latence max 1 [D]. Fenêtre
  `WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
  WS_EX_TOPMOST`, composée par DirectComposition [D].
  À vérifier par PresentMon : passage en « independent flip » ou plan
  matériel (MPO) en plein écran [H]. Sinon, le gain se réduit à l'écart entre
  la composition de Chrome et celle du DWM seul.
- **Pièges** : occlusion de Chrome (§3.5) ; mise à l'échelle DPI par écran ;
  Alt+Tab et notifications par-dessus ; Edge et Chrome ont des fenêtres
  différentes ; GPU hybride (le Booster doit décoder et présenter sur le GPU
  de l'écran, jamais copier entre GPU).
- **Distribution** : installateur par utilisateur, sans droits administrateur
  (schéma d'URL en `HKCU`), signé Authenticode par la chaîne SignPath déjà en
  place pour l'hôte [C, `release.yml`]. Mise à jour : reprise de
  `SelfUpdater` [C].

### 7.2 macOS

- **Décodage** : `VTDecompressionSession` avec la propriété temps réel ;
  sorties `CVPixelBuffer` adossées à `IOSurface`, transformées en textures
  Metal sans copie par `CVMetalTextureCache` [D].
- **Présentation** : `CAMetalLayer`, `maximumDrawableCount` 2,
  `displaySyncEnabled` à faux si possible [D]. Pas de tearing en fenêtre sous
  macOS ; en plein écran, le passage direct à l'écran est décidé par le
  système [H].
- **Pièges** : le plein écran natif d'un navigateur ouvre un Space à part ; la
  superposition doit le rejoindre (`fullScreenAuxiliary`,
  `canJoinAllSpaces`) [D], et le comportement varie d'une version à l'autre
  [H]. VideoToolbox ne passe pas une perte de référence [C,
  `decoderRidesOutGaps`] : la logique de réparation de la page reste valable.
- **Distribution** : l'app de l'hôte est signée par un certificat autosigné,
  le `.pkg` n'est ni signé ni notarié [C]. Pour un composant que des joueurs
  téléchargent sur leur Mac, Gatekeeper bloquera sans notarisation :
  **compte Apple Developer (99 $/an) à décider** (§13).

### 7.3 Linux

- **Décodage** : VA-API (Intel, AMD), Vulkan Video (AMD, NVIDIA, Intel
  récents), NVDEC en option ; sortie DMA-BUF importée dans Vulkan sans
  copie [D]. Le dépôt a déjà l'expérience de Vulkan Video et de VA-API côté
  hôte [C, notes D3D12/Vulkan].
- **Présentation** : swapchain Vulkan `MAILBOX` ou `IMMEDIATE`.
- **Superposition** : X11 sûr (fenêtre `override-redirect`, forme d'entrée
  vide) ; KDE Wayland par `wlr-layer-shell` (KWin le prend en charge) [D] ;
  **GNOME Wayland impossible** (xdg-shell : ni position ni empilement) [D].
  Chrome sous Wayland natif ou XWayland change la donne [H].
- **Intérêt propre** : c'est sous Linux que Chrome décode le plus souvent en
  logiciel (VA-API inégal selon distributions et versions) [H]. Le Booster y
  gagnerait du décodage, mais c'est aussi là qu'il s'affiche le moins bien.
- **Distribution** : `.deb`/`.rpm`/AppImage et dépôts signés déjà en place [C,
  `make-packages.sh`, `make-repo.sh`]. Les navigateurs en Flatpak ou Snap ne
  lancent pas facilement un programme de l'hôte : schéma d'URL par le portail,
  Native Messaging bloqué [H].

### 7.4 Android et iOS

- **Android** : une page ne lance pas une app et ne la joint pas en local
  sans les mêmes demandes ; surtout, une app ne s'affiche au-dessus du
  navigateur qu'avec la permission d'overlay système, gérée à part, qui
  n'est pas faite pour une vidéo plein écran [D]. **Voie réaliste : l'app
  native** (Trusted Web Activity pour l'interface, ou l'appli TV existante).
  Pas de Booster.
- **iOS / iPadOS** : pas de processus en arrière-plan joignable par Safari,
  pas de superposition. **Pas de Booster.** Voie possible : une app avec
  WKWebView + couche Metal (forme E), soumise à la revue de l'App Store.
  À rouvrir seulement si E réussit sur bureau.

## 8. Sécurité et communication

### 8.1 Menaces

- **N'importe quel site** peut sonder le port du Booster ou lui parler
  (empreinte, CSRF, DNS rebinding vers `localhost`).
- **Flux hostile** : le Booster décode des octets venus du réseau, hors du bac
  à sable du navigateur ; les décodeurs matériels et leurs pilotes sont une
  surface d'attaque.
- **Détournement** : un Booster qui accepterait des commandes (lancer,
  ouvrir, écrire) serait un outil d'exécution à distance.
- **Mise à jour** : un canal de mise à jour falsifié installe du code.

### 8.2 Mécanismes

1. **Pas de service** : lancé par le schéma d'URL sur un geste de
   l'utilisateur, il quitte seul (§3.3).
2. **Loopback seulement**, `127.0.0.1` et `::1`, jamais `0.0.0.0`. Port fixe
   documenté (à choisir loin de 47984-48010 de Sunshine et des ports du
   produit), repli sur deux ports voisins.
3. **Jeton à usage unique** : la page tire 256 bits au hasard, les passe dans
   l'URL de lancement ; le premier message du canal doit les prouver
   (HMAC du défi du Booster). Un site tiers ne peut pas lancer le Booster
   sans geste ni sans confirmation du navigateur, et ne connaît pas le jeton.
4. **Contrôle d'origine** : en-tête `Origin` comparé à une liste
   (`https://stream.moonlightweb.top`, `stream.dev…`, les origines LAN que
   l'utilisateur a validées). Refus de `Host` autre que `127.0.0.1:<port>`
   (contre le DNS rebinding).
5. **Protocole minimal et fermé** : « configure », « image codée »,
   « rectangle », « montrer / cacher », « fin ». Aucune commande, aucun
   chemin de fichier, aucune URL. Taille maximale par message ; un message
   hors protocole ferme le canal.
6. **Décodage confiné** : processus de décodage à droits réduits
   (AppContainer / jeton restreint sous Windows, App Sandbox sous macOS,
   seccomp sous Linux) [H, à chiffrer en B3].
7. **Version** : poignée de main avec version du protocole ; la page refuse
   un Booster trop ancien ou trop récent et reste en web.
8. **Mise à jour** : paquet signé, vérifié avant installation, par la chaîne
   de l'hôte (`SelfUpdater`, signature déjà vérifiée en mode « bundle » sur
   macOS [C]).
9. **Option extension** (plutôt qu'une demande « réseau local ») : l'extension
   reçoit le port et le jeton par Native Messaging, la page les obtient par
   `externally_connectable` ; l'hôte natif vérifie l'identifiant de
   l'extension [D]. À choisir en B2 selon ce que les utilisateurs acceptent
   le mieux : une demande d'autorisation ou une extension.

### 8.3 Ce que l'utilisateur voit

Une première fois : « Installer le Booster » (lien depuis les réglages), puis
la confirmation du navigateur pour ouvrir le Booster, puis la demande
« réseau local ». Ensuite : rien, sauf un indicateur « Booster actif » dans
l'overlay de stats.

## 9. Gains attendus, incertitudes, régressions

### 9.1 Latence (HEVC / H.264 / AV1)

| Poste | Aujourd'hui | Avec C1 | Nature |
|---|---|---|---|
| Relais page → Booster | 0 | +0,1-0,5 ms | [H] |
| Décodage | 0,4-5 ms | ~idem | [C] puis [H] |
| Dessin → photon | 15-25 ms au-delà du canevas | 1 rafraîchissement (DWM) à ~0 (independent flip + tearing) | [H] |
| **Total, médiane** | — | **0 à −16 ms à 120 Hz** (0 à 2 images), plus à 60 Hz | **[H], à prouver par B0 puis B1** |

L'incertitude est entière : le repère Steam HEVC (≈ MoonlightWeb) dit que le
natif peut ne rien gagner. Le PyroWave de Steam dit l'inverse, sans séparer
codec et présentation. Le gain sera par marches d'une image d'affichage, et
une même configuration peut changer de marche d'une passe à l'autre [C,
§6.5] : il faut des séries longues et alternées.

### 9.2 PyroWave

- Décodage : de 6,1-6,4 ms (page, 780M, 1440p) vers ~3,2 ms (le temps GPU) ;
  sur une carte dédiée, sous la milliseconde [C → H].
- Plus le gain de présentation du §9.1.

### 9.3 Hors latence

- Codecs refusés par le navigateur : HEVC sous Firefox, décodage matériel
  sous Chrome Linux, HEVC 4:4:4 10 bits (vert sous Chrome Windows [C]).
- HDR natif, sans tone mapping dans WebGPU.
- Moins de travail sur le fil principal de la page : le décodage et le dessin
  sortent du fil principal (aujourd'hui les deux y sont [C]).

### 9.4 Risques de régression

- **Chrome se croit masqué** et ralentit la page : entrées et transport
  dégradés (§3.5).
- **Double chemin** : chaque correctif du décodage de la page (repli de codecs,
  réparations, Apple sans ride-out) doit avoir son pendant dans le Booster.
- **Sondes aveugles** : `mwLatency`, `FrameLog`, `ContentAgeProbe` lisent le
  canevas. Il faudra leur pendant natif, sinon les bancs perdent leurs
  repères.
- **Repli lent** : une panne du Booster coûte une IDR et un temps sans image.
- **Énergie** sur portable : un deuxième processus GPU actif.
- **Capture d'écran et partage** (OBS, Discord, Teams) : une fenêtre
  superposée se capture autrement que l'onglet.
- **Support** : une classe de bogues « avec Booster seulement ».

## 10. Le parcours, du premier banc à la distribution

Chaque phase s'ouvre sur le « go » de Bruno, avec l'effort conseillé (Opus 5.5).
Rien ne se fait avant la sortie de la v0.4.0.

| Phase | Ce qu'elle fait | Code produit ? | Porte | Effort |
|---|---|---|---|---|
| **B0** | Borne native sans Booster : même hôte, même clic, `click-photon` sur le bureau composé du client. Comparer MoonlightWeb (Chrome, Canvas2D, tearing) à **moonlight-qt** (D3D11VA, flip, V-Sync coupé) et à `mw-click-target` comme référence d'affichage. HEVC à 120 et 60 Hz, Windows (UM790Pro), puis Mac. | **non** | **B0** | `high` |
| **B1** | Labo de présentation superposée, outil hors produit (`tools/booster-lab`, comme `mw-click-target`) : il décode un flux enregistré ou relayé, et s'affiche au-dessus d'un Chrome en plein écran. PresentMon (mode de présentation), occlusion de Chrome (rAF, minuteries de la page), clic au photon. Windows seul. | **non** (outil) | **B1** | `xhigh` |
| **B2** | Canal et lancement : schéma d'URL, WebSocket loopback, demande « réseau local » dans Chrome, Edge, Firefox ; coût par image ; option extension. Labo seulement. | **non** (labo) | **B2** | `high` |
| **B3** | Intégration Windows derrière `mw_booster=1` : relais des images, retours vers l'hôte, repli, sondes natives ; H.264, HEVC, AV1. Plein écran seulement. | oui, caché | **B3** | `xhigh` |
| **B4** | PyroWave natif dans le Booster (si Ultra est adopté) ; HDR natif. | oui, caché | — | `high` |
| **B5** | macOS (VideoToolbox, Metal, Spaces). | oui, caché | **B5** | `xhigh` |
| **B6** | Linux X11 et KDE Wayland ; GNOME Wayland déclaré non pris en charge. | oui, caché | — | `high` |
| **B7** | Distribution : installateurs par utilisateur, signature (Authenticode, notarisation si décidée), mise à jour, page « Installer le Booster », textes i18n. | oui | **B7** | `high` |
| **B8** | Fenêtre agrandie (hors plein écran), si la V1 tient. | oui | — | `high` |
| **B9** | (option) C2 : le Booster pair de l'hôte. | oui | **B9** | `max` |

### 10.1 Mesures

- Clic → photon au bureau composé du client (`click-photon.ps1`, déjà utilisé
  pour Steam [C]), 60 clics par case au moins, en alternance ABAB, médiane et
  p90, et l'écran du client nommé avec sa fréquence.
- Âge de l'hôte à l'affichage (le Booster horodate sa présentation sur
  l'horloge partagée, comme `FrameLog`).
- Images répétées et perdues ; à-coups au défilement.
- PresentMon : mode de présentation de la superposition et de Chrome.
- Page : cadence de rAF et des minuteries pendant la superposition
  (contrôle de l'occlusion).
- CPU, GPU, énergie (portable) avec et sans Booster.
- Repli : temps sans image après la mort forcée du Booster.

### 10.2 Portes et critères d'abandon

- **B0 — abandon** si moonlight-qt n'affiche pas le clic **au moins une image
  d'affichage plus tôt** que MoonlightWeb en médiane (≥ 4 ms à 120 Hz,
  ≥ 8 ms à 60 Hz) sans p90 plus mauvais, sur Windows. Le gain maximal du
  Booster serait alors nul, et seul l'intérêt « codecs » (§9.3) resterait :
  à rejuger à part.
- **B1 — abandon de C** si la superposition (a) ne gagne pas au moins 75 % de
  l'écart B0, ou (b) fait ralentir la page (rAF ou minuteries < 95 % de leur
  cadence), ou (c) ne tient pas en plein écran sur deux GPU (NVIDIA, AMD).
  Bascule vers l'étude de E, sur décision de Bruno.
- **B2 — abandon du loopback** si le canal coûte plus de 0,5 ms au p99 par
  image, ou si la demande « réseau local » ne peut pas être obtenue proprement
  depuis la page du rendez-vous. Alors extension, ou arrêt.
- **B3 — pas d'activation** si un seul repli (codec, transport, pertes) régresse
  dans la suite de bancs existante, ou si le repli prend plus de 500 ms.
- **B5 — macOS hors périmètre** si la superposition ne suit pas le Space du
  plein écran sur deux versions de macOS.
- **B7 — pas de distribution** sans signature reconnue par l'OS (sinon,
  l'installation demande de contourner un avertissement de sécurité).

## 11. Coûts de développement et de maintenance

| Poste | Windows | macOS | Linux |
|---|---|---|---|
| Décodage natif | moyen (D3D11VA, connu par moonlight-qt) | moyen (VideoToolbox) | élevé (VA-API + Vulkan Video + pilotes variés) |
| Présentation et superposition | moyen, risque occlusion | élevé (Spaces, versions) | élevé (X11 / KDE / non GNOME) |
| Distribution | faible (Inno, SignPath en place) | moyen à élevé (notarisation à décider) | moyen (paquets en place ; Flatpak/Snap des navigateurs) |
| Maintenance continue | suivre Chrome, Edge, Firefox (occlusion, demande réseau local) et les pilotes | suivre macOS chaque année | suivre Mesa, compositeurs |

Au total, un second pipeline de décodage et de présentation à garder au niveau
du premier, par OS. Le coût récurrent pèse plus que l'écriture : chaque
version de navigateur peut casser la superposition ou le canal.

## 12. Ce qui est établi, ce qui est supposé

- **Établi** [D] : pas d'import de surface GPU inter-processus dans une page ;
  demande « réseau local » de Chrome (142, 147) et Firefox (149/151) ; limites
  de Native Messaging ; occlusion de Chrome sous Windows ; xdg-shell sans
  placement.
- **Établi** [C] : décodage du navigateur < 1 ms sur iGPU récent ; 15-25 ms
  entre canevas et bureau composé ; MoonlightWeb ≈ Steam HEVC au photon ;
  file WebGPU ~3 ms ; décodage et dessin sur le fil principal.
- **Supposé** [H] : que l'independent flip s'obtient sous une superposition ;
  que Chrome ne se juge pas masqué ; le coût du relais loopback ; le gain
  final ; le comportement des Spaces ; le support de WebTransport loopback sur
  Safari ; l'effet de Chrome Wayland.

## 13. Décisions pour Bruno

1. **Ouvrir B0** après la v0.4.0 (aucun code produit, un banc UM790Pro + hôte).
2. Si B0 passe : **B1 sur Windows seul**, outil hors produit.
3. **Canal** (B2) : demande « réseau local » de la page, ou extension par
   navigateur, ou les deux.
4. **Port fixe** du Booster.
5. **Langage** : C++ (reprise du backend) ; Rust seulement si E/Tauri s'ouvre.
6. **macOS** : compte Apple Developer et notarisation, ou macOS hors V1.
7. **Linux** : GNOME Wayland déclaré non pris en charge.
8. **Périmètre V1** : plein écran seulement ; PyroWave et HDR natifs dans la
   V1 ou après.
9. **Nom** définitif du composant.
10. Si C échoue en B1 : étudier **E** (coquille native + WebView), ou arrêter.

## 14. Sources

Légende : ✓ = page ouverte et lue le 09/10/2026 ; ◦ = lue par extraits de
recherche le 09/10/2026 ; sans marque = référence officielle à relire en
B0-B2 avant de s'appuyer dessus.

Navigateurs, réseau local, contenu mixte
- ✓ Chrome, Local Network Access : https://developer.chrome.com/blog/local-network-access
- Spécification LNA (WICG) : https://wicg.github.io/local-network-access/
- ◦ Chrome, extension aux WebSockets (intention de livrer) : https://groups.google.com/a/chromium.org/g/blink-dev/c/O6GMKt44Ups
- Fiche Chrome Platform Status : https://chromestatus.com/feature/5152728072060928
- ◦ Firefox, autorisations réseau local : https://support.mozilla.org/en-US/kb/control-personal-device-local-network-permissions-firefox
- ◦ Firefox, politique `LocalNetworkAccess` : https://mozilla.github.io/enterprise-admin-reference/reference/policies/localnetworkaccess/
- ◦ WebKit, loopback et contenu mixte : https://bugs.webkit.org/show_bug.cgi?id=171934
- ◦ WebKit, régression iOS 18 : https://bugs.webkit.org/show_bug.cgi?id=279249
- Contenu mixte (W3C) : https://w3c.github.io/webappsec-mixed-content/
- Origines dignes de confiance : https://w3c.github.io/webappsec-secure-contexts/#is-origin-trustworthy
- WebTransport, `serverCertificateHashes` : https://www.w3.org/TR/webtransport/#dom-webtransportoptions-servercertificatehashes
- `registerProtocolHandler` : https://developer.mozilla.org/en-US/docs/Web/API/Navigator/registerProtocolHandler
- Schémas d'URL sous Windows : https://learn.microsoft.com/en-us/previous-versions/windows/internet-explorer/ie-developer/platform-apis/aa767914(v=vs.85)
- ◦ Chromium, occlusion des fenêtres sous Windows : https://chromium.googlesource.com/chromium/src/+/master/docs/windows_native_window_occlusion_tracking.md

Extensions et Native Messaging
- ✓ Chrome, Native Messaging : https://developer.chrome.com/docs/extensions/develop/concepts/native-messaging
- ◦ Chrome, `externally_connectable` : https://developer.chrome.com/docs/extensions/reference/manifest/externally-connectable
- Firefox, Native Messaging : https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/Native_messaging
- Safari, messages vers l'app native : https://developer.apple.com/documentation/safariservices/messaging-a-web-extension-s-native-app

API web média
- WebCodecs (constructeurs de `VideoFrame`) : https://www.w3.org/TR/webcodecs/#videoframe-constructors
- WebGPU, `importExternalTexture` : https://www.w3.org/TR/webgpu/#dom-gpudevice-importexternaltexture
- WebRTC Encoded Transform : https://w3c.github.io/webrtc-encoded-transform/

Windows
- Modèle flip DXGI : https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-flip-model
- Tearing et écrans à fréquence variable : https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays
- Objet d'attente de la swapchain : https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject
- API vidéo Direct3D 11 : https://learn.microsoft.com/en-us/windows/win32/medfound/direct3d-11-video-apis
- Direct3D 12 Video : https://learn.microsoft.com/en-us/windows/win32/medfound/direct3d-12-video-overview
- DirectComposition : https://learn.microsoft.com/en-us/windows/win32/directcomp/directcomposition-portal
- Styles étendus de fenêtre : https://learn.microsoft.com/en-us/windows/win32/winmsg/extended-window-styles

macOS
- `VTDecompressionSession` : https://developer.apple.com/documentation/videotoolbox/vtdecompressionsession
- `CVMetalTextureCache` : https://developer.apple.com/documentation/corevideo/cvmetaltexturecache
- `IOSurface` : https://developer.apple.com/documentation/iosurface
- `CAMetalLayer` : https://developer.apple.com/documentation/quartzcore/cametallayer
- `NSWindow.CollectionBehavior` : https://developer.apple.com/documentation/appkit/nswindow/collectionbehavior-swift.struct
- Notarisation : https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution

Linux
- VA-API : https://intel.github.io/libva/
- Vulkan Video, décodage : https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_video_decode_queue.html
- DMA-BUF : https://docs.kernel.org/driver-api/dma-buf.html
- xdg-shell : https://wayland.app/protocols/xdg-shell
- wlr-layer-shell : https://wayland.app/protocols/wlr-layer-shell-unstable-v1

Mobile et coquilles
- Trusted Web Activity : https://developer.android.com/develop/ui/views/layout/webapps/trusted-web-activities
- Android, permission d'overlay : https://developer.android.com/reference/android/Manifest.permission#SYSTEM_ALERT_WINDOW
- Electron : https://www.electronjs.org/docs/latest/
- Tauri, WebView par OS : https://v2.tauri.app/reference/webview-versions/
- moonlight-qt (borne B0) : https://github.com/moonlight-stream/moonlight-qt

Dans le dépôt : `docs/design/ultra-lan-poc.md` (§6.5, §6.9, §6.11, §6.13,
§6.17-6.18), `docs/design/click-waits.md`, `docs/design/glass-to-glass.md`,
`docs/wiki/05-Streaming-and-Transports.md`, `docs/wiki/09-Installers-and-Packaging.md`,
`frontend/js/ui/StreamView.js`, `frontend/js/api/WebRtcDataChannel.js`,
`frontend/js/stream/renderers/`, `frontend/js/stream/ultra/`.

## 15. Concrètement, pour l'utilisateur

Rien ne change aujourd'hui : c'est une étude, et MoonlightWeb reste une page
qu'on ouvre sans rien installer.

Si le projet aboutit, un joueur sur PC (puis sur Mac, et sous Linux hors
GNOME) pourra installer un petit programme facultatif. La première fois, le
navigateur lui demandera d'ouvrir ce programme et de laisser la page joindre
« les appareils du réseau local ». Ensuite, en plein écran, l'image ne passera
plus par l'affichage du navigateur : c'est le programme qui la montrera,
pendant que la page garde les menus, le son, la souris, le clavier et la
manette. Le joueur pourrait voir son geste à l'écran une ou deux images plus
tôt : un peu plus de réactivité en jeu rapide, rien de visible sur un bureau
ou une vidéo. Si le programme plante ou manque, l'image revient dans la page
après un court noir.

Ce que le projet ne règle pas : le Wi-Fi, l'encodage sur l'hôte, les
téléphones, les tablettes et les TV, et les bureaux Linux GNOME. Et il peut
très bien s'arrêter à sa première mesure, si un client natif ne fait pas
mieux que la page.
