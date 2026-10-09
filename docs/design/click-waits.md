# Les deux attentes du clic hors codec

> Où passe le temps d'un clic sur l'hôte natif, entre sa réception et la remise
> de l'image qui le montre (poste A), et dans la page, entre la soumission du
> décodage PyroWave et sa fin (poste B). Plan « attente », lancé le 08/10/2026.
> La cible est l'hôte natif : Windows d'abord, puis Linux et macOS. Ce document
> garde les constats ; le plan détaillé vit hors du dépôt.

## 1. Le point de départ

Banc du 06/10 (POC Ultra, §6.19-6.24 de `ultra-lan-poc.md`) : hôte DualRTX, la
RTX encode, écran virtuel à 120 Hz, client UM790Pro en câble, 456 clics. Entre
la réception du clic par l'hôte et l'image du drapeau capturée : 15,2 ms en
HEVC, 13,0 ms en PyroWave. Côté page, le décodage WebGPU de PyroWave prenait
6,3 ms, dont ~1,8 ms de travail GPU.

Le modèle d'alors pour le poste A : une demi-image d'attente du rafraîchissement
suivant (4,2 ms à 120 Hz), plus une image entre la composition de DWM et la
remise par la capture (8,3 ms). **Le banc 1 l'infirme pour la seconde moitié.**

## 2. L'instrumentation

Tout est sur l'horloge de l'hôte (QueryPerformanceCounter, commune au serveur,
au worker et au drapeau), et tout reste éteint hors banc.

- `clicktrace=1` (`MW_NATIVE_TUNING`) : chaque appui remis à SendInput (début
  et fin de l'appel), chaque réveil de la capture (début d'attente, retour,
  statut ; pour une image, `LastPresentTime` de DDA ou `SystemRelativeTime` brut
  de WGC, `LastMouseUpdateTime`, `AccumulatedFrames`), et le minutage de DWM.
  CSV `click-trace-<pid>-<ms>.csv` à côté du journal du worker. Le relais
  journalise chaque entrée horodatée (reçue, traitée).
- `MW_LATENCY_FLAG_TRACE=1` : la ligne du drapeau porte l'instant de son
  crochet, puis DwmFlush et le minutage de DWM après chaque drapeau.
  `MW_LATENCY_FLAG_SKIP=*` : le drapeau reste armé mais ne se dessine nulle part.
- `tools/click-target` (`mw-click-target`) : le « jeu idéal ». Une fenêtre D3D12
  en flip model, tearing permis, qui dessine le drapeau au clic et présente
  aussitôt.
- Page : `localStorage.mw_ultra_trace=1` garde la chronologie de chaque image
  PyroWave, les horodatages GPU de chaque image, et une passe vide de référence
  une image sur huit.
- Analyse : `scripts/bench/clickpath/hostpath.py` (poste A),
  `gpuwait.py` (poste B). Mode d'emploi dans leur README.

Les passes témoin sans aucune trace donnent les mêmes clics (32,2 et 32,1 ms de
médiane, contre 31,4 et 32,6 tracées) : la trace ne coûte rien de mesurable.

## 3. Banc 1 (08/10/2026)

Hôte DualRTX en `--dev`, la RTX encode, écran virtuel en 1920×1080 à 120 Hz
(le 06/10 était en 2560×1440), client UM790Pro sous Windows en câble, Chrome
avec `--enable-webgpu-developer-features`. 60 clics par passe, cases alternées.

### 3.1 Poste A, avec le drapeau du banc

Médianes sur 120 clics par case, de la réception par le relais à la remise de
l'image qui montre le drapeau :

| Étape | DDA, PyroWave | DDA, HEVC | WGC, HEVC |
|---|---|---|---|
| relais → SendInput appelé | 0,12 | 0,13 | 0,15 |
| SendInput appelé → crochet du drapeau | 4,20 | 4,39 | 4,10 |
| crochet → drapeau peint | 5,58 | 5,25 | 7,06 |
| drapeau peint → image présentée | 3,26 | 3,42 | 9,92 |
| présentée → remise par la capture | 0,07 | 0,10 | 0,00 * |
| **total hôte** (médiane / moyenne) | **14,5 / 16,1** | **14,7 / 17,2** | **22,4 / 25,7** |

\* WGC : l'heure de présentation est ramenée à celle de la remise (elle court en
avance), la remise ne se mesure donc pas.

Ce que ça dit :

- **Pas d'image en plus entre DWM et la capture.** L'image du drapeau est
  présentée au premier rafraîchissement de l'écran virtuel qui suit sa peinture
  (`late` = 0 en médiane), et DDA la remet en 0,1 ms. La demi-image d'attente
  du modèle est bien là (3,3-3,4 ms de médiane, 4,2 de moyenne).
- **Le drapeau lui-même coûte ~10 ms.** SendInput ne rend la main qu'une fois
  tous les crochets souris bas niveau passés, celui du drapeau compris (4,2-4,4 ms
  avant même qu'il s'exécute) ; puis montrer sa fenêtre topmost et layered prend
  5,3-5,6 ms (une moyenne de 6,5-8 ms, une queue à 10-12). C'est un artefact du
  banc.
- **WGC est nettement plus lent.** Sur l'écran virtuel, la capture n'a livré
  qu'environ 46 images par seconde (5 060 en ~110 s, contre ~115 pour DDA),
  l'image du drapeau arrive deux rafraîchissements après le suivant, et le clic
  passe de 32 à 46 ms. La cause n'est pas vérifiée. PyroWave refuse WGC : sa
  route D3D12 exige DDA.
- **Le minutage de DWM ne décrit pas l'écran capturé.** `DwmGetCompositionTimingInfo`
  donnait une période de 6,95 ms (144 Hz, l'horloge d'un autre écran) alors que
  les présentations de l'écran virtuel tombaient sur une grille de 8,33 ms.
  `hostpath.py` tire la grille des présentations elles-mêmes.

### 3.2 Poste A, avec le « jeu idéal » (premier essai de A1)

`mw-click-target` en plein écran sur l'écran virtuel, tearing permis, 240 i/s,
le drapeau de l'hôte éteint (`MW_LATENCY_FLAG_SKIP=*`), HEVC, DDA, 60 clics :

| Étape | Médiane | Moyenne | p90 |
|---|---|---|---|
| relais → SendInput appelé | 0,17 | 0,21 | 0,44 |
| SendInput appelé → WM_LBUTTONDOWN | 0,92 | 1,86 | 4,55 |
| WM_LBUTTONDOWN → Present rendu | 0,34 | 0,44 | 0,88 |
| Present → image présentée (capture) | 5,48 | 5,67 | 12,04 |
| présentée → remise | 0,11 | 0,15 | 0,25 |
| **total hôte** | **7,25** | **8,38** | 14,96 |

Clic total : **27,2 ms de médiane** (32 avec le drapeau). Avec une application
qui réagit vite, la part de l'hôte tombe à ~7 ms, presque toute entre le Present
de l'application et la présentation que DDA voit.

Les passes en fenêtre (`--window`) et synchronisée (`--sync 1`) du même essai
n'ont reçu aucun clic : 60 appuis injectés, aucun `WM_LBUTTONDOWN`. La cause
est au §3.3.

### 3.3 Poste A, A1 : fenêtre, plein écran, synchronisé

Même banc le soir (19h20-19h35), `mw-click-target` sur l'écran virtuel, HEVC,
DDA, 60 clics par passe, toutes complètes. Médianes en ms (elles ne
s'additionnent pas exactement) :

| Cas | Clic total | Hôte | Relais → vu par l'appli | Appli → Present rendu | Present → remise par DDA |
|---|---|---|---|---|---|
| plein écran, tearing | 26,9 | 8,3 | 1,2 | 0,35 | 5,8 |
| fenêtre, tearing | 27,4 | 6,6 | 1,1 | 0,35 | 5,1 |
| plein écran, synchronisé, entrée lue avant l'attente | 42,5 | 20,4 | 6,3 | 6,8 | 7,3 |
| fenêtre, synchronisé, entrée lue avant l'attente | 42,1 | 17,4 | 5,2 | 6,9 | 6,9 |
| plein écran, synchronisé, entrée lue après | 32,9 | 15,5 | 4,6 | 0,5 | 7,2 |
| fenêtre, synchronisé, entrée lue après | 33,8 | 15,7 | 5,4 | 0,7 | 7,1 |

Ce que ça dit :

- **Les clics perdus venaient du curseur de l'hôte**, pas de la fenêtre. La
  sonde clique là où il se trouve, sans le bouger, et il était sur un autre
  écran. Les pixels lus au drapeau étaient bien le fond de l'outil. L'outil
  place maintenant le curseur sur sa fenêtre et l'y ramène. Le revers vaut
  pour toutes les passes au drapeau de l'hôte : leurs clics tombent là où est
  le curseur, donc dans les fenêtres d'un écran physique si quelqu'un y
  travaille. Aucun banc ne gare encore le curseur sur l'écran virtuel.
- **Plein écran ou fenêtre, même chemin** : 27 ms dans les deux cas, 5,1 à
  5,8 ms entre le Present et la remise. L'écran virtuel (IddCx) n'a pas de flip
  indépendant : DWM compose tout, et le plein écran n'y gagne rien.
- **DWM compose l'écran virtuel sur l'horloge d'un autre écran.** L'écran
  virtuel annonce 120 Hz, et une appli synchronisée y tourne bien à 120 i/s.
  Mais les présentations que voit DDA sont espacées de 6,95-7,0 ms, et de
  14 ms quand l'appli est à 120 i/s : c'est la grille de DISPLAY9, à 144 Hz.
  Avec une appli à 240 i/s, DDA voit ~139 présentations par seconde, que le
  flux à 120 i/s ne porte pas toutes. Les passes au drapeau, sur un bureau
  presque immobile, gardaient une grille à 120 Hz. Reste à voir ce que fait
  DWM quand l'écran virtuel est seul, ou plus rapide que les écrans physiques.
- **Synchronisé, le clic coûte 6 ms de plus, ou 15 si l'appli lit son entrée
  avant d'attendre.** Une appli synchronisée ne lit son entrée qu'une fois
  par image : le clic attend 4,4-5,2 ms avant d'être vu. Son image attend
  ensuite le rafraîchissement, et DDA la remet 7 ms après le Present, avec une
  composition sans elle entre les deux. Une boucle simple, qui lit l'entrée
  avant d'attendre le swap chain, ajoute une image (`--input-first`). C'est le
  jeu qui en décide, pas l'hôte.
- `GetFrameStatistics` ne donne toujours rien sur l'écran virtuel
  (`displayedUs` nul dans les six passes).

### 3.4 Poste B, l'attente du GPU dans la page (780M, PyroWave)

Deux passes, ~12 200 images chacune, horodatages GPU non arrondis :

| Mesure | Médiane |
|---|---|
| soumission → fin du travail (`onSubmittedWorkDone`) | 5,0-5,2 ms |
| travail GPU : décodage 1,77 + écart 0,13 + affichage 0,31 | 2,2 ms |
| soumission → début du GPU | 0,7-1,4 ms |
| fin du GPU → rappel de Chrome | 1,1-1,8 ms |
| **soumission vide** (aucun travail) → fin | **3,5-3,7 ms** |
| fin → VideoFrame, VideoFrame → dessinée | 0,1, puis 0,3 ms |

L'horloge du GPU du 780M court à +1 935 ppm de celle de la page (16 ms en 8 s) :
`gpuwait.py` retire cette pente avant de recaler, et les bornes du recalage
tiennent alors à 0,6 ms près.

**Le poste B est l'aller-retour de Chrome et de Dawn, pas le GPU** : une
soumission vide coûte déjà 3,6 ms. Le travail du GPU en ajoute ~1,5.

### 3.5 Un bug trouvé en route

Le relais répondait « injection traitée » 40 µs après la réception, avant même
d'injecter. Il construisait sa réponse à partir d'un objet temporaire, dont le
destructeur partait aussitôt. Le `hostInMs` de la sonde lisait donc ~0 ms au
lieu des 5 ms de SendInput. Corrigé (`afb0c445`). Le chiffre « hôte → capturée »
du 06/10 n'en dépendait pas.

## 4. Banc 2 (09/10/2026) : l'écran virtuel à 240 Hz, et un vrai jeu

Même banc : DualRTX en `--dev`, la RTX encode, client UM790Pro en câble, flux
HEVC à 120 i/s, DDA. L'écran virtuel en 1920×1080, à 120 ou 240 Hz
(`MW_VDD_REFRESH`). `presentmon.py` lit PresentMon à côté de chaque passe.

240 Hz est la fréquence du produit sous Windows depuis le 30/09 (`03c189ea`,
décision A du plan framerate-hote) ; les 120 Hz des bancs 1 et A1 étaient
forcés. Le flux, lui, restait fixé à 120 i/s, détection de l'« Auto » coupée
(`mw_autostep=0`), alors que le produit l'allume par défaut (§4.1).

### 4.1 AW2.1 : l'écran virtuel à 240 Hz

`mw-click-target` en fenêtre, 120 clics par case (deux passes, ordre ABBA) :

| Cas | Clic p50 | Clic p90 | Clic moyen | Hôte p50 | Hôte p90 | Hôte moyen |
|---|---|---|---|---|---|---|
| tearing, 120 Hz | 27,5 | 33,3 | 27,5 | 6,8 | 16,3 | 8,2 |
| tearing, 240 Hz | 27,4 | 31,6 | 26,7 | 7,7 | 11,4 | 7,6 |
| synchronisé, 120 Hz | 32,5 | 42,7 | 34,6 | 13,5 | 21,5 | 14,0 |
| synchronisé, 240 Hz | 29,2 | 32,5 | 29,2 | 10,1 | 16,0 | 10,7 |

- **DWM compose l'écran virtuel au rythme de l'écran le plus rapide.** À
  240 Hz, DDA voit les présentations sur une grille à 240,000 Hz. À 120 Hz,
  elles suivaient la grille de DISPLAY9, à 144 Hz (§3.3).
- **En tearing, rien ne change en médiane.** La composition vient plus tôt :
  le Present précède la composition suivante de 1,0 ms au lieu de 3,4 (p50,
  sur les images que PresentMon voit). Mais un flux à 120 i/s n'encode que
  la première présentation de chaque tranche de 8,3 ms. À 240 Hz, une
  composition sur deux n'est pas transportée, et le drapeau attend souvent la
  suivante (`late=1`, `between=1`). Il ne reste qu'un gain en queue : l'hôte
  passe de 16,3 à 11,4 ms au p90, et le clic gagne 0,8 ms en moyenne.
- **Ce frein vient du flux fixé par le banc.** La porte (`FrameCadence`),
  rejouée sur les captures de chaque passe (mêmes comptes que son journal),
  écarte 49 % des présentations de l'outil à 240 Hz, soit 2,1 ms de plus en
  moyenne pour un instant quelconque ; 15 % et 1,0 ms à 120 Hz (grille de
  144). Sur un client qui déchire, le produit monte le flux à 240 i/s dès que
  le contenu va plus vite (« Auto » détecté, Phase UA du POC Ultra, par défaut
  depuis le 02/10) : la porte devient un plafond et n'écarte plus rien. Le
  clic avec la détection allumée reste à mesurer (§5).
- **En synchronisé, 240 Hz gagne 3,3 ms en médiane et 10 ms au p90.**
  L'application suit l'écran virtuel et tourne à 240 i/s : c'est sa propre
  attente qui raccourcit, pas celle de l'hôte.
- L'encodage ne bouge pas (1,3-1,4 ms en moyenne).

### 4.2 Sous un vrai jeu : RE9, la copie

RE9 en fenêtre (1632×918) sur l'écran virtuel, la RTX à 98-99 % en 3D, son
coupé. Le jeu seul, sans clic, sur ~83 s de scène :

| Écran virtuel | Images neuves capturées | Écart entre présentations de DWM | Remise par DDA | Encodage moyen / p95 |
|---|---|---|---|---|
| 120 Hz | 78,0 par s | 13,8 ms (2 pas de la grille à 144 Hz) | 0,10 ms | 3,6 / 12,3 ms |
| 240 Hz | 77,3 par s | 12,7 ms (le rythme du jeu) | 0,12 ms | 3,5 / 13,3 ms |

- **L'écran virtuel à 240 Hz ne coûte pas d'images au jeu.** Cela répond à la
  question du 30/09 (plan framerate-hote, décision A), restée sans mesure.
- La porte du flux à 120 i/s n'écarte aucune image du jeu pendant la scène :
  à 77 i/s, il va moins vite que le flux. Les 11 par seconde « not carried »
  du journal viennent des logos et des menus.
- L'échec du 30/09, où la capture ne voyait que 3 à 7 images par seconde, ne
  revient pas. Deux choses ont changé : la fenêtre tient désormais dans
  l'écran virtuel (1936×1119 auparavant, sur 1920×1080), et elle est au premier
  plan. Le banc ne dit pas laquelle des deux comptait.

Le clic sous le jeu : `mw-click-target` en tearing par-dessus RE9, qui continue
de dessiner derrière lui à 99 %, contre le même outil sans jeu (120 Hz, 60 clics
chacun) :

| | Clic p50 | Clic p90 | Hôte p50 | Present → remise p50 | Présentations de DWM | Encodage moyen / p95 |
|---|---|---|---|---|---|---|
| sans jeu | 26,9 | 30,8 | 6,5 | 4,7 | 142 par s | 1,6 / 3,6 ms |
| RE9 derrière | 31,5 | 40,9 | 8,3 | 6,1 | 103 par s | 4,6 / 10,2 ms |

- **Un vrai jeu coûte 4,6 ms au clic.** DWM compose moins souvent quand le
  GPU est plein, d'où 1,8 ms de plus. L'encodage passe derrière le travail du
  jeu sur le même GPU, d'où ~3 ms.
- Cette part de l'encodage vient surtout du banc. La `--dev` tourne avec un
  jeton limité et n'obtient que la classe GPU HIGH. Le worker SYSTEM du
  produit installé obtient REALTIME, et le 27/09 (G2) REALTIME ramenait le p99
  de l'encodage de 10-16 ms à 3 ms.
- RE9 a planté une fois sur deux juste après son lancement à 240 Hz. Il saute
  de lui-même vers son `TargetDisplay`, un écran physique. Rien ne
  l'attribue à l'écran virtuel.

### 4.3 Pièges du banc

- **Une fenêtre d'un écran physique peut prendre les clics.** Allumé, l'écran
  virtuel prend la place 0,0. Une fenêtre de l'Explorateur, posée en 405,114
  sur DISPLAY5, se retrouvait alors dessus, au premier plan, et a pris les
  60 clics d'une passe. `mw-click-target` est maintenant « toujours au premier
  plan ». Il note aussi toute fenêtre qui le couvre sous le curseur (ligne
  `{"covered": ...}`).
- **PresentMon ne sert pas sur l'écran virtuel.** Il ne garde qu'une image sur
  13 de l'outil, et perd 30 à 60 s d'un coup, même avec `--no_track_display`.
  Quand il place une image à l'écran, il s'accorde avec DDA à 0,1 ms près.
  Il ne reste utile que pour les modes de présentation : `Composed: Flip`
  partout.
- `hostpath.py` calait mal les images du client à 240 Hz : deux présentations
  à 4 ms d'écart faussaient le décalage des horodatages. Il le choisit
  désormais par vote.

## 5. Porte AW1 et suite

**Porte AW1 : le poste mesuré au drapeau est un artefact du banc, mais il
reste de la production.** ~10 ms sur 14,5 viennent du drapeau. Pour une
application rapide, il reste 6,5 à 8 ms d'hôte, dont 5 à 6 entre son Present
et la remise par DDA. Le plein écran n'y change rien, puisque l'écran virtuel
est toujours composé. Sous un vrai jeu qui sature le GPU, la composition
ajoute ~2 ms. La synchronisation verticale du jeu est à sa charge, pas à
celle de l'hôte.

Ce que les leviers AW2 ont montré, et la suite :

1. **AW2.1 (écran virtuel à 240 Hz)** : un gain pour les jeux synchronisés,
   presque rien pour les autres à flux fixe, et aucun coût vu. C'est déjà la
   fréquence du produit sous Windows (`03c189ea`) : le banc 2 apporte les deux
   mesures qui manquaient à la décision A du plan framerate-hote, le clic et
   RE9. Reste le ressenti de Bruno.
2. **Le cadencement du flux** : à 240 Hz et à flux fixe, une présentation sur
   deux attend la tranche suivante. Le produit lève déjà ce frein par la
   détection de l'« Auto », que le banc coupait ; rien à coder. Mesurée au
   §6 : l'hôte y gagne 2 à 3 ms, mais le clic reste à refaire avec la sonde
   corrigée et en SCTP, avant AW2.2.
3. **L'encodage sous un vrai jeu** se mesure avec le produit installé
   (REALTIME), pas avec la `--dev`.
4. L'écran virtuel seul, sans écran physique allumé, n'a pas été essayé : il
   faut éteindre les écrans physiques de l'hôte de banc, ce qui ne se fait
   pas à distance.

Concrètement, pour l'utilisateur : dans un jeu qui réagit vite, sans
synchronisation verticale, l'hôte prend 6 à 8 ms d'un clic qui en dure ~11
en LAN filaire (§6.1 ; les ~27 ms mesurés au banc comptaient aussi la sonde et
le transport du banc). Le reste se partage entre l'encodage, le réseau, le
décodage et l'affichage chez le client. Jouer en plein écran ou en fenêtre ne change rien.
Activer la synchronisation verticale dans le jeu ajoute environ une image de
retard, ou deux selon la façon dont le jeu lit ses entrées : c'est un réglage
du jeu, pas de MoonlightWeb. L'écran virtuel du produit, à 240 Hz, la réduit
(3 ms de moins au clic, 10 ms de moins dans les pires cas) sans coûter
d'images au jeu.
Quand le jeu pousse le GPU à fond, un clic coûte ~2 ms de plus côté hôte.

Pour le poste B, les hypothèses B1 se resserrent sur l'aller-retour de Chrome :
présenter par le canevas WebGPU sans `onSubmittedWorkDone` (B2.1) devient le
premier levier à essayer.

## 6. Le cadencement du flux, et la sonde qui se mesurait elle-même (09/10/2026)

Créneau de 59, 07:26-07:44. Même banc qu'au §4 : l'écran virtuel du produit à
240 Hz, `mw-click-target` en fenêtre, 60 clics par passe, deux passes par case
(ABBA). Le flux est en « Auto » (120 i/s, la fréquence du client), avec la
détection coupée ou allumée (`pass.py --autostep`, le défaut du produit).
Hôte : moyenne des médianes et des moyennes des deux passes.

| Cas | Flux | Hôte p50 / moyen | Clic p50 / p90 / moyen, sonde d'avant |
|---|---|---|---|
| tearing, détection coupée | 120 i/s, 111 présentations/s écartées | 7,2 / 7,0 | 27,0 / 30,4 / 26,0 |
| tearing, détection | 240 i/s, aucune écartée | 4,4 / 4,9 | 30,6 / 39,9 / 32,5 |
| synchronisé, détection coupée | 120 i/s | 9,4 / 9,9 | 29,8 / 33,1 / 29,9 |
| synchronisé, détection | 240 i/s | 8,3 / 8,4 | 38,5 / 47,3 / 39,3 |

- **La détection fait ce qu'il faut.** Le flux monte à 240 i/s dans les
  quatre passes, et la porte n'écarte plus rien. L'hôte gagne 2,8 ms en
  médiane en tearing (2,1 en moyenne). En synchronisé, il gagne 1,1 ms en
  médiane et 4 ms au p90.
- **Le clic mesuré empire pourtant de 4 à 10 ms, et c'est la sonde.** Hors des
  clics, une image va de la capture au dessin en 12 ms, à 120 comme à
  240 i/s. Mais pendant qu'elle guette le drapeau (200 ms au plus par clic),
  le Canvas2D relit trois pixels après chaque dessin. Ce sont trois
  allers-retours vers le processus GPU : 4,7 ms par image sur le 780M, contre
  0,2 sans la sonde.
  - À 120 i/s, la relecture tient dans l'intervalle. À 240 i/s (4,2 ms), non :
    les images s'empilent derrière les relectures, et le décodage attend
    7,7 ms en médiane au lieu de 0,6.
  - Et le drapeau était daté après sa propre relecture. Sur ce chemin
    (Canvas2D sur le thread principal, le défaut), chaque clic mesuré
    comptait donc ~4,5 ms de sonde, à toute cadence, bancs 1, A1 et 2
    compris. Les écarts mesurés à cadence égale restent valables.
- **Un second écart du banc : le transport.** Mes passes faisaient passer la
  vidéo par une piste RTP (`MW_RTP_VIDEO`, hérité des lanceurs U1.4). Chrome
  remet ces images au client sur son métronome de 64 Hz (POC Ultra, §6.11 :
  7,8 ms de retenue en médiane). Elles arrivent par paquets de 2 à 5 toutes
  les ~16 ms, ce qui aggrave l'empilement. Le produit envoie la vidéo en SCTP.
- **Le correctif de la sonde.**
  - Le moteur de rendu note l'instant du dessin avant de relire, et la sonde
    date le drapeau de cet instant (`probeDrawnAt`, Canvas2D, WebGL et
    WebGPU).
  - Une seule relecture couvre la ligne entre les trois points. Sur la RTX,
    elle coûte deux fois moins : 0,6 à 1,2 ms contre 1,3 à 2,2 en médiane
    (micro-banc, Chrome sans fenêtre). Sur le 780M, elle passe de 4,7 à
    3,2-4,3 ms seulement.

### 6.1 La remesure en SCTP, avec la sonde corrigée (08:11)

Créneau de 59, 07:52-08:10, les mêmes 8 passes, avec la vidéo en SCTP comme
dans le produit. Les 480 clics sont datés à l'instant du dessin de leur image
(`--from-draw` de `hostpath.py`). L'image qui montre le drapeau est celle dont
le dessin contient cet instant. « Clic → capture » se lit sur l'horloge du
client : c'est la montée de l'entrée plus la part de l'hôte, et la sonde n'y
touche pas. « Capture → écran » est la médiane des images hors de la fenêtre
de la sonde.

| Cas | Clic brut p50 / moyen | Clic → capture p50 / moyen | Capture → écran | Clic estimé |
|---|---|---|---|---|
| tearing, détection coupée | 11,7 / 11,5 | 8,3 / 8,0 | 3,4 | 11,4 |
| tearing, détection | 11,8 / 11,8 | 3,7 / 4,1 | 3,5 | 7,6 |
| synchronisé, détection coupée | 12,4 / 13,2 | 8,5 / 9,1 | 4,0 | 13,1 |
| synchronisé, détection | 15,1 / 15,6 | 7,1 / 7,8 | 3,2 | 11,0 |

- **Le transport du banc coûtait 8 ms par image.** En SCTP, une image va de
  la capture à l'écran en 3,2 à 4 ms, contre 11,7 à 12 ms sur la piste RTP.
  Un clic brut vaut 11,5 ms au lieu de 26-27 ms.
- **La détection fait gagner 3,9 ms en moyenne entre le clic et la
  capture en tearing, et 1,3 ms en synchronisé.** Les images qui ne portent
  pas de clic n'en sont pas ralenties (3,2-3,5 ms contre 3,4-4,0). Clic
  estimé : 11,4 → 7,6 ms en tearing, 13,1 → 11,0 en synchronisé.
- **À 240 i/s, la sonde gêne encore sur le 780M**, ce qui explique le clic
  brut égal ou pire. Sa relecture de 3,2 à 4,3 ms tient le fil principal, et
  la réception de l'image suivante l'attend : l'image du drapeau arrive 0,1 ms
  après la fin de la relecture précédente, soit 5,3 ms après sa capture au
  lieu de 2,8. Lire l'image décodée par `VideoFrame.copyTo` n'y change rien :
  sur une image GPU, l'appel bloque le fil 3,1 ms (micro-banc, RTX). Une
  relecture par WebGPU (`mapAsync`) serait la piste suivante. D'ici là, un
  gain à 240 i/s se juge par cette décomposition, pas par le clic brut.

Concrètement, pour l'utilisateur : rien ne change dans le produit, mais on
sait maintenant ce qu'il vaut. En LAN filaire, avec un jeu rapide et le flux
du produit (SCTP), un clic met environ 11 ms à revenir à l'écran du client, et
l'« Auto » détecté le ramène vers 7,5 ms en montant le flux à 240 i/s. Les
deux tiers de ce temps sont côté hôte, entre l'entrée injectée et l'image
capturée : c'est là que les leviers suivants doivent chercher.
