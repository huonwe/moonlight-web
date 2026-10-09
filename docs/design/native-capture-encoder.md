# Moteur natif de capture & encodage — MoonlightWeb Native Host

> Chantier demandé dans `moonlightweb-native-capture-encoder-plan.md`.
> Branche : `feature/native-capture-encoder`.
>
> Ce document est le livrable d'architecture de la mission (§34) **tenu à jour
> par ce qui a été mesuré**, pas par ce qui était prévu. Chaque chiffre ici a
> été relevé sur du matériel réel ; les sections marquées ⚠️ consignent une
> hypothèse que le banc a **réfutée**, et sont les plus utiles à lire.
>
> Le plan de session d'origine vit dans
> `~/.claude/plans/splendid-enchanting-rossum.md` ; en cas de divergence,
> **c'est ce fichier-ci qui fait foi** — il est le seul des deux à être
> versionné avec le code qu'il décrit.

---

## 1. Résumé exécutif

MoonlightWeb est un **client** GameStream : pour streamer la machine sur
laquelle il tourne, il fallait installer Sunshine. Ce chantier lui donne son
propre moteur de capture et d'encodage, dans le processus qui tient déjà la
PeerConnection.

Le gain de latence ne vient pas du transport (inchangé) mais de ce qui
disparaît en amont :

```
AVANT (host local via Sunshine)
  capture → encode → RTP+FEC+AES-GCM → UDP loopback → moonlight-common-c
    (réassemblage, déchiffrement, FEC) → QByteArray → signal Qt en file
    → relais → fragmentation → SCTP/DTLS → navigateur

APRÈS (moteur natif)
  capture (surface GPU) → encode (zéro-copie) → fragmentation → SCTP/DTLS → navigateur
```

Supprimés : un aller-retour réseau, RTP, FEC, un chiffrement AES-GCM redondant
(DTLS chiffre déjà), le réassemblage, **et un saut de signal Qt en file**.

### Mesuré (RTX 5060 Ti, 2560×1440, H.264)

| Étape | Mesure |
|---|---|
| Capture DXGI (présent → acquis) | **0,06 ms** moyenne, 0,11 ms au pire |
| Encodage NVENC (contenu statique) | **3,46 ms** moyenne, 3,70 ms au pire |
| Copies mémoire par frame | **1** (lecture du bitstream GPU→CPU) |

Capacités confirmées en ouvrant une vraie session : AV1, HEVC, H.264, 10-bit,
4:4:4.

---

## 2. Où le module se greffe

Deux points d'extension **existaient déjà** et étaient prévus pour ça :

| Point | Fichier |
|---|---|
| `IStreamBackend` | `backend/src/backend/streambackend/IStreamBackend.h` |
| `MediaDescriptor` (union taguée) | `.../MediaDescriptor.h` |

Le seul refactor du code existant est l'extraction d'**`IMediaEngine`**
(`backend/src/streaming/IMediaEngine.h`) hors de `MoonlightShim`, pour que les
relais parlent à un moteur abstrait plutôt qu'à moonlight-common-c.
`MoonlightShim` en dérive sans qu'une ligne de son corps change.

**Inchangé, et devant le rester** : tout le chemin `gamestream` / `wolf` /
`multiseat`, le format de trame sur le DataChannel (en-tête 17 o), le décodeur
WebCodecs du navigateur. L'encodeur natif produit de l'Annex-B/OBU exactement
comme Sunshine, donc le frontend n'a rien à changer pour la vidéo.

---

## 3. Structure

```
backend/native-host/              # cible CMake mw-native-host (STATIC)
  LICENSE.md                      # la frontière juridique, expliquée
  cmake/boundary_check.cmake      # …et rendue mécanique
  include/mw/native/              # API publique : C++17 pur, zéro Qt, zéro GPL
  src/core/                       # Probe, Selector, Log, façade
  src/capture/windows/            # DxgiDuplication (+ WGC en repli, à venir)
  src/convert/windows/            # ColorConvert : NV12 (4:2:0) et AYUV (4:4:4)
  src/encode/windows/             # NvencApi, NvencCapabilities, NvencEncoder
  src/platform/windows/           # sonde + boucle de session
  third_party/nvenc-headers/      # nv-codec-headers (MIT), SDK 12.0
  tests/                          # Qt-free, exécutables sur CI sans GPU
```

### La frontière de licence est vérifiée, pas déclarée

`mw-native-host` ne lie **ni Qt, ni moonlight-common-c, ni aucune dépendance
GPL**. C'est ce qui la garde relicenciable seule (§26 de la mission).

`cmake/boundary_check.cmake` tourne **à chaque build** et casse la compilation
en nommant fichier et ligne si un `#include` interdit apparaît (Qt,
`Limelight.h`, FFmpeg, x264/x265, `backend/src/`). Vérifié en le faisant
échouer volontairement.

---

## 4. Capture

### Windows — DXGI Desktop Duplication (retenu)

`AcquireNextFrame` **débloque sur le présent réel** au lieu de scruter, et
`LastPresentTime` date ce présent en QPC. Donc t₀ est une **mesure**, pas une
estimation, et tous les chiffres de latence en aval en héritent.

Repli prévu : Windows.Graphics.Capture, pour les cas où DDA répond
`DXGI_ERROR_UNSUPPORTED` (sorties hybrides). Les deux rendent un
`ID3D11Texture2D`, donc l'étage encodeur est identique.

Trois points de justesse invisibles hors exécution :

1. **Les horloges.** DXGI date en QPC, le reste du moteur en `steady_clock`.
   Même cadence, origines différentes : sans le couple de calibration pris au
   démarrage, la latence de capture serait un écart entre deux époques sans
   rapport — grand, stable, et vide de sens.
2. **Un présent à zéro n'est pas une frame.** DXGI réveille aussi sur un simple
   mouvement de pointeur ; le compter comme une frame enverrait un doublon
   horodaté n'importe comment.
4. **Six étapes, deux threads, un mutex.** Chaque frame porte t₀ présent, t₁
   acquis, t₂ converti, t₃ encodé ; le thread émetteur ajoute t₄ premier octet
   et t₅ dernier octet (`FrameSentSink`). `NativeMediaEngine` verse les six
   différences dans des histogrammes à pas logarithmique (`StageStats`, huit
   cases par octave, percentile rendu par le bord haut de sa case : il surestime,
   jamais l'inverse). Le message `stats` porte la fenêtre (`stages`), le log
   porte la session entière en une ligne à l'arrêt. Depuis le 02/09/2026 ; rien
   sur ce chemin ne s'optimise sur une estimation.
3. **L'époque du relais.** Le moteur date en absolu ; le relais, lui, attend le
   contrat du shim GameStream : un temps de présentation **relatif à la première
   frame**, qu'il rajoute à l'époque pour retrouver l'horloge. `NativeMediaEngine`
   fait la soustraction (époque = présent de la première frame). Livrer l'absolu
   comptait l'horloge deux fois : le `backendTs` du client avançait à 2×, et
   toute mesure de gigue en aval était fausse (bug B2, corrigé le 02/09/2026).

### Linux / macOS

Linux : KMS/DRM d'abord (§19.3–19.5), le portail PipeWire en repli ; l'écran
virtuel fait par le compositeur (Mutter, KWin, portail) et les apps dans leur
propre gamescope (§35) ; le son par PipeWire (§19.7).
macOS : ScreenCaptureKit, qui écrit directement le NV12 de l'encodeur (§20).

---

## 5. Association display → GPU

C'est ce qui décide de tout le zéro-copie : capturer sur le GPU A pour encoder
sur le GPU B impose une copie VRAM→RAM→VRAM qui écrase tout le reste.

Sur Windows, **DXGI répond exactement** : l'adaptateur qui énumère une sortie
est celui qui la scanne. Aucune heuristique. Le LUID est conservé et sert à
rouvrir le même adaptateur pour la duplication *et* pour l'encodeur.

> `D3D_DRIVER_TYPE_UNKNOWN` est obligatoire quand on fournit un adaptateur.
> Demander `HARDWARE` l'ignore silencieusement et prend le défaut — c'est ainsi
> qu'un pipeline « zéro-copie » se met à copier entre GPU sans rien dire.

**La copie inter-GPU existe depuis le 04/09/2026** (`CrossGpuBridge`). Quand le
Selector choisit un encodeur sur un autre adaptateur que celui qui scanne
l'écran — un écran piloté par un GPU sans encodeur, ou le banc qui force
`gpu=<id>` pour mesurer un iGPU sans écran — la trame capturée passe par une
texture de lecture (staging) sur le GPU source, un `Map`, une copie ligne à
ligne dans une texture dynamique du GPU d'encodage, puis la conversion et
l'encodeur y sont construits. D3D11 n'offre aucune autre route entre deux
adaptateurs. Coût mesuré : **14 Mo par trame 1440p, 2 à 5 ms**, compté dans
l'étape `convert` et résumé en fin de session (« cross-GPU copy: N frames of
14 MB, x ms mean, y ms max »). `SessionInfo::copiesPerFrame` passe à 3. La
session le dit en warning au démarrage : sur ce chemin, toutes les promesses
zéro-copie de ce document sont hors jeu. Avant cette date la session refusait
de démarrer.

**Depuis le 21/09/2026, la relecture passe par le moteur de copie**
(`acdcda49`). Un `CopyResource` D3D11 part dans la file 3D du GPU source, où
il attend derrière l'image d'un jeu. Sous Resident Evil Requiem sur l'Arc,
la relecture d'une image 1440p y prenait 15 ms en moyenne et 64 au p99, pour
1,6 ms de copie. Le pont ouvre désormais la surface capturée en D3D12 par son
handle NT, et la relit sur une file COPY de ce GPU. Cette file tourne sur le
moteur DMA, que le jeu n'utilise pas : 1,6 ms en moyenne, 2,3 au p99, sous la
même charge, avec des pixels identiques. Si une étape est refusée (surface sans
handle partagé, GPU sans D3D12), la relecture reprend le chemin D3D11, et le
journal le dit une fois. `MW_BRIDGE_DMA=off` force le chemin D3D11, pour
l'avant/après. La chaîne D3D12 (§32) ne prend pas ce pont : une session qui
copie entre GPU reste en D3D11.

---

## 6. ⚠️ Ce que le banc a corrigé

### 6.1 La taille du display était fausse (mise à l'échelle DPI)

La sonde annonçait l'écran en **2048×1152** alors qu'il fait **2560×1440** —
rapport exactement 1,25, la mise à l'échelle Windows à 125 %.

`DXGI_OUTPUT_DESC::DesktopCoordinates` est exprimé en coordonnées de bureau
virtuel, que Windows **met à l'échelle** pour un processus non
per-monitor-DPI-aware. Et déclarer cette awareness depuis une bibliothèque est
exclu : c'est un réglage de **processus**, qui appartient à l'UI Qt de
l'application hôte.

→ La taille vient du **mode SOURCE** de `QueryDisplayConfig`, avec
`EnumDisplaySettings` en repli. Un test scelle l'invariant : sonde et
duplication doivent donner la même taille.

Sans ça, l'utilisateur se serait vu proposer — et aurait streamé — une
résolution amputée d'un quart de ses pixels.

`QueryDisplayConfig` sert aussi au rafraîchissement, qui porte le rationnel
exact : `EnumDisplaySettings` arrondit, et un panneau 143,98 Hz rapporté 143
fait battre la capture visiblement.

### 6.2 Le plancher de pilote NVENC, pas « la version la plus récente »

Premier essai : SDK 13.1 vendoré. Le banc — une RTX 5060 Ti **neuve** — expose
l'API 208 (SDK 13.0) et refusait toute session.

NVENC n'est rétro-compatible que dans un sens : un pilote accepte les versions
de structure de sa génération **ou plus anciennes**, jamais plus récentes. La
version d'en-tête n'est donc pas « jusqu'où peut-on monter » mais **un plancher
imposé à tous les utilisateurs**.

→ SDK **12.0** (`n12.0.16.2`, pilotes 520+/oct. 2022). Vérifié comme contenant
tout ce que le moteur utilise : AV1, `ULTRA_LOW_LATENCY`, intra-refresh,
invalidation de référence, 10-bit, 4:4:4, surfaces D3D11.

Absents de 12.0 et vérifiés comme tels : `splitEncodeMode`, filtre temporel,
`lookaheadLevel`. Seul le premier pourrait compter un jour — il répartit une
frame sur plusieurs moteurs NVENC, ce qui ne concerne que les 5080/5090 en 4K
haute fréquence. Monter à 12.2 le rendrait accessible au prix d'un plancher
pilote 2024.

### 6.3 Les adaptateurs d'écran virtuel ne se disqualifient pas

Le banc rapporte **5 adaptateurs DXGI pour 3 GPU physiques** : les deux en trop
sont Parsec Virtual Display et Virtual Display Driver, qui présentent le nom et
le device-id NVIDIA sous un LUID propre.

L'hypothèse était qu'ils refuseraient une session NVENC et tomberaient d'eux-
mêmes. **Faux** : adossés à une vraie carte NVIDIA, ils en ouvrent une et
rapportent les mêmes codecs.

C'est en réalité la bonne réponse — un display accroché à l'un d'eux doit être
capturé **et** encodé là, DXGI routant les deux vers le même silicium. Il n'y a
donc rien à filtrer : l'identité d'adaptateur vient de DXGI, et la requête de
capacités sert seulement à connaître les codecs.

### 6.5 Un host sans adresse a fait déborder la pile

Le premier `/apps` sur le host natif tuait le processus — `0xc0000005` dans
`ntdll.dll`, sans une ligne de log (une pile épuisée ne peut pas se dérouler
pour écrire).

Le préchargement des jaquettes marque une app en attente, appelle
`startBoxArtFetch` qui ne trouve **aucune adresse** — le host natif EST ce
processus — et conclut l'échec *immédiatement* ; le gestionnaire de complétion
retire le marqueur puis rappelle le préchargement, qui rechoisit la même app.

Ce qui espace normalement les tentatives, c'est l'attente d'une réponse réseau.
Sans réseau, rien ne casse la boucle. Le code existant supposait, sans le dire,
que tout host de la liste a une adresse — invariant que le host natif a brisé.

Corrigé aux deux niveaux : un host sans adresse ne précharge rien, et une app
dont la jaquette a échoué n'est plus rechoisie dans la même passe (sans quoi un
échec réseau sur un host réel bouclait aussi — en requêtes plutôt qu'en pile,
donc invisible mais bien présent).

### 6.4 « Un encodeur » ne veut pas dire « peut encoder »

L'iGPU AMD du banc annonce le runtime AMF avec une **liste de codecs vide** (la
requête AMF n'est pas écrite). Le sélecteur s'y serait replié — payant une
copie inter-GPU — pour ensuite échouer à la négociation de codec, en
abandonnant la RTX qui pouvait le faire.

→ « Pouvoir encoder » exige un encodeur **et** au moins un codec. Même règle
dans `Probe.cpp` pour la disponibilité globale : un host qui échoue au clic est
pire que pas de host.

---

## 7. Encodage

Chaque réglage est une décision de latence, prise **contre** les défauts de
NVENC qui visent l'encodage de fichiers.

| Réglage | Pourquoi |
|---|---|
| `ULTRA_LOW_LATENCY`, **preset P1** (P4 jusqu'au 04/09/2026) | La croyance « P1 plus mou pour moins d'une milliseconde » était fausse (`docs/bench-native-host.md`) : sur une RTX 5060 Ti à 1440p, P1 encode un FPS en **3,4 ms contre 7,7** pour P4 (p99 5 contre 11), au **même QP** (25) et sans rien de visible sur l'image décodée ; +1 de QP sur un jeu de plateforme, +5 seulement sur du texte qui défile. Appliqué le 04/09/2026 sur confirmation de Bruno. Le preset ULL active de lui-même le multipass quart de résolution, **conservé** : l'éteindre gagne 0,6 ms mais casse la rafale de raffinement de l'écran fixe (§9.1, QP bloqué à 29 au lieu de 8) |
| **VUI `bitstream_restriction` en H.264** | sans `max_num_reorder_frames` le décodeur D3D11 de Chrome retient un DPB entier avant d'afficher : **200 ms** de décodage mesurés sur un flux sans B-frame, 1 ms après. Le HEVC le porte par défaut (04/09/2026) |
| **Aucune B-frame** (`frameIntervalP = 1`) | elle référencerait une image pas encore envoyée → une trame entière retenue |
| **GOP infini + intra-refresh** | une keyframe est un pic de débit ; `MediaTrackRelay` documente ce que ces pics font à un lien congestionné (perte → tempête de PLI → effondrement) |
| **CBR, VBV = une frame** | c'est le VBV qui impose réellement la faible latence : aucune frame ne peut être si grosse qu'elle mette plusieurs temps de trame à passer |
| **SPS/PPS à chaque keyframe** | le décodeur du navigateur s'y configure ; un client qui arrive en retard doit pouvoir démarrer sur la suivante |

Le zéro-copie tient : NVENC enregistre directement la texture D3D11 écrite par
la passe de conversion, sur le même adaptateur.

`setBitrate()` reconfigure sans redémarrer la session — la base du rate-control
piloté par le retour réel du client.

**Le VBV de `setBitrate()` passe par le même plancher que celui de l'init**
(`vbvBitsPerFrame`, `RateControl.h`), sur les trois encodeurs. Jusqu'au
02/09/2026 AMF divisait simplement par le fps : la rafale de raffinement (§9.1)
appelle `setBitrate()` deux fois par transition figé/mouvement, et chaque retour
au budget ordinaire laissait l'encodeur AMD avec un VBV 2,4× plus serré à 144 Hz
que celui choisi à l'init — l'image molle que le plancher existe pour éviter,
réintroduite par le chemin censé la rendre nette. Corrigé (audit B4) ; le test
`test_rate_control.cpp` fixe l'égalité init/ré-application à tout fps.

### L'intra-refresh ne sert à rien sans un récepteur qui l'accompagne

L'intra-refresh est implémenté sur les **trois** encodeurs (NVENC, AMF, oneVPL).
Mais il ne gagne rien tant que le navigateur continue de réclamer une keyframe
au premier trou : on paierait le coût de la vague de rafraîchissement **et** le
pic de la keyframe. Le gain n'est pas en régime établi, il est dans la
**récupération de congestion** — aujourd'hui un trou fait jeter les deltas des
deux côtés et exige la plus grosse frame possible sur un lien qui vient de
prouver qu'il saturait.

D'où un contrat en trois temps, chacun capable de dire non :

| Étape | Qui décide | Ce qui circule |
|---|---|---|
| Demande | le navigateur | `ride_out_loss` dans `/start` (constante `RIDE_OUT_LOSS` dans `BackendClient.js`, un booléen prévu pour les A/B) |
| Octroi | l'encodeur | `SessionInfo::intraRefresh` — ce qui a été **accordé**, pas ce qui a été demandé |
| Application | les deux extrémités | `intra_refresh` dans la réponse `/start` |

La direction compte : le serveur renvoie ce que le flux **fait**. Un récepteur
qui suppresserait ses demandes de keyframe face à un flux sans vague de
rafraîchissement resterait indéfiniment sur une image corrompue. Côté backend,
`DataChannelRelay::ridingOutLoss()` exige donc **et** l'opt-in du client **et**
`IMediaEngine::intraRefreshActive()`, et ne débraye que les deux portes de perte
— jamais celles du démarrage de session, où il n'y a aucune référence à
rattraper. Côté navigateur, la suppression est bornée par un chien de garde de
2,5 s : passé ce délai sans trame contiguë, on redemande une keyframe.

**Pas de demande depuis une plateforme Apple (28/09/2026).** Le pari suppose un
décodeur qui décode un delta dont la référence n'est jamais arrivée, en
rapiéçant le trou : c'est ce que font les décodeurs de Windows, sur lesquels il
a été mesuré. VideoToolbox, qui décode tout flux H.264 et HEVC de WebCodecs sous
macOS, iOS et iPadOS, le refuse (« Decoding error »). Vu au test C5.7 du plan
D3D12 (Chrome sur un Mac, par Internet, hôte Arc en oneVPL avec intra-refresh) :
trois calages du lien en trois minutes, chacun suivi d'une erreur du décodeur,
d'un décodeur neuf et d'une keyframe — 60 à 110 ms d'image figée, et la keyframe
que le pari devait éviter arrivait quand même, une erreur plus tard. Aucune API
ne distingue les deux décodeurs : `decoderRidesOutGaps` (`BrowserDetect.js`) lit
l'agent utilisateur, et le navigateur ne demande pas `ride_out_loss` sur une
plateforme Apple. L'hôte encode alors sans vague et récupère à l'ancienne —
deltas jetés pendant le calage, keyframe dès que le lien se vide —, le chemin
qui tournait déjà sans erreur sur ce Mac en D3D12 au même test. Ailleurs, rien
ne change.

Le flag traverse le processus worker (`cfg["rideOutLoss"]`) : le moteur média
vit dans l'enfant, le poser sur la session du parent ne l'atteindrait jamais.

**La période de la vague est une durée, pas un nombre de frames** (audit B6,
corrigé le 03/09). Les trois encodeurs figeaient 120 frames, qui ne font
2 s qu'à 60 fps : un flux à 30 fps mettait 4 s à se réparer — plus que le
chien de garde du client, qui redemandait donc la keyframe et payait les deux —
et un flux à 144 fps balayait en 0,8 s, soit 2,5 fois les bits intra par
seconde nécessaires. `intraRefreshPeriodFrames(fps)` dans `RateControl.h`
donne 2 s **dans la cadence effective** (le réglage, ou le Hz de l'écran à
réglage 0 — jamais la cadence de l'écran quand le flux est bridé sous elle),
bornée à [30, 600] contre l'absurde ; NVENC prend la moitié comme longueur de
vague, AMF en dérive son compte de blocs par frame (arrondi vers le bas, donc
le balayage ne peut que dépasser légèrement 2 s, jamais y rester en dessous :
1080p HEVC à 240 fps = 1 CTB par frame, 510 frames), oneVPL le pose dans
`IntRefCycleSize`. La ligne « ready » de chaque encodeur dit la période
retenue (`intra-refresh over N frames`). Vérifié en flux réel sur la RX 7600
(AMF HEVC) : 120 frames à 60 fps, 480 à 240 fps, image présente dans les deux
cas.

---

## 8. Conversion couleur — et le 4:4:4

BT.709 plage limitée, l'espace que le pipeline négocie déjà pour le SDR : un
stream natif rend comme un stream Sunshine sur le même écran.

**Un rendu, pas un compute shader** : D3D11 ne sait pas lier un UAV sur un plan
de NV12 (les tranches de plan n'existent qu'en D3D12). Ce qu'il sait faire,
c'est une RTV typée — une vue `R8_UNORM` d'une texture NV12 adresse son luma,
une vue `R8G8_UNORM` son chroma.

| Chroma | Sortie | Passes |
|---|---|---|
| 4:2:0 (défaut) | NV12, 2 plans | 2 draws |
| 4:4:4 (option On) | AYUV empaqueté | **1 draw** |

Le 4:4:4 est donc plus *simple* que le 4:2:0. Mesuré : keyframe de **53 627
octets contre 39 764** en 4:2:0 — les ~35 % attendus.

Deux pièges silencieux :

- **l'ordre des octets d'AYUV** (V, U, Y, A dans un mot 32 bits) — se tromper
  échange les couleurs au lieu d'échouer ;
- **le profil ET `chromaFormatIDC`** doivent suivre le format d'entrée, sinon
  NVENC accepte du 4:4:4 et encode du 4:2:0 en jetant la chroma : indiscernable
  d'une option sans effet.

Le 4:4:4 n'est activé que si le client le demande **et** que l'encodeur sait le
faire, avec une trace explicite sinon. Dégrader en silence serait pire que
refuser : toute la raison de demander du 4:4:4 est la lisibilité du texte.

Le HDR est **refusé explicitement** plutôt que converti comme du SDR : le FP16
scRGB demande une transfert PQ et une cible P010. Le traiter avec la matrice SDR
donnerait une image délavée — faux d'une façon qui ne se voit pas.

**Corrigé le 02/09/2026 (B1).** Ce refus était atteint sur toute machine avec le
HDR Windows actif : la duplication demandait FP16 *en premier* quel que soit le
stream, DXGI livrait donc le bureau en FP16, et le convertisseur refusait → le
host natif échouait au clic. Désormais le format demandé suit **ce que la session
consomme**, pas ce que l'écran fait : `DxgiDuplication` reçoit `hdr` et ne nomme
`R16G16B16A16_FLOAT` que si la session rend du HDR ; sinon `B8G8R8A8_UNORM` seul,
et DXGI livre un bureau HDR **déjà ramené en SDR** (tone-mapping système), qui est
exactement l'image qu'un stream SDR doit porter. La session demande d'abord au
convertisseur (`ColorConvert::supportsSource`) s'il sait traiter FP16 ; tant que le
chemin P010 n'existe pas, un HDR négocié par le Selector est **rabattu en SDR avec
une trace** (« HDR negotiated but this build converts SDR only ») plutôt que
d'échouer. Vérifié sur écran SDR (comportement identique, `(SDR, BGRA8)` dans le
log) ; le cas HDR actif reste à confirmer à la main sur un écran en HDR.

**Dépassé le 16/09/2026 (§16.6).** Le « déjà ramené en SDR » de DXGI est un
écrêtage à 80 nits, pas un tone-mapping : dès que le curseur « luminosité du
contenu SDR » de Windows est monté, tout le bureau arrive cramé. La duplication
livre désormais toujours le bureau tel qu'il est (FP16 s'il est HDR) et c'est le
convertisseur qui ramène le SDR, avec le niveau de blanc lu sur l'écran.

---

## 9. Boucle de session

Un seul thread, aucune file d'attente. Ni l'un ni l'autre ne se justifierait :

- une file n'aide que si le producteur va plus vite que le consommateur, or le
  consommateur **est le réseau** — prendre du retard signifie que le lien est
  plein, et tamponner dans un lien plein ajoute du délai sans livrer plus ;
- un second thread coûterait un réveil par frame pour recouvrir un travail
  d'une milliseconde.

La boucle bloque dans `AcquireNextFrame` : elle est cadencée par l'écran, pas
par un minuteur choisi, et un bureau immobile ne coûte rien.

### 9.1 Le plancher sur écran immobile

La capture livre **sur dommage** : un écran où rien ne bouge ne produit rien, ce
qui est indiscernable d'un stream mort. D'où un plancher — une frame toutes les
500 ms, la dernière image ré-encodée, quelques centaines d'octets.

**Ce nombre appartient à la capture, pas au client.** Combien de temps un écran
figé peut se taire dépend de l'exactitude avec laquelle la plateforme signale un
dommage. Sur DDA le signal est exact : `AcquireNextFrame` rend la main sur le
présent réel, 0,06 ms après. Tout changement est donc une frame *immédiatement*,
puis la **rafale de raffinement** ré-encode cette image pendant 1 s à ×3 de
budget jusqu'à convergence. Un plancher plus haut ne ferait que continuer après
ça, au budget ordinaire, sur une image que l'encodeur a déclarée finie — donc en
mode bureau il n'apporte rien, quel que soit l'appareil. Une plateforme au
signal plus flou remonte ce nombre ici, et aucun client ne l'apprend.

**La rafale est cadencée sur le lien (02/09/2026, audit B5).** Jusque-là elle
partait à ×6 aussi vite que l'encodeur produisait : au banc, la première passe
d'un stream à 20 Mbps sortait à 248 Ko, exactement le plafond ×6, soit 100 ms
d'un lien à 20 Mbps — et la rafale entière (plusieurs centaines de Ko) se
retrouvait devant la première frame du mouvement suivant. Deux corrections :

- **×3 au lieu de ×6.** Le multiplicateur n'est pas une netteté : une image
  fixe converge vers le même total de bits quel que soit le plafond par frame
  (chaque passe code le résidu de la précédente), un plafond plus petit étale
  seulement les mêmes bits sur plus de passes. Ce qu'il est, c'est **une
  latence** : le temps maximal qu'une passe occupe le lien, donc l'attente
  maximale d'une frame de mouvement arrivée juste derrière — `boost / 60` s,
  soit 50 ms au lieu de 100.
- **Une passe n'est envoyée que lorsque le lien a fini la précédente**,
  estimé par le débit réglé du stream (`LinkOccupancy`, `RateControl.h`) : la
  rafale n'a jamais plus d'une passe d'avance sur le lien, quel que soit le
  débit. Le plancher de vivacité n'est pas cadencé (quelques centaines
  d'octets). Le log de fin de rafale dit combien de réveils ont été retenus
  (`N passes, M held for the link`).
- **La sortie de rafale ne se fie plus à la taille seule.** Le premier flux
  réel rejoué après les deux points ci-dessus a montré `58 KB + 3097 KB over
  26 passes, 8 held for the link (window closed)` sur un écran immobile à
  55 Mbps / 165 fps : en CBR l'encodeur remplit chaque passe jusqu'à sa cible
  (~120 Ko) quoi qu'il ait à dire, donc le critère « passe ≤ 2 Ko » n'était
  atteint qu'à 1 Mbps, et partout ailleurs la rafale durait toute sa seconde —
  3 Mo par arrêt de souris, sans une ligne de log (seule la convergence était
  journalisée). `RefineConvergence` (`RateControl.h`) tranche désormais sur
  **une passe minuscule ou un QP qui ne baisse plus** (deux passes de suite
  après quatre), avec un **plafond de 8 passes** pour l'encodeur qui ne
  rapporte pas de QP — soit 400 ms de lien au plus pour une image fixe. Toute
  sortie est journalisée (`converged`, `pass cap`, `window closed`, `screen
  moved`, `display lost`), avec le QP première → dernière passe quand il est
  connu. Rejoué sur le même écran : `58 KB + 1081 KB over 8 passes, 2 held for
  the link, QP 32 -> 11 (pass cap)` — le tiers des octets, et le QP dit ce que
  la rafale a acheté.

Pourquoi une estimation et pas `bufferedAmount` : ce compteur ne mesure que ce
que libdatachannel garde **après** refus d'usrsctp, dont le tampon d'émission
fait 1 Mio (`sctptransport.cpp`, `sctp_sendspace`). Sur un lien à 20 Mbps ce
sont 420 ms de retard qui se lisent zéro ; le compteur ne bouge pas pour une
rafale de quelques centaines de Ko. Le débit réglé est la parole de
l'utilisateur sur son lien, celle que le rate control croit déjà pour chaque
frame ; le retour mesuré du client (§9.3) le remplacera comme débit du modèle
quand il existera, le modèle ne change pas.

Un client peut demander **plus que la vivacité**, par un message `framefloor`
que les trois relais transmettent :

| Situation du client | Plancher demandé |
|---|---|
| Bureau, pointeur libre (tout appareil) | rien — celui de l'hôte |
| Mode jeu, pointeur capturé | **30 fps** — le pointeur est *dans* l'image, et écran figé ≠ session inactive (pause, menu, chargement) |

Deux bornes, côté hôte : jamais plus vite que le fps du stream (le réglage de
l'utilisateur passe avant une demande venue d'une page), jamais plus lentement
que les 500 ms. Le timeout d'`AcquireNextFrame` suit le plancher, sinon une
frame due à 33 ms serait livrée à 100.

### 9.6 Cadence : le fps réglé est tenu (02/09/2026)

Jusqu'au 02/09, la borne « jamais plus vite que le fps du stream » ne valait
que pour le plancher : la boucle, elle, encodait **chaque présent DXGI**. Sur
l'écran 165 Hz du banc avec 60 réglés, 164 images/s traversaient un CBR
dimensionné pour 60 — le budget est *par image*, donc le fil portait 2,75 × le
débit réglé sur un écran en mouvement (mesuré : 21,7 Ko de moyenne × 164 =
28 Mbit/s pour 20 réglés, 63 Mbit/s quand les images plafonnaient au VBV en
déplaçant une fenêtre). Invisible en LAN ; sur Internet l'excédent attend dans
le SCTP et se sent comme un pointeur qui traîne.

Le mécanisme (`FrameCadence`, pur, testé sans écran) : l'intervalle du stream
est une grille. Chaque présent est **converti** (la texture de sortie du
convertisseur est ce que les chemins « écran fixe » réémettent, elle doit
rester la plus fraîche) mais seul le **premier présent à ou après chaque
échéance** est encodé, **à l'instant où il arrive**. Les autres sont sautés.
Rien n'attend jamais sur le thread de capture. La grille avance d'un intervalle
à chaque présent admis (jamais « maintenant + intervalle », sinon la cadence
dérive — mesuré 56 fps pour 60) ; un présent qui arrive un peu **avant**
l'échéance, dans le quart d'intervalle qui la précède, passe aussi, plutôt que
d'attendre une période d'écran entière pour le suivant. La grille n'était
**ré-ancrée** sur le présent admis que s'il était en retard de plus d'une
période d'écran, pris pour un présent manquant : écran resté fixe, boucle
bloquée, ou jeu tournant au fps du stream mais déphasé — auquel cas la grille
se verrouillait sur lui au lieu de battre contre lui. Cette règle prenait
aussi pour manquant le présent d'un contenu plus lent que l'écran ; elle est
remplacée depuis le 09/10 par des fenêtres (§9.6.1).

**Première version, abandonnée le 02/09 au soir.** Elle faisait l'inverse :
retenir le *dernier* présent de chaque intervalle et l'encoder à l'échéance,
réveil par le timeout d'`AcquireNextFrame` sous `timeBeginPeriod(1)`, pour
une émission parfaitement régulière et l'image la plus fraîche possible *à
l'échéance*. Mesurée par Bruno à 30 km par Internet : cette attente faisait
**5,4 ms de moyenne et 17 ms au p99**, soit les deux tiers du temps hôte (8,1 ms
sur un pipeline qui en coûte 2,7) — un présent arrivé juste après une échéance
patientait l'intervalle entier. Une image qui attend sur l'hôte est de la
latence que le joueur sent ; une émission en avance ou en retard d'une période
d'écran (6 ms à 165 Hz, rien quand le jeu tourne lui-même au fps du stream) ne
l'est pas, et avec le *tearing* côté client elle est invisible. Latence
d'abord, régularité ensuite. L'étape `hold` (t₂ → t₂ᵇ, `dueUs`) a disparu des
stats, de l'overlay et du banc avec elle ; `encode` va désormais de t₂ à t₃.

Le Selector résout « 0 » en la fréquence arrondie de l'écran ; la garde n'est
construite que si le stream est **plus lent** que l'écran. À la cadence de
l'écran tout présent est encodé tel quel — une garde au même rythme retiendrait
encore un présent arrivé un peu tôt pour le jeter au suivant (mesuré : 23 sur
1 651 en 10 s).

Le budget de l'encodeur suit désormais le fps **effectif** (`m_EncodeFps`) et
non plus 60 quand le réglage était 0.

Banc du 02/09 (première version, avec l'étape `hold` qui n'existe plus),
1440p HEVC AMF sur le RX 7600, 20 Mbit/s, la même vidéo 4K jouant à l'écran :

| Réglage | présents | encodées | non portées | hold ms | encodage ms | présent→encodé ms | Ko/image |
|---|---|---|---|---|---|---|---|
| 60 (avant) | 1 641 | 1 641 | 0 | — | 4,28 / 5,12 / 5,63 | 4,75 / 6,66 / 7,68 | 21,7 moy., 48 p95 |
| **60 (après)** | 1 647 | 601 | 1 045 (104/s) | 0,99 / 5,63 / 6,14 | 4,10 / 5,12 / 5,63 | 5,52 / 9,22 / 10,24 | 36,0 moy., 48 p95 |
| 0 = 165 | 1 651 | 1 628 | 0 | — | 4,24 / 5,12 / 5,63 | 4,87 / 6,66 / 8,19 | 13,2 moy., 40 p95 |

(moyenne / p95 / p99.) Sur un vrai flux depuis l'instance dev (1080p HEVC
AMF, 20 Mbit/s, 37 s, bureau avec une vidéo qui joue) : 6 107 présents, 2 148
images envoyées, 105/s non portées ; `hold` 0,50 / 4,61 / 6,14 ms, encodage
3,31 / 4,10 / 4,61, **total présent → dernier octet 4,73 / 8,19 / 10,24 ms**.
Le gain : 36 Ko × 60 = 17 Mbit/s réels pour 20 réglés, au lieu de 28 à 63 ;
l'encodeur dépense enfin son budget par image sur du contenu.

**Version sans attente, mesurée le 02/09 au soir**, même instance dev, même
écran 165 Hz, 60 réglés, 1080p HEVC AMF, 63 s de bureau avec une vidéo qui
joue : 10 386 présents, 3 793 images envoyées (60/s exactement), 104/s non
portées ; hôte `acquire 0,11 / 0,21 / 0,29 · convert 0,18 / 0,35 / 0,51 ·
encode 3,38 / 4,10 / 4,61 · queue 0,16 / 0,38 / 0,96 · send 0,33 / 0,70 /
1,02 · total 4,19 / 5,63 / 6,66 ms` (moyenne / p95 / p99). La session
précédente, avec rétention, sur le même banc : total **7,70 / 20,48 / 24,58**.
Le p99 hôte a été divisé par presque quatre pour le même débit sur le fil.

### 9.6.1 La porte porte le débit demandé : des fenêtres au lieu du recalage (09/10/2026)

Le banc Android TV (B, 08/10) l'a trouvé : un flux à **50 i/s** d'un contenu à
60 i/s, sur un écran à 144 Hz, ne portait que **40 i/s** (« 606 not carried
(20/s) »). Le contenu présente toutes les 16,7 ms. Avec une grille de 20 ms,
la première présentation après un tic est souvent en retard de plus d'une
période d'écran (6,9 ms), alors qu'aucune ne manque. La porte recalait alors
la grille sur elle, la suivante tombait trop tôt et sautait : deux images sur
trois. Rejoué sur RE9 (77 i/s) sur l'écran virtuel à 240 Hz, réglage par
défaut depuis le 30/09 : 48,4 i/s portées pour 60, 40,7 pour 50. La même règle
coupait aussi un tiers des images d'une mire à 60 i/s sur un écran à 120 Hz,
flux à 60 (N4 du même banc). Chaque image tombe un rafraîchissement trop tôt
ou trop tard : la porte se recalait sur les tardives et sautait les précoces.

**La règle (`FrameCadence::admit()`).** Chaque tic possède une fenêtre, d'un
quart d'intervalle avant lui à un quart avant le suivant. La première
présentation de la fenêtre est encodée à l'instant où elle arrive, aussi tard
soit-elle dans la fenêtre, et la grille avance d'un intervalle sans changer de
phase. Seule une fenêtre restée vide déplace la grille :
- la présentation qui suit arrive dans l'intervalle suivant (un jeu au débit
  du flux qui a glissé hors de sa fenêtre, un contenu un peu plus rapide avec
  un trou plus long que d'habitude) : elle compte pour le tic manqué, et la
  fenêtre suivante s'ouvre sur elle ;
- deux fenêtres vides ou plus (écran fixe, boucle bloquée) : la grille repart
  d'elle, un intervalle plus loin, comme avant.

Les quatre exigences tiennent :
- rien n'est jamais retenu ;
- le premier changement après une pause part aussitôt, et le suivant un
  intervalle plus tard ;
- aucun rattrapage en rafale : un seul tic manqué est rattrapé au plus, et
  seulement tant que le contenu continue ;
- un jeu au débit du flux se cale : sa présentation la plus tardive tombe en
  fin de fenêtre, les autres dedans.

Le plafond (`ceiling()`, flux au débit de l'écran ou au-dessus) garde sa
règle, à l'identique.

**La piste écartée.** Avancer la grille d'un nombre entier d'intervalles
depuis le tic manqué, sans jamais changer sa phase, rend le débit au contenu
plus rapide. Mais un jeu au débit du flux, à un rafraîchissement près, bat
alors contre la grille : en simulation, il descend jusqu'à 45 i/s pour 60 sur
un écran à 120 Hz. Elle rattrape aussi après une pause : deux images coup sur
coup.

**Hors ligne.** Tests natifs (`test_frame_cadence.cpp`) : 60 i/s sous 50 à
144 et 240 Hz, et 77 i/s sous 60 à 240 Hz, à ±1 % ; un jeu à 60 sous un flux
à 60, sur des écrans à 120, 144, 165 et 240 Hz, au rafraîchissement près, avec
une dérive de ±0,1 % : au plus 4 images écartées en 20 s ; la pause, la
rafale, le plafond. L'ancienne règle échoue à 493 vérifications de ces tests,
l'avance par intervalles entiers à 227. Rejeu des passes du plan « attente »
(`scripts/bench/clickpath/gatesim.py`, `--before` pour l'ancienne porte) :

| Scène de RE9, écran virtuel à 240 Hz | flux 60 | flux 50 | flux 30 |
|---|---|---|---|
| avant | 48,4 | 40,7 | 29,6 |
| après | 59,6 | 49,7 | 29,8 |

Les passes de l'outil à 240 i/s (flux 120 sur l'écran à 240 Hz) gagnent
0,1 à 0,3 i/s ; celles où le flux est un plafond ne changent pas. Deux images
admises à moins d'un demi-intervalle l'une de l'autre restent rares : moins
de 3 % sur les traces de RE9 et de l'outil, aucune à moins d'un quart.

### 9.7 Écran verrouillé : la session attend, le stream reste vivant (02/09/2026)

`AcquireNextFrame` rend `DXGI_ERROR_ACCESS_LOST` pour trois raisons de durées
très différentes : un changement de mode (quelques centaines de ms), une
invite UAC sur bureau sécurisé (le temps de la lire), un **verrouillage** —
Win+L, écran de veille, capot refermé — pendant lequel DXGI refuse d'ouvrir
une duplication du bureau sécurisé, des minutes durant. La première version de
`restartCapture` bornait l'attente à 10 s : verrouiller le PC depuis le stream
tuait le stream (bug B3 de l'audit).

Désormais la boucle réessaie **tant que la session tourne**, au pas de
`RestartBackoff.h` : 100, 200, 400, 800 ms puis 1 s (une tentative = création
d'un device D3D11 + `DuplicateOutput`, ce n'est pas gratuit, et un écran
verrouillé depuis une minute ne reviendra pas dans les 100 ms). Pendant
l'attente le stream **reste vivant** : sans ça le navigateur déclare la famine
à 1 s de silence et descend l'échelle de qualité sur une session qui n'est que
verrouillée. Ce qui part est une **image noire**, au plancher d'écran fixe
(500 ms, ou le plancher demandé par le client), encodée par l'encodeur que la
session a encore — convertisseur et encodeur gardent leurs propres références
au device et survivent à la perte de la duplication ; ils ne sont libérés
qu'une fois la duplication rouverte, juste avant la reconstruction, pour ne
jamais avoir deux sessions matérielles ouvertes à la fois. Noir plutôt que le
dernier bureau : un bureau figé ressemble à un gel, et celui qui vient de faire
Win+L le referait. L'input continue de passer tout le temps (`SendInput`
atteint le bureau sécurisé), donc un mot de passe tapé dans le noir
déverrouille l'hôte ; la duplication rouvre, la première image est une
keyframe. Le journal dit « display is away », « still away after 10 s » une
fois, puis « display is back after N s and M failed attempts ».

Vérifié le 02/09 sur l'instance dev : quatre changements de mode 1440p ↔ 1080p
pendant un stream, reprise au premier essai à chaque fois (≈ 50 ms, aucune
image noire nécessaire), image intacte. **Le verrouillage lui-même reste à
tester à la main** : bench-desk a UAC en « élever sans demander », donc pas de
bureau sécurisé provoquable depuis un script, et verrouiller le poste sans
l'accord de son utilisateur n'est pas une chose que l'agent fait.

### 9.8 Le budget de l'encodeur suit la cadence réelle (04/09/2026)

Le CBR est un budget **par image** : débit ÷ cadence. La cadence dont l'encodeur
est configuré est le réglage du stream — ou le rafraîchissement de l'écran quand
le réglage est 0 — et un jeu ne tourne presque jamais à l'une ni à l'autre. Un
jeu à 60 fps sur un écran 165 Hz sous un stream « 165 » produit 60 images par
seconde, chacune dotée d'un 165e du débit : le fil porte 60/165 de ce que le
joueur a autorisé, et l'image est quantifiée comme si le lien était 2,75 fois
plus petit. Mesuré au banc (Call of Duty à 60 fps, 40 Mbit/s) : 29 Ko et QP 25
par image avec le réglage à 165 ; 65 Ko et QP 12 avec le réglage à 60 — même
contenu, même lien, même encodeur.

`encode::EffectiveCadence` compte les images réellement encodées depuis une
capture sur des fenêtres d'une seconde et tient la cadence pour laquelle le
budget est dimensionné. Le budget est déplacé en **multipliant le débit
transmis à l'encodeur** par cadence configurée ÷ cadence réelle — pas en
reconfigurant la cadence, parce que les trois encodeurs ont un `setBitrate()`
à chaud et aucun ne promet un changement de cadence à chaud. Le fil porte
toujours le débit réglé : (débit × 165/60) × 60 images = débit. Le VBV grandit
avec, et c'est le but : une image plus grosse est permise parce qu'il en vient
moins. Le modèle de lien (`LinkOccupancy`) ne voit rien de tout cela — c'est le
débit du fil, et il ne bouge pas ; la rafale de raffinement multiplie par-dessus.

Sens de variation : une cadence qui **monte** est rattrapée dans le quart de
seconde (sous-fenêtre de 250 ms — chaque image est désormais trop grosse, et
une seconde à 2,75× le lien c'est le pointeur qui traîne) ; une cadence qui
**descend** attend deux fenêtres d'une seconde qui s'accordent (un à-coup ne
gonfle pas la seconde suivante) ; 15 % d'hystérésis ; jamais sous 30 fps (un
écran fixe ne capture presque rien, ses passes ont leur propre budget) ni
au-dessus du réglage (la parole du joueur sur son lien). Log : « frames arrive
at 59 fps for a 165 fps stream — encoder budget 111864 kbps per second of
frames (40000 on the wire) ».

Vérifié sur le clip : à 40 Mbit/s, 61 Ko et QP 16 par image en moyenne sur 20 s
(dont les deux premières à l'ancien budget) contre 29 Ko et QP 25 ; à 20 Mbit/s,
31 Ko et QP 22 contre 15 Ko et QP 31. Encode +0,3 ms pour des images deux fois
plus grosses. Le fil reste sous le réglage.

### 9.9 Le débit suit le lien, piloté par ce que voit le récepteur (04/09/2026)

Le §9.3 annonçait un rate control « piloté par le retour réel du client » ; le
voici, et il a fallu corriger le plan sur deux points. `clientstats` n'existe
que sur le transport media (compteurs RTP), et `bufferedAmount` ne voit rien :
il compte ce qui a débordé du méga-octet de tampon usrsctp, donc 420 ms de
retard à 20 Mbit/s se lisent zéro. Sur l'hôte, un lien qui s'étrangle ressemble
à des images qui partent à l'heure. **Le premier endroit où la file se mesure
est l'arrivée** : chaque image porte l'heure de présent de l'hôte, et le
récepteur sait de combien elle arrive plus tard que d'habitude — la file, en
millisecondes, des secondes avant la première perte. C'est l'idée du contrôle
de congestion par le délai de WebRTC, réduite à l'os : un flux, un sens, un
récepteur qui horodate tout, un encodeur qui change de débit entre deux images.

**Client** (`StreamView._startLinkReporting`, hôte natif sur DataChannel) : un
message `linkstats` toutes les 500 ms — `owdRiseMs` = minimum sur la fenêtre de
(arrivée − présent hôte) moins le minimum de session (référence glissante sur
30 s, pour qu'une dérive d'horloge ne se lise jamais comme une file) ; le
décalage entre les deux horloges s'annule dans la soustraction, il ne reste que
l'attente — `gaps` = trous de numérotation, `fps`. Sur media, les compteurs de
`clientstats` alimentent la même entrée (pertes RTP en trous, jitter en délai).
Côté hôte, les évictions du `FrameSender` s'ajoutent au rapport : c'est de la
perte que l'hôte s'inflige, et le signe le plus sûr d'un lien plein.

**Moteur** (`encode::RateGovernor`, pur, testé ; `Session::reportLink`,
`LinkFeedback` en en-tête public) : surutilisation — délai ≥ 30 ms (deux
intervalles à 60 fps), un trou ou une éviction — **coupe de 20 % à l'instant**
et gel de 2 s ; trois secondes de calme (délai < 10 ms, rien de perdu)
remontent de **5 % par rapport**, jamais au-dessus du réglage ; entre les deux
(file présente, pas croissante) on tient ; plancher 20 % du réglage et
2 Mbit/s ; quatre secondes sans rapport valent une coupe, une seule — et
**depuis le 05/09**, le premier rapport d'un récepteur revenu de l'arrière-plan
porte `resumed` : ses trous sont les images que notre propre émetteur a évincées
faute de drainage et son silence était celui du navigateur, donc ce rapport n'est
pas lu comme une surutilisation et **la coupe du silence est défaite à l'instant**
(la cible revient où le lien l'avait laissée, une coupe de vraie congestion
antérieure reste) au lieu de remonter en cinq rapports. Couper
vite et remonter lentement : une coupe coûte de la netteté une seconde, un
débordement coûte la main du joueur.

**Trois couches** dans la boucle de session, et un seul `setBitrate()` : le
plafond du joueur (le réglage, déplacé par l'échelle du front via
`setTargetBitrate`) → ce que le lien prend (le gouverneur ; c'est le débit du
fil, et celui du modèle de lien `LinkOccupancy`) → le budget par image de la
cadence réelle (§9.8), et la rafale de raffinement par-dessus.

**Le front s'efface** : `_onStreamCongested` ne reconstruit plus la session
pour l'hôte natif. Relancer pour −30 % de débit vingt secondes après le premier
signe jetterait une image que l'hôte adapte déjà en 500 ms. L'échelle garde ses
autres hôtes.

Vérifié en LAN (04/09) : « link reports flowing from the receiver (first:
owdRise 0 ms, gaps 0) », gouverneur muet, 6,1 ms. La réaction en vraie
congestion se lit dans le log — « [native] link: delay rising — encoding at
32000 kbps of the 40000 set » — et **reste à observer sur un lien qui
souffre** (4G, hôtel).

### 9.10 Une image perdue se répare par un delta (04/09/2026)

Le §9.2 promis depuis le début. Jusqu'ici un trou dans la numérotation côté
client fermait la porte aux deltas et réclamait une IDR : ~70 Ko d'un coup sur
un lien déjà en peine, et une image figée le temps de l'aller-retour.

NVENC garde désormais un **DPB de quatre images** (`maxNumRefFrames` /
`maxNumRefFramesInDPB`, les trois codecs) et chaque image est estampillée de
**son propre numéro** (`inputTimeStamp = frameNumber`). Quand le récepteur nomme
celle qu'il n'a pas reçue, `NvEncInvalidateRefFrames` la retire des références
et l'image suivante est prédite depuis celles que le récepteur possède : un
delta ordinaire, le flux se répare sans rien de plus gros. Le DPB est un repli,
pas une recherche plus large — l'encodeur prédit toujours depuis la dernière
image, et le temps d'encode ne bouge pas : mesuré A/B au banc (`dpb=1` contre 4,
clip FPS 1440p, 40 Mbit/s, deux passes chacun) 3,69 / 3,79 ms contre
3,75 / 3,77 ms, même taille, même QP.

Le chemin : `/start` répond `ref_invalidation` quand l'encodeur de la session le
fait vraiment (`SessionInfo::referenceInvalidation`, faux sur oneVPL) ; sur un
trou de numérotation le client envoie `invalidateref {from, to}` (ids de fil) et
**continue de décoder** au lieu de jeter les deltas jusqu'à la keyframe ; le
relais DC traduit les ids de fil en numéros de moteur — un anneau des 512
derniers, parce que les deux divergent à chaque image que le relais jette avant
d'attribuer un id — et appelle `Session::invalidateReference` ; la session le
garde pour le thread de capture, qui le dit à l'encodeur juste avant la
prochaine image. Un trou plus large que le DPB (16 ids), un id oublié, un
encodeur sans la fonction : keyframe comme avant, le récepteur a toujours une
réparation. Un décodeur qui n'accepterait pas de décoder par-dessus le trou
tombe dans `_handleDecoderError`, qui demande une keyframe.

Vérifié avec le crochet de debug `localStorage.mw_drop_test = N` (le client
jette un delta sur N) : « Frame gap: lost 120..120 — naming them to the host,
decoding on », « reference invalidated: frame 222 never reached the receiver,
healing with a delta », aucune IDR demandée, aucune erreur de décodeur, 53
images/s et 5,2 ms pendant l'exercice (NVENC).

### 9.10.1 AMF : la même réparation par références long terme (06/09/2026)

⚠️ **plan corrigé** : le plan disait « pas d'équivalent AMF (AMF n'a pas
d'appel) ». AMF n'a en effet pas le `NvEncInvalidateRefFrames` de NVIDIA, mais
il a les **références long terme** (LTR) — de quoi faire la même réparation par
l'autre bout. Au lieu de *retirer* l'image perdue, on *nomme une survivante* :
les images sont marquées dans des slots LTR au fil de l'encodage
(`MarkCurrentWithLTRIndex`), et quand le récepteur nomme une perte, l'image
suivante est forcée à ne prédire que du slot le plus récent *antérieur* à la
perte (`ForceLTRReferenceBitfield`) ; en mode `RESET_UNUSED`, le pilote lâche
les slots non nommés — précisément ceux qui portaient les images gâtées. Le flux
est propre à partir de cette image, sans rien de plus gros qu'un delta.

`ReferenceSlots` (`encode/ReferenceSlots.h`, pur, 37 checks) tient
l'arithmétique : quel slot marquer, quel slot est propre avant une perte,
lesquels oublier. **Portée** : 4 slots marqués à chaque image ne reculent que de
3 images — 18 ms à 165 fps, moins qu'un aller-retour Internet. Donc on marque
tous les `stride` images, le pas choisi pour que les slots couvrent ≥ 125 ms
quelle que soit la cadence (60 fps → pas 2 → 8 images ; 165 → pas 6 → 24). Le
prix est que la référence forcée peut être de `stride` images plus vieille que
la dernière propre — un delta un peu plus gros, une fois, au lieu d'une keyframe.
Ce que la table enregistre est ce que le **buffer de sortie confirme** avoir été
marqué, jamais ce qui a été demandé : un pilote qui ignore le marquage
(`MarkedLTRIndex` absent) ou la référence forcée (`ReferencedLTRIndexBitfield`
sans le bit) dégrade en keyframe et le dit, il ne fabrique pas une table fausse.

⚠️ **Deux corrections du 06/09, trouvées en observant la réparation en vrai.**

**Le sentinelle `-1` de `MarkedLTRIndex` arrive en 32 bits.** La documentation
dit « default = -1 » ; le pilote le range dans 32 bits et la propriété rend
**4294967295**. Le test `markedIdx >= 0` lisait donc « je n'ai pas marqué »
comme le slot quatre milliards, appelait `marked()` avec un cast qui retombe sur
−1, et la table gardait **un trou là où elle croyait avoir une référence**. Le
trou est invisible jusqu'à une perte, où la réparation nomme un slot que le
pilote n'a jamais rempli. Tout ce qui sort des slots accordés est un refus,
quelle que soit sa forme binaire — et le refus est dit avec la valeur reçue.

**Une référence se juge sur l'image, pas sur l'index.** Le pilote référence
souvent un autre slot que celui demandé, et il a le droit : ce qui rend une
référence propre est **l'image qu'elle porte**, pas son numéro. `allBefore()`
répond « toutes les images nommées précèdent-elles la perte ? » ; `describe()`
nomme les images derrière un bitfield, pour que le log soit lisible. Quand la
réponse est oui, c'est une réparation, même si ce n'est pas le slot demandé.
Quand c'est non — une image *postérieure* à la perte, ou aucune référence long
terme — le delta prédit de ce que le récepteur n'a pas, et **l'image suivante
est forcée en keyframe** : c'est ce que le design promettait et que le code ne
faisait pas (il se contentait d'un avertissement, une fois).

Vérifié sur la RX 7600 réelle le 06/09 (les trois codecs) : « AMF ready : … 4
LTR slots every N frames with reference invalidation (reach M frames) », et
`dpb=1` (le « avant » du banc) éteint proprement les slots (« no reference
invalidation »). Coût mesuré nul (bench §8c, point 5).

✅ **Réparation observée en vrai le 06/09** (l'angle mort du banc est levé : le
Chrome piloté décode ce flux depuis le correctif des paramètres AMF, §9.10.3, et
`mw_drop_test` s'arme donc). Flux HEVC AMF, un delta jeté toutes les 60 images,
**12 pertes** : le client les nomme et continue de décoder — *zéro* « Requesting
IDR », *zéro* erreur de décodeur, image nette de bout en bout. Sur l'hôte,
**une perte sur deux est réparée par un delta** :

```
AMF healed frame 71 with a delta from long-term slot 2 = frame 68
                                  (driver referenced slot 1 = frame 66)
```

et l'autre moitié dégrade en keyframe, avec sa raison :

```
AMF ignored the forced long-term reference (asked slot 3 = frame 126,
    referenced slot 0 = never marked, slot 1 = frame 130) for a loss at 129
```

✅ **L'alternance avait une cause, traitée le 06/09 : le pilote se réserve
l'index long terme 0.** Première hypothèse — « il ne marque pas la keyframe » —
**fausse** : en laissant parler l'avertissement cinq fois, les refus tombent sur
les frames 0, 8, 16, 24, 32 — **toutes celles qui demandaient l'index 0**,
keyframes comme deltas. Le pilote répond −1 à toute demande de marquage en 0, et
référence cet index de lui-même quand une image est forcée ailleurs (c'est le
« slot 0 = never marked » de chaque dégradation). La table ne lui parle donc
plus qu'en indices **1..N** (`kLtrReservedIndices`) : slot *s* de la table =
index *s* + 1 pour le pilote, un index de plus demandé à l'init pour garder
quatre places utiles (5 accordés → 4 slots), bitfield rapporté décalé d'un cran
avant jugement — le bit 0 du pilote tombe, et s'il a référencé cela seul, le
bitfield vide vaut « rien à garantir », donc keyframe plutôt qu'un pari sur ce
qu'il garde là. Rejoué : **7 pertes, 7 réparations par delta**, aucun refus de
marquage, le slot 0 de la table (index 1) porte enfin des images (« driver
referenced slot 0 = frame 128 »), zéro IDR, zéro erreur de décodeur.

### 9.10.2 L'éviction du FrameSender nomme enfin l'image jetée (06/09/2026)

Le §9.10 laissait un reste : quand l'émetteur (`FrameSender`) évince un delta de
sa file parce que le lien est plein, il « ne dit pas lequel il a jeté » et le
relais demandait donc une keyframe (ou, en intra-refresh, laissait le client
nommer le trou un aller-retour plus tard). L'émetteur **est** pourtant la seule
partie qui connaît le numéro de l'image jetée avec certitude. `enqueue` /
`enqueueFragments` prennent maintenant un `std::vector<uint32_t>* evicted`
optionnel, rempli du `frameNumber` de chaque delta écarté (dans les deux chemins
d'éviction : le plafond dur et la profondeur 1 du natif). Le relais, pour un
moteur qui répare par invalidation (`referenceInvalidation()` vrai), les passe
aussitôt à `invalidateReference` — un aller-retour **avant** que le récepteur ne
voie le trou et le nomme lui-même ; la session traduit un refus en keyframe
comme toujours. Pour tout autre moteur, le comportement d'avant est intact
(demande de keyframe sauf ride-out), garanti par le drapeau `nameEvictions`.
Ceci vaut pour NVENC comme pour AMF depuis que ce dernier a l'invalidation.

**Le même geste au calage du lien, derrière un interrupteur (29/09/2026, §9-25
du plan D3D12).** Il restait une image jetée sans nom : celle que le relais
écarte quand le lien ne se vide plus (`SendBacklog`, « Dropped delta frame (link
not draining) »). Elle part **avant** d'avoir reçu un id de fil, donc le client
ne voit aucun trou.
- En ride-out, le delta suivant prédit de cette image. VideoToolbox refuse ce
  delta orphelin (d'où `50b2b574` : Apple ne demande plus le ride-out).
  Windows le rapièce jusqu'au passage de la vague d'intra-refresh.
- Sans ride-out, le relais jette tout jusqu'à une keyframe demandée au
  dégorgement.

Avec `namedrops=1` (`MW_NATIVE_TUNING`, clé lue par le relais par
`NativeMediaEngine::nameLinkDrops`), un moteur qui répare par invalidation
reçoit le numéro de l'image jetée à l'instant. Le delta suivant prédit d'une
image que le client a déjà : ni keyframe, ni vague à attendre. Un calage plus
long que la portée des références finit en keyframe, que la session demande
d'elle-même.

En envoi direct, le relais tourne sur le fil de capture. L'invalidation
précède donc l'encodage de l'image suivante, et aucun delta déjà codé ne
prédit de l'image jetée. Avec `pipelined=1`, une image peut déjà être codée
quand la remise arrive : ce cas n'est pas couvert. Le compteur
`sctpDeltaNamed` de la ligne « Drop counters » compte ces images nommées.

Le défaut dépend de l'encodeur, d'après les bancs ci-dessous.

**Le banc Linux, fait le 29/09** (banc §8n.27 : Chrome de l'UM790Pro, coupures
et bridages par `netem`, un détecteur de dégâts lu sur le canvas du stream) :
- NVENC et AMF perdent 93 à 96 % de leurs images abîmées. Presque toutes
  venaient des bridages du lien, et les gels ne changent pas.
- D3D12 VE, qui attend déjà des images clés, y gagne un peu.
- **oneVPL se bloque** : après des réparations enchaînées depuis la même
  référence longue, un encodage ne se termine jamais, et la session finit
  10 s plus tard. Trois passes sur trois, jamais vu en production.
  `namedrops` ne doit donc jamais valoir pour oneVPL.
- **Cause trouvée le même jour** (banc §8n.30, reproduit hors ligne) : une
  réparation par référence longue **pendant une vague d'intra-refresh** bloque
  l'encodeur HEVC d'Intel. Vagues bout à bout, la première réparation suffit ;
  sans vague, 176 réparations passent. H.264 et AV1 ne se bloquent pas. Les
  pertes que le client signale prennent le même chemin : la v0.3.1 y est
  exposée sur Intel.
- **Corrigé le même jour** (option A de Bruno, §21.6b) : en HEVC sous
  intra-refresh, oneVPL ne répare plus par référence longue, et chaque perte
  coûte une image clé. Rejoué sur le banc du §8n.30 : plus aucun blocage.

**Les bancs Windows et Mac, faits le 29/09** (banc §8n.28, le lien bridé côté
hôte par WinDivert) :
- Chrome sous Windows (N95) confirme Linux : NVENC perd 85 % de ses images
  abîmées, AMF 46 %, D3D12 VE ne bouge pas, et les gels non plus.
- Sur le Mac, `namedrops` est neutre. VideoToolbox n'affiche pas d'image fausse,
  et l'image clé évitée coûte peu en LAN : sur NVENC, deux fois moins d'images
  clés demandées, pour les mêmes gels.

**Le défaut, décidé par Bruno le 29/09 : NVENC et AMF en D3D11**, là où les
bancs montrent un gain (`nameLinkDropsByDefault`, `EncoderTuning.h`).

**NVENC en entrée D3D12 le prend aussi, le même jour** (banc §8n.29) : sur le
Chrome du N95, il perd 87 % de ses images abîmées, comme NVENC en D3D11. C'est
la chaîne que le réglage D3D12 donne à une carte NVIDIA. La règle lit donc
l'encodeur de la route D3D12 (`SessionInfo::videoEncoder12`) : sur une carte
NVIDIA, D3D12 Video Encode reste sans.

Ailleurs, rien ne change :
- oneVPL, jamais : son HEVC s'y bloquait, et il ne répare plus du tout sous
  intra-refresh (§21.6b) ;
- D3D12 Video Encode : neutre ;
- AMF en entrée D3D12 : neutre au banc §8n.29, et seule une clé de banc le
  fait tourner ;
- VA-API et Vulkan Video sous Linux : pas mesurés ; VideoToolbox n'a pas
  d'invalidation.

La clé l'emporte sur le défaut : `namedrops=0` l'éteint, `namedrops=1` l'allume
ailleurs. Le relais lit la chaîne qui tourne (`NativeMediaEngine::nameLinkDrops`) :
une chaîne D3D12 retombée en D3D11 en cours de session suit la règle de D3D11.
Quand le nommage est actif, la ligne « streaming » du journal de session porte
`[link drops named]`.

### 9.11 La cadence s'aligne sur le rafraîchissement du client (04/09/2026)

Un client qui peint sur son vsync — *tearing* coupé, ou un navigateur qui ne
sait pas déchirer — affiche au plus **une image par rafraîchissement**, et
seulement au rafraîchissement. Un stream 60 sur un écran client 144 Hz tombe
alors sur des tics espacés de 2,4 rafraîchissements : certaines images tiennent
deux rafraîchissements, d'autres trois, et l'œil lit l'alternance comme un
à-coup alors qu'aucune image n'a été perdue. Le même stream sur 120 Hz est
parfaitement régulier — 60 divise 120.

Donc quand le client peint sur son vsync, le stream tourne à un **diviseur
entier du rafraîchissement client** plutôt qu'au réglage exact, dans une fenêtre
de ±20 % autour de lui : 144 Hz et 60 réglés donnent **72** (un rafraîchissement
sur deux), 165 Hz donne **55** (un sur trois), 120 Hz garde 60 (un sur deux).
Le réglage est un vœu sur la fluidité et le budget du lien, pas un contrat ; une
cadence à un cinquième de lui qui tombe sur la grille du client est l'image la
plus lisse pour le même coût. Le diviseur le plus proche gagne, le plus rapide
en cas d'égalité (72 plutôt que 48 pour 60 sur 144 Hz : un joueur qui a demandé
60 et peut avoir 72 pour la même fluidité est mieux servi).

`AlignedCadence` (`core/CadenceAlign.h`, pur) fait ce choix. La grille de
`FrameCadence` est tenue en **nanosecondes** : 55 fps sur 165 Hz, c'est trois
périodes de 6060,6 µs = 18181,8 µs, et une grille tronquée au microseconde
glisserait d'une image toutes les deux minutes (mesuré en test sur 99 000
présents : zéro dérive, chaque écart de trois présents exactement). Le budget
de l'encodeur suit par `EffectiveCadence::retarget()` — l'encodeur garde le
débit par image pour lequel il a été construit, et le débit transmis est
multiplié à l'instant où la grille change, jamais une seconde d'images
sur-dimensionnées pendant qu'elle passe de 60 à 72.

**Quand la règle ne s'applique pas**, la garde de cadence ordinaire (§9.6)
reprend telle quelle : le client *déchire* (Chromium desktop, le défaut) — il
n'y a pas de grille contre laquelle battre, l'image est peinte dès qu'elle est
décodée, et le réglage brut est la plus basse latence ; le réglage est 0 (le
rythme de l'hôte) ; aucun diviseur ne tombe dans la fenêtre (le réglage tient) ;
ou le diviseur demande plus que l'écran hôte ne produit (hôte 60 Hz, client
144 Hz, 60 réglés → 72 : l'hôte n'a pas 72 présents à donner, le réglage tient).

**Le chemin.** Le client mesure son rafraîchissement par `requestAnimationFrame`
(`util/RefreshRate.js`) : la **moyenne** des bons deltas donne la période au
dixième de pour-cent (les horodatages sont grossis à la milliseconde sans
isolation cross-origin, donc un delta seul ne distingue pas 165 de 166,7 ; une
médiane trie d'abord les images que le navigateur a sautées). La valeur part
dans `/start` (`client_refresh_mhz`, `client_vsync = !tearing`) ; l'hôte choisit
la cadence à l'ouverture et la journalise (« 55 fps stream for a 165 Hz client
presenting on vsync (60 set, every 3rd refresh) »). Quand la fenêtre change
d'écran en cours de stream, le nouveau taux part en `clientrefresh {mhz, vsync}`
sur le canal d'input et l'hôte re-choisit **entre deux images**, sans rien
relancer (`Session::setClientRefresh`, natif seulement, `qobject_cast` sur les
deux relais). Le message est traité comme `framefloor` : accepté par
`InputPolicy`, inerte pour tout host GameStream.

Vérifié le 04/09 sur l'instance dev (client Chrome dédié, *tearing* coupé, écran
165 Hz) : `/start` porte 164 972 mHz, l'hôte ouvre à **55 fps, un
rafraîchissement sur trois**, NVENC configuré `2560x1440@55`, budget par image
tenu ; fin de session 4 771 présents, 797 non portés, arrêt propre. **Limite
mesurée** : Chrome garde son horloge `requestAnimationFrame` à 165 Hz même
lorsque la fenêtre est physiquement sur l'écran 60 Hz, donc le re-choix en cours
de session ne se déclenche pas sur cette machine — le navigateur ne distingue
pas le taux par moniteur. Le chemin de re-choix est couvert par test unitaire
(`retarget`) ; sa démonstration en vrai demande un client qui rapporte
effectivement deux taux (à voir chez Bruno, multi-moniteurs de fréquences
différentes).

### 9.12 Gels et récupération côté navigateur (04/09/2026)

Ce que le navigateur fait quand l'image s'arrête ou se casse a été écrit pour
Sunshine : une keyframe demandée à chaque doute, parce que c'était la seule
réparation qui existait. Avec l'hôte natif, §9.10 en a apporté une autre — la
perte nommée, réparée par un delta — et les cinq réflexes ci-dessous ont été
relus à cette lumière. La règle : **le chemin GameStream ne change pas d'un
octet** ; tout ce qui suit est conditionné à `ref_invalidation` (la réponse
`/start`) ou à l'hôte natif.

**Famine (G1).** Le transport déclarait une famine après 1 s sans image et
demandait une IDR. Pour l'hôte natif, cette demande est toujours de trop : une
image qui n'est pas venue n'est pas une image perdue. Si des images ont été
jetées en route, la première qui arrive porte un trou de `frameId` et le
gestionnaire de trous les nomme à l'hôte (§9.10) ; si aucune ne l'a été, le flux
reprend simplement. Dans les deux cas la keyframe aurait été la plus grosse image
qui soit, envoyée sur un lien qui vient de prouver qu'il souffre. Donc, quand
l'hôte répare par invalidation, la famine **compte et chronomètre** (`stalls`,
ligne « Stream resumed after N ms ») et **ne demande rien**. ⚠️ Le plan
prévoyait un seuil adaptatif de « 4 intervalles d'image » (67 ms à 60 fps) :
**impossible**. Un écran fixe se tait légitimement — le plancher de vivacité de
l'hôte est à **500 ms** (§9.1, mesuré : 2 fps dans l'overlay sur un bureau
immobile), Sunshine n'envoie rien du tout — et un seuil de 67 ms lirait chaque
bureau au repos comme un flux mort. La seconde est la marge au-dessus de ce
plancher, et c'est sur elle que l'hôte a dimensionné son plancher.

Deux autres demandes du transport tombent sous la même règle sur l'hôte natif :
l'image incomplète et l'image périmée (`FRAME_TIMEOUT_MS`). Et un **bug** trouvé
à la lecture : `onFrameLoss` invalidait la référence côté vue **sans condition**,
donc sur l'hôte natif une image dont un fragment manquait était d'abord nommée à
l'hôte et réparée par un delta (§9.10), puis, 500 ms plus tard, le nettoyage des
images périmées la déclarait perdue, invalidait la référence, jetait tous les
deltas et demandait la keyframe que la réparation venait d'éviter. Sur l'hôte
natif `onFrameLoss` ne fait plus rien : le trou de `frameId` est la seule voie.
Et comme le canal vidéo est **ordonné** (`unordered = false`, 3 retransmissions),
une image qu'une plus récente a dépassée ne se complétera jamais : elle est
déclarée perdue **à l'instant** où la suivante s'assemble, au lieu d'attendre les
500 ms de l'horloge — c'est le temps pendant lequel l'image restait en retard
d'une image qu'elle aurait pu montrer.

**Ride-out (G2).** Le chien de garde qui laisse la vague d'intra-refresh
travailler avant de réclamer une keyframe comptait **2,5 s** d'horloge. Or la
vague avance **par image encodée** : `intraRefreshPeriodFrames` vaut 2 s de
la cadence de l'encodeur, mais un jeu qui présente à 30 sous un stream 165
étire une période de 330 images sur **11 s**, et l'horloge de 2,5 s l'aurait
déclarée en échec quatre fois de suite. `/start` porte désormais
`intra_refresh_frames` (`SessionInfo::intraRefreshFrames`, le nombre exact que
les trois encodeurs ont reçu) et le chien de garde compte les **images reçues**
contre 1,2 période — les images non reçues ont fait avancer la vague aussi, mais
ne compter que les siennes est le côté prudent — avec l'horloge gardée en
garde-fou (15 s) pour un flux qui s'arrête. Le message « Ride-out did not
recover » devient un compteur (`rideOutFailed`), montré dans le détail de
l'overlay avec les deux autres. Sans période connue (hôte qui ne la dit pas),
l'horloge de 2,5 s reste seule.

**Erreur du décodeur (G3).** `VideoDecoder` est à usage unique : sur erreur,
la vue le ferme, en crée un autre, remet le parseur NAL à zéro et demande une
keyframe. Relu : rien n'est à garder — les deltas en attente prédisent depuis
une référence que le nouveau décodeur n'a pas, et le parseur remis à zéro se
reconfigure à l'arrivée de la keyframe, qui porte ses SPS/PPS de toute façon.
Ce qui manquait, c'est la **mesure** : la récupération est chronométrée de
l'erreur à la première image que le nouveau décodeur sort, et journalisée avec
les images jetées entre-temps (« Decoder recovered in N ms, M frames dropped
meanwhile »), sur le thread principal comme dans le worker ; compteur
`recoveries`, même ligne d'overlay.

**Garde keyframe (G4).** Côté hôte, `sendBufferedKeyframe` ne jette une
keyframe tamponnée que si une plus récente est **déjà partie** — la mémoire
`webrtc-media-freeze-diagnosis` est respectée telle quelle. Côté natif, une
chose changeait : une keyframe demandée (`requestKeyframe`) n'était encodée qu'au
prochain `emit`, et sur un **écran fixe** le prochain `emit` est le tic du
plancher — jusqu'à **500 ms** pendant lesquelles un navigateur en récupération,
ou qui vient d'ouvrir son canal vidéo, regardait le vide alors que l'image
convertie attendait. La boucle encode maintenant **à l'instant** une keyframe
demandée sur un écran immobile (même image que le plancher aurait envoyée, un
encodage). C'est ce que Sunshine ne peut pas faire (§9.1 : il n'encode que sur
dommage) et que l'hôte natif peut.

**Onglet caché, retour (G5).** Mesuré le 04/09 sur l'instance dev : un onglet
**caché** continue de décoder — seuls ses timers passent à une seconde,
`linkstats` compris, ce qui reste sous les 4 s de silence que le gouverneur
(§9.9) lit comme un lien mort — et il n'y a rien à réparer. Une page **gelée**
(onglet d'arrière-plan sur téléphone, ou Chrome qui gèle un onglet caché depuis
longtemps ; `Page.setWebLifecycleState frozen` au banc) est différente : le
gouverneur coupe à 16 000 kbps pour « no report from the receiver » et remonte
en cinq rapports, et au dégel les images qui attendaient dans le canal arrivent
en rafale. Toute mesure qui lit un temps d'arrivée prendrait cette rafale pour
le fait du lien — la référence de délai aller simple lirait des secondes de
« montée » et l'hôte couperait son débit au moment même où le spectateur
revient ; le détecteur d'arrêts périodiques marquerait un événement ; le pacer
épinglerait sa réserve. Au retour (`visibilitychange` → `_resyncAfterHidden`)
ils sont ré-amorcés, le transport apprend que le silence était le nôtre, et les
manettes — dont la scrutation `requestAnimationFrame` s'est arrêtée avec
l'onglet, pendant que le chien de garde de l'hôte centrait ce qu'il n'entendait
plus — sont **redites une fois**, au repos ou non (`GamepadManager.resendAll`).
Les images que l'hôte a évincées pendant l'absence arrivent comme un trou de
`frameId` et prennent le chemin ordinaire : nommées, réparées par un delta.
Rien n'est demandé au retour — sauf, depuis le 05/09, que le premier
`linkstats` porte `resumed` (§9.3) : le gouverneur ne lit pas ses trous comme
ceux du lien et défait à l'instant la coupe que notre silence lui avait fait
faire, au lieu de la remonter en cinq rapports.

### 9.13 Le plafond d'Auto et le budget de pixels (20/09/2026)

Une résolution et une cadence sont choisies séparément et se **multiplient** :
2560×1440 à 120 fps, ce sont 442 millions de pixels par seconde à capturer,
réduire, convertir, encoder, transmettre, décoder et peindre — plus que ce que
l'une ou l'autre des deux machines tient une soirée de jeu. Trois règles, toutes
côté client (`util/StreamResolution.js`, `util/RefreshRate.js`), tiennent ce
produit :

1. **Résolution Auto : plafond à 1440 lignes** (`AUTO_MAX_HEIGHT`). La boîte
   d'Auto est cet écran-ci **à sa forme**, ramené à 1440 lignes quand il en a
   plus : un 4K demande 2560×1440. Au-delà, le gain est un détail qu'on voit en
   se penchant et le coût est payé deux fois, à l'encodage et au décodage. Le
   plafond ne s'applique qu'à Auto : « Match my screen » et « Custom » sont le
   mot du joueur et ne sont jamais bridés. L'écran virtuel créé à la demande
   suit la même règle sous Auto — le fabriquer en 4K pour ensuite streamer du
   1440p ne ferait que donner trois millions de pixels à jeter au compositeur.

2. **Cadence Auto : plafond à 120 fps** (`AUTO_FPS_MAX`). Au-delà de 120, la
   fluidité gagnée s'achète à une image toutes les 8 ms sur toute la chaîne, et
   un choix fait **à la place** du joueur ne dépense pas ça. Le joueur qui veut
   nourrir son 165 Hz le prend par son nom dans la liste. La ligne des Réglages
   le dit quand le plafond mord (« Auto (120 FPS, votre écran à 165 Hz est
   bridé) ») : un libellé qui annonce 165 pour streamer 120 serait un mensonge.
   La mesure **brute** continue de partir à l'hôte (`client_refresh_mhz`) : la
   grille sur laquelle les images atterrissent reste celle de la dalle.

3. **Budget de 250 Mpx/s dès que l'un des deux est en Auto**
   (`PIXEL_RATE_BUDGET`, `fitPixelBudget`). 1920×1080 à 120 fps, c'est 249
   millions : la référence sur laquelle le nombre a été lu. Au-dessus, **la
   résolution paie d'abord** — une cadence se sent à chaque mouvement de souris,
   cent lignes de résolution presque jamais — jusqu'à un plancher de **1080p**,
   puis la cadence, jusqu'à un plancher de **60 fps**. Aucun des deux planchers
   ne descend sous ce que fait l'écran du client lui-même : un stream déjà plus
   petit que l'écran où il atterrit n'a plus rien à donner. Une paire encore
   au-dessus du budget aux deux planchers passe : les planchers sont la
   promesse, le budget est la visée. Deux choix explicites (résolution **et**
   cadence nommées) ne sont jamais touchés — c'est l'affaire du joueur.

   Ainsi un 4K 144 Hz en Auto/Auto demande **1920×1080 à 120** ; un ultra-large
   2560×1080 en Auto demande 2560×1080 à **90** (la résolution étant déjà au
   plancher, c'est la cadence qui paie) ; un 3440×1440 en Auto/Auto demande
   2580×1080 à **89** ; et 1080p120 ne bouge pas.

**Le côté hôte.** Un plafond n'est pas un vœu, et l'alignement de cadence
(§9.11) est libre de monter de 20 % : un client vsync à 144 Hz à qui Auto
demande 120 se verrait servir 144, ce qui remettrait un cinquième du budget.
Le client dit donc son plafond dans `/start` (`stream_fps_max` →
`SessionConfig::maxFps`), et `alignCadence()` ne retient plus un diviseur
au-dessus de lui. Il se lit avec le plafond vivant du décodeur
(`setClientFpsCap`) : le plus petit des deux gagne. Une cadence **nommée** par
le joueur n'envoie aucun plafond — là, les 72 pour 60 valent leurs 20 %. Et
l'écran virtuel est créé à la cadence **du stream** et non à la mesure brute :
une dalle à 144 Hz qui streame un Auto à 120 laissait le compositeur présenter
24 images par seconde dans le vide.

---

## 10. Host natif dans l'UI

- **Aucun pairing** : ni PIN, ni certificat, ni association. Il n'y a pas deux
  parties à authentifier.
- **Aucun appel réseau** au lancement : `launch()` renvoie un descripteur
  nommant un display.
- **Les displays SONT la liste d'apps** : une carte par écran, titrée du seul
  numéro que montrent les réglages Windows (`Display 1`) — le modèle du moniteur
  et le mode restent dans le log de sonde, pas sur la carte. La grille existante
  devient le sélecteur, zéro nouvelle UI, et avec un seul écran c'est une carte
  et un clic.
- Host nommé `<hostname> — MoonlightWeb Host`, non persisté (recalculé au
  démarrage d'après ce que la machine sait faire).

---

## 11. Repli vers Sunshine

`probe()` répond `unavailable` avec une raison machine — jamais affichée telle
quelle — dans exactement ces cas :

| Condition | Détail |
|---|---|
| Pas d'API de capture | DDA **et** WGC échouent |
| Aucun display attaché | machine headless — depuis le 18/09/2026 la carte reste, avec « MoonlightWeb Virtual Display » comme unique app (Windows : VDD by MTT embarqué dans l'installeur, nœud nommé et désactivé par défaut, tâche élevée `--vdisplay-apply` pour l'allumer/éteindre ; macOS : `CGVirtualDisplay` créé par le process serveur, `mw::native::vdisplay` ; Linux Wayland, depuis le 01/10/2026 : créé par le compositeur au démarrage du stream, Mutter sous GNOME, KWin sous KDE Plasma 6, §35) ; ouvrir la carte allume l'écran, le rend primaire et le diffuse ; voir `backend/src/backend/VirtualDisplay.h` |
| Pas d'encodeur utilisable | aucun GPU avec encodeur **et** codec |
| Pas de session interactive | service Windows en session 0 |
| OS trop ancien | Windows < 10 2004 |
| Architecture non supportée | Windows ARM64 en v1 |

Message unique, non technique :

> « Le streaming natif MoonlightWeb n'est pas disponible sur cette
> configuration. Installez Sunshine pour utiliser cette machine comme host. »

Sur la page Hosts, ce message n'apparaît **que** si aucun host n'est visible
**et** le natif est indisponible.

---

## 12. Licences

| Dépendance | Licence | Commercial | Note |
|---|---|---|---|
| nv-codec-headers (NVENC) | MIT (notice NVIDIA sur l'en-tête) | OK | rien du SDK n'est redistribué ; `nvEncodeAPI64.dll` vient du pilote |
| SDK Windows (DXGI, D3D11, WASAPI) | licence SDK | OK | — |
| AMF, oneVPL | MIT | OK | en-têtes seulement, les runtimes viennent des pilotes |
| **ViGEmClient** (manette) | **MIT** | OK | ⚠️ noté BSD-3 dans le plan d'origine — c'est faux, l'amont livre du MIT. Vendoré tel quel en v1.16.18.0, jamais modifié |
| ViGEmBus (le pilote) | BSD-3 | OK | **pas redistribué** : installé par l'installeur depuis l'amont |
| **libopus** (audio) | **BSD-3** | OK | livré le 04/09/2026 : sous-module `native-host/third_party/opus` épinglé v1.5.2, bibliothèque statique, sans programmes ni tests ni installation |
| libva, libdrm, EGL/GLES, GBM | MIT | OK | livrés le 05/09/2026 (§19), liés dynamiquement, trouvés par pkg-config |
| **libpipewire-0.3** (audio Linux) | **MIT** | OK | livré le 05/09/2026 (§19.7) ; **libpulse écarté** (LGPL) — d'où la limite « PipeWire doit être le serveur audio » |
| OpenH264 | BSD | OK | à venir (repli logiciel, pas encore nécessaire) |
| ❌ FFmpeg / libavcodec | LGPL/GPL | **écarté** | Sunshine l'utilise (156 appels `av_*`) ; nous non |
| ❌ x264 / x265 | GPL-2.0 | **interdit** | — |
| ❌ moonlight-common-c | GPL-3.0 | **jamais lié au module** | — |

**Brevets codec** — à arbitrer à la commercialisation, pas maintenant : AVC
(Via LA), HEVC (Access Advance + Via LA, le plus complexe), **AV1 (AOMedia,
libre de redevance)**. D'où la préférence AV1 quand les deux bouts suivent.

---

## 13. État

Tableau remis à jour le 29/09/2026, à la clôture du plan D3D12 (§32.19).

| Livré et mesuré | Reste |
|---|---|
| Module isolé + garde de licence | |
| **Audio** : WASAPI loopback → cadenceur 5 ms → libopus, thread « Pro Audio » (04/09/2026) | |
| `IMediaEngine`, relais découplés | |
| Sonde displays/GPU + association ; WGC en repli, et retour à DDA dès que le bureau de l'utilisateur revient (§32.10) | |
| Capture DXGI (0,06 ms) ; worker SYSTEM pour le bureau sécurisé (§31) ; Ctrl+Alt+Suppr appuyé par le service lanceur, sous la stratégie que l'installeur pose ; un stream qui démarre sur l'écran de sécurité ; l'invite UAC cliquée depuis le client ; les gestes de Bruno depuis son iPhone, poste verrouillé compris (§31.7) | |
| Conversion NV12 + AYUV 4:4:4 ; HDR (P010 + BT.2020 PQ) | |
| NVENC (3,46 ms), AMF (3,70 ms), oneVPL (mesuré sur l'Arc et le N95) | |
| **Chaîne D3D12** (§32) : conversion D3D12, D3D12 Video Encode en HEVC, H.264 et AV1, contrôle de débit maison ; **par défaut sur Intel** (§32.9) ; NVENC et AMF en entrée D3D12 derrière le réglage (§32.12) ; l'encodeur gardé à travers un redémarrage de capture (§32.18) et deux images en vol sur un GPU Intel à mémoire propre (§32.20), par défaut depuis le 29/09 | NVIDIA et AMD restent en D3D11, plus rapides chez eux |
| Intra-refresh sur les trois encodeurs + ride-out client (sauf plateformes Apple) | |
| Curseur composé, plancher sur écran immobile choisi par le client (§9.1) | |
| Copie inter-GPU (§5) : un écran dont le GPU n'encode pas streame quand même ; la relecture passe par la file COPY depuis le 21/09 | |
| Six étapes mesurées par frame, p95/p99 dans les stats et le log (§4, point 4) ; temps GPU de la conversion et de l'encodeur au banc (`gputiming=1`) | |
| Clavier/souris (`SendInput`), manette (ViGEm) + rumble | |
| Installeur : ViGEmBus en silencieux ; plus de Sunshine | |
| Banc de shaders de réduction `mw-scaler-bench` + passe Lanczos-2 séparable en lumière linéaire dans `ColorConvert`/`GlConvert`, letterbox, `MW_SCALER` (§28, 17/09/2026) ; ce qu'elle coûte, GPU par GPU (§8n.25 du banc) | L'A/B sur un vrai flux par encodeur (`MW_SCALER`), le visuel client 1:1 |
| **Linux** : KMS ou portail → GL → VA-API ; route scindée Vulkan compute → VA-API par défaut sur AMD (§32.8) ; chaîne Vulkan Video derrière le réglage, prise sur la preuve au pixel (§32.7) ; OpenH264 en repli | G5 : Counter-Strike 2 et l'endurance sous un jeu ; NVIDIA et Intel sous Linux, quand leurs cartes seront sur le banc |
| **macOS** : ScreenCaptureKit → VideoToolbox | |

**Le chemin est complet côté serveur**, et jouable : le host natif apparaît sans
pairing, un clic sur un écran construit un `NativeMediaEngine` qui alimente le
relais WebRTC existant, et clavier/souris/manette reviennent par le
DataChannel d'entrée. Le chemin Sunshine/Wolf/MultiSeat est intact.

**L'audio est là depuis le 04/09/2026** (`src/audio/`) : la sortie par défaut de
l'hôte, capturée en loopback WASAPI, cadencée à une trame Opus toutes les 5 ms
exactement (le fil avance l'horloge RTP d'une trame par paquet, donc le
cadenceur remplit de silence quand rien n'a été capturé et jette au-delà de
20 ms de file), encodée par libopus en CELT bas délai à 128 kbit/s VBR. Ce qui
reste : l'hôte continue d'entendre son propre son — le loopback capture ce qui
part vers les haut-parleurs — là où Sunshine le coupe (`localAudioPlayMode`).
Périphérique audio virtuel ou équivalent à étudier pour la v0.3.0.

### Input — ce qui a été tranché

| Point | Décision |
|---|---|
| Clavier | **Scancodes**, pas codes virtuels : jeux et DirectInput ne voient que ça. VK direct sur `NON_NORMALIZED` |
| Souris absolue | Mappée sur l'écran capturé puis sur le bureau **virtuel** (0..65535), avec le rectangle DPI-virtualisé — l'inverse du correctif de §6.1, et c'est voulu |
| Souris absolue sous Linux | Même chemin, en deux temps explicites : le device uinput rapporte une fraction de son axe que le compositeur étale sur tout le bureau, donc l'origine du display **et** les bornes du bureau entrent dans le calcul (§23.3) |
| Souris relative | Passe par l'accélération du pointeur de l'hôte, comme une vraie souris |
| Manette | **Xbox 360 via ViGEmBus**. Les bits étendus Sunshine (paddles, touchpad, Share) sont jetés : un pad X360 n'a pas ces boutons |
| Bitmask de modificateurs | **Ignoré** : le navigateur envoie déjà un vrai keydown/keyup pour Maj/Ctrl/Alt/Meta |
| Fin de session | Tout ce qui est encore enfoncé est relâché, et les pads débranchés |
| Pilote absent | Dégradation silencieuse : clavier/souris intacts, pas de manette |
| Saut de thread | ⚠️ **Toujours présent** : le relais marshale vers le thread Qt (`DataChannelRelay.cpp:764`) parce que le même handler pilote presse-papier, politique et stats. Le sink est prêt, le relais non |

---

## 14. Le banc : `--native-bench`

L'instrument des benchmarks d'encodeur. Il fait tourner le moteur — capture,
conversion, encodage — sur un écran pendant N secondes **vers un puits** : pas de
réseau, pas de navigateur, rien en aval de l'encodeur n'entre dans la mesure.

```
MoonlightWeb.exe --native-bench display=1,seconds=10,codec=hevc,fps=0,bitrate=20000,out=bench.csv
MoonlightWeb.exe --native-bench ""          # liste les écrans (display=<id>)
```

Clés : `display`, `seconds`, `codec` (hevc|h264|av1), `fps` (0 = celui de
l'écran), `bitrate` (kbps), `width`/`height` (0 = ceux de l'écran), `yuv444`,
`intra` (intra-refresh), `out`.

**Depuis le 04/09/2026, les réglages d'encodeur eux-mêmes** (`EncoderTuning`,
en-tête public, défaut = le choix du moteur, jamais rempli par une session
navigateur) : `preset=1..7`, `tuning=ull|ll`, `multipass=off|quarter|full`,
`aq`, `taq`, `preanalysis`, `quality=speed|balanced|quality`, `tu=1..7`,
`vbv=<frames>` ; et `gpu=<id>` pour encoder sur un autre GPU que celui de
l'écran (copie inter-GPU, §5) — c'est ainsi que l'iGPU AMD de bench-desk, qui ne
pilote aucun écran, a pu être mesuré. Sans `display=`, le banc liste écrans **et
GPU**. Chaque encodeur écrit dans son log ce que le preset ou l'usage active de
lui-même (multipass, AQ, lookahead ; qualité, pré-analyse, VBAQ) puis la
configuration effective. Le lookahead n'est pas exposé : il retient N images par
construction, disqualifié avant toute mesure. La variable d'environnement
`MW_NATIVE_TUNING` accepte les mêmes clés sur une **vraie session** (log
« MW_NATIVE_TUNING in effect »), pour l'A/B à l'œil que le banc ne peut pas
faire. Campagne du 04/09/2026 et recommandation : `docs/bench-native-host.md`.

**Depuis le 27/09/2026, la chaîne d'image elle-même** (plan D3D12, §32) :
- Sous Windows : `pipeline=auto|d3d11|d3d12`, `conv12=direct|compute`,
  `enc12=ve|nvenc|amf`, `rc12=driver|qp`, `reencode=0|1`, `refit=0|1`,
  `interfloor=<k>|off`, `prio12=normal|high|realtime`,
  `creator12=own|default`, `ddasync=gpu|none|cpu`, `gputiming=0|1`,
  `strict12=0|1`, `pipelined=0|1` (§32.17 ; par défaut 1 sur un GPU Intel à
  mémoire propre, 0 ailleurs, depuis le 29/09) et `keep12=0|1` (§32.18, 1 par
  défaut depuis le 29/09).
- Sous Linux : `pipeline=auto|vaapi|vulkan`, `convert=gl|vulkan`,
  `priovk=normal|high`.
- Sur une vraie session seulement (`MW_NATIVE_TUNING`) : `namedrops=0|1`,
  l'image jetée au calage du lien nommée à l'encodeur (§9.10.2, §32.20 ; 1 par
  défaut depuis le 29/09 pour NVENC, en entrée D3D11 ou D3D12, et pour AMF en
  D3D11 ; 0 ailleurs).
- Les clés du banc lui-même : `dump=<fichier>` (le flux tel qu'il sort),
  `lose=<N>` (une perte signalée toutes les N images) et
  `ramp=<kbps>[@<s>]` (le débit alterne, comme le gouverneur le ferait).
- En variables d'environnement : `MW_D3D12_FAULT` (une panne injectée,
  §32.10) et `MW_DDA_REFUSE` (un refus de la duplication).

Sur une clé inconnue, `--native-bench` imprime la liste de toutes les clés,
avec leur sens.

Par frame, une ligne CSV : numéro, keyframe, capturée ou ré-émise, octets,
**QP moyen** (`frameAvgQP` côté NVENC, `StatisticsFeedbackAvgQP` côté AMF — un
q-index 0–255 en AV1), t₀ présent, t₁ acquis, t₂ converti, t₃ encodé, et les
durées dérivées. Le résumé sur la sortie standard donne cadence de capture
effective, moyenne/p95/p99 par étape, taille par frame (toutes, puis deltas
seuls) et QP moyen.

Premier passage le 02/09/2026 sur bench-desk : NVENC (RTX 5060 Ti) rend le QP —
première keyframe 1080p à **36 Ko / QP 36**, puis la rafale de raffinement à
248 Ko / QP 20 → 15, encode 5,8 ms de moyenne ; AMF (RX 7600, 1440p) encode en
5,5 ms mais **ne rend aucune statistique** sur ce pilote (`GetProperty` échoue
sur le buffer de sortie) — à élucider quand la campagne AMF commence.

Le QP est le **proxy objectif de qualité** du protocole de bench (plan v2 §5) : à
débit fixe, un réglage plus rapide qui coûte plus de 2 points de QP moyen n'est
pas plus rapide, il est plus flou. Les trois contenus (bureau fixe, défilement
de texte, séquence de jeu rejouée plein écran) sont affaire d'opérateur : le banc
mesure ce qui est à l'écran.

Pas un service : Desktop Duplication exige le bureau interactif, donc une
commande de terminal.

### ✅ Vérifié de bout en bout (31/08/2026)

Une **image réelle du bureau, dans un navigateur**, par le moteur natif :

- host `bench-desk — MoonlightWeb Host` **READY**, aucun pairing demandé ;
- une seule carte d'app, `Display 1 — 2560×1440 · 60 Hz` — la grille d'apps
  existante EST le sélecteur d'écran, comme prévu ;
- un clic → stream ; côté navigateur :
  `First video frame: isKeyframe=true size=39946 codec=hevc`, rendu en 1920×1080 ;
- côté worker : duplication 2560×1440 → conversion + mise à l'échelle 1920×1080
  → NVENC HEVC intra-refresh ;
- arrêt propre au bouton.

Trois bugs ont été trouvés en poussant ce test, et aucun n'était visible en
test unitaire : le crash du préchargement de jaquettes (§6.5), la validation
d'URL RTSP appliquée au natif, et `serverCodecModeSupport` à zéro.

### ✅ Multi-GPU vérifié (31/08/2026)

Deux cartes dédiées dans la même machine — RTX 5060 Ti et RX 7600 — avec un
écran sur l'AMD et un dummy HDMI sur la NVIDIA. C'est le scénario que §5
(association display → GPU) existe pour couvrir.

DXGI attribue chaque sortie sans ambiguïté, y compris entre constructeurs :

| Display | GPU | Encodeur retenu |
|---|---|---|
| Display 1 — 2560×1440 · 60 Hz | AMD RX 7600 | **AMF** |
| Display 2 — 800×600 · 30 Hz | AMD RX 7600 | *(écran virtuel Parsec/VDD)* |
| Display 3 — 1920×1080 · 60 Hz | NVIDIA RTX 5060 Ti | **NVENC** |

Chaque display encode donc sur **le GPU qui le scanne**, en zéro-copie, sans la
moindre table de correspondance à maintenir. Les capacités le confirment aussi
par adaptateur : la RX 7600 rapporte AV1/HEVC/H.264, l'iGPU AMD du même pilote
seulement HEVC/H.264.

**Limite constatée, et voulue** : deux sessions propriétaires simultanées ne
coexistent pas — la seconde démolit la première. C'est le take-over délibéré du
`/start` existant, pas une propriété du moteur natif.

### Intel (oneVPL) — ⚠️ **exécuté pour la première fois le 07/09/2026**

> ⚠️ Cette section disait « écrit, jamais exécuté ». Elle est corrigée : le
> chemin Intel a streamé pour de vrai sur le banc `bench-intel` (N95 / UHD
> Graphics, pilote 32.0.101.7088), et il n'en est **rien sorti d'intact** — cinq
> défauts, dont trois fatals, décrits au §21. Ce qui suit reste vrai de la
> conception ; le tableau des choix a été corrigé là où le matériel a tranché.

Ce qui était vérifié **avant** tout matériel, et l'est resté :

- **le calcul des paramètres**, qui est la partie la plus facile à se tromper
  en silence. `TargetKbps` est un `mfxU16` : au-delà de 65535 kbps il faut
  `BRCParamMultiplier`, sans quoi une demande à 100 Mbps se replierait sur une
  fraction d'elle-même et ressemblerait à un réglage de débit ignoré. Testé :
  150000 kbps → 50000 × 3 ;
- **l'alignement** : surface 16-alignée, crop à la taille réelle (1080 → 1088
  de surface). Se tromper donne quelques pixels de rebut au bord ;
- **l'absence du runtime**, qui doit répondre proprement — c'est le cas sur ce
  banc, sans GPU Intel.

Choix assumés, notés pour qui reprendra :

| Point | Décision |
|---|---|
| Runtime | oneVPL 2.x (`libvpl.dll`) seulement, pas le Media SDK historique |
| Implémentation | filtrée sur HARDWARE — sinon oneVPL sert son repli logiciel en silence |
| Choix du GPU | par notre device D3D11, comme AMF, plutôt qu'en appariant à la main les énumérations Intel et DXGI. ⚠️ **mais pas par `MFXVideoCORE_SetHandle`** : sur le dispatcher 2.x il faut le donner à la CRÉATION (`mfxHDL` + `mfxHandleType`) — §21.1 |
| Moteur | `LowPower = ON` (VDENC, le bloc fixe) avec repli automatique sur le moteur général si `Query` le refuse — §21.4 |
| Débit variable | possible seulement **vers le bas** : `Reset` refuse toute cible au-dessus de celle de l'init — §21.5 |
| 4:4:4 | non revendiqué : la passe de conversion produit de l'AYUV, qu'oneVPL ne prend pas en entrée d'encodeur |
| Intra-refresh | demandé par `mfxExtCodingOption2` (`IntRefType = VERTICAL`), avec repli explicite sur les keyframes si `EncodeInit` le refuse — le refus est journalisé, jamais avalé |

### Vérification restante

**Un stream Wolf réel.** Le refactor `IMediaEngine` est purement typologique —
aucun corps de méthode modifié — mais il touche les trois relais. Sunshine a
été revérifié le 31/08 après le chantier intra-refresh (HEVC, 132 fps, 9,9 ms,
aucun changement de comportement) ; Wolf reste à repasser.

## 15. Le service : capturer depuis la session console (04/09/2026)

L'installation standard de MoonlightWeb est un service Windows (NSSM, session 0).
La session 0 n'a pas de bureau : Desktop Duplication n'y duplique rien, `SendInput`
n'y atteint aucune fenêtre, et la sonde du moteur le dit déjà en une ligne
(`hasInteractiveSession()` = faux, « no interactive desktop session »). Sans ce
chantier, le host natif n'existe donc **pas** pour l'installation la plus
courante — il n'apparaît qu'en instance dev, lancée à la main sur un bureau.

Tout ce que le natif fait doit se passer dans la **session console** — celle qui a
le moniteur et le clavier — dans un processus qui tourne sous l'utilisateur qui y
est connecté. Deux choses en découlent : **sonder** le moteur, et **lancer** le
worker de stream, tous deux ailleurs que dans le processus service.

### 15.1 `--native-probe` : les yeux du service sur le bureau

`MoonlightWeb --native-probe` fait tourner `NativeHost::probe()` là où il est
lancé, imprime **un objet JSON** sur stdout (`NativeCapabilitiesJson`, schéma
versionné : les deux bouts sont le même binaire, mais un schéma inconnu — un
fichier remplacé sous un service qui tourne — est refusé en entier plutôt que
cru à moitié) et sort ; le log du moteur part sur stderr, où le parent le récupère
pour le sien. Les énums voyagent en valeurs numériques, la poignée d'adaptateur
64 bits en hexadécimal (un `double` JSON perdrait ses bits au-delà de 2⁵³).

`NativeProbeService` en fait un **instantané** : lancer un processus à chaque
requête de la liste des hosts serait cher, donc la dernière réponse est gardée,
rendue tout de suite, et renouvelée en arrière-plan quand elle a plus de 20 s et
que quelqu'un demande, quand l'utilisateur console change (ouverture/fermeture de
session, surveillée toutes les 5 s **sans** rien lancer), et au démarrage.
`changed()` lève la carte du host quand l'utilisateur se connecte et la baisse
quand il se déconnecte. Avant la première réponse, l'instantané dit
« indisponible, en attente de la sonde de session console » : un host qui
apparaît une seconde après le démarrage du service est correct, un host offert
qui échoue ne l'est pas. Sur un bureau (instance dev, lancement manuel), rien de
tout cela : `snapshot()` appelle directement le moteur, à chaque fois.

### 15.2 `ConsoleProcess` : lancer sous l'utilisateur connecté

`WTSGetActiveConsoleSessionId` + `WTSQueryUserToken` donnent le jeton du shell de
l'utilisateur ; sous UAC c'est le jeton **filtré** d'un administrateur, alors le
jeton **lié** (le complet) est demandé ensuite et préféré quand il existe — le
worker tape alors dans les fenêtres élevées comme l'utilisateur le pourrait, et
tourne quand même **sous l'utilisateur, pas sous SYSTEM**. Un processus réseau qui
décode des inputs venus d'un navigateur est précisément celui à qui laisser le
moins de privilèges possible ; le prix — image noire pendant qu'une invite UAC est
à l'écran, que la boucle de capture gère déjà comme un display perdu — est celui
que les utilisateurs Sunshine paient sous une autre forme (l'invite est capturée,
mais le stream ne peut pas y répondre non plus).

`CreateProcessAsUserW` sur `winsta0\default`, avec le bloc d'environnement de
l'utilisateur (son `%APPDATA%`, là où Qt met ensuite le log et l'identité du
worker) et trois tubes dont les bouts enfant sont les **seuls** handles hérités
(`PROC_THREAD_ATTRIBUTE_HANDLE_LIST` : le parent peut être SYSTEM et l'enfant
l'utilisateur, aucun socket ni fichier du service ne doit franchir cette ligne).
Les tubes sont drainés par des threads à eux ; la sortie n'est signalée qu'une
fois les deux lecteurs finis, si bien qu'une ligne `response` écrite juste avant
de mourir n'est jamais doublée par la sortie du processus.

`StreamWorkerHost` choisit le lanceur en une ligne : un worker **natif** démarré
par le service va en session console (`ConsoleProcess`), tout autre backend parle
à un host par le réseau et tourne très bien depuis la session 0 (`QProcess`,
inchangé). Le même protocole de lignes JSON, les mêmes trois tubes ; rien après
`start()` ne voit la différence. `MW_CONSOLE_LAUNCH=force` prend ce chemin depuis
un bureau ordinaire, pour l'éprouver sans service.

### 15.3 `/api/native/status` et la ligne d'overlay

`GET /api/native/status` répond `{available, reason, remote_session, ...}`. La
disponibilité et la raison (l'énum) vont à tout appelant : c'est ce qui explique
pourquoi la carte « <hôte> — MoonlightWeb Host » est ou n'est pas dans sa liste.
Le reste — noms d'écrans, GPU, encodeur, codecs — ne va qu'à un appelant **assis à
la machine** (même raisonnement que `/api/internet/status` qui masque sa topologie
à distance). En service, `remote_session` est vrai et `user_present` distingue
« personne n'est encore connecté » d'« aucun encodeur ». Côté client, une ligne
**« Encodeur : NVENC »** dans le détail de latence nomme enfin le bloc de silicium
qui encode ce stream (`describeEncoder()`, portée dans `/start` par
`native_encoder`).

⚠️ **corrigé le 04/09 au soir sur retour de Bruno** : la première version mettait
`describeSession()` en entier — « NVIDIA GeForce RTX 5060 Ti · NVENC HEVC
intra-refresh » — dans le bloc toujours visible. Soixante caractères de valeur
élargissent la carte au-delà d'un écran de téléphone, et **toutes les autres
lignes se retrouvaient coupées** (« 1920× », « 0.8 M »). La chaîne longue reste ce
qu'elle a toujours bien fait, une ligne de log ; le codec a déjà sa ligne, le GPU
est sur la page admin, et ce qui manquait vraiment à l'overlay tient en un mot.
Il est rangé dans le détail plutôt que dans le bloc compact : il explique les
étapes listées sous lui et ne change jamais en cours de session.

### 15.4 Vérifié / à valider par Bruno

Vérifié le 04/09 en mode `MW_CONSOLE_LAUNCH=force` (bureau, `ConsoleProcess` en
`CreateProcess` même session) : le service voit « no desktop (session 0) »,
affiche d'abord « waiting for the console session probe », relaie le stderr de la
sonde en `[native-probe]`, puis « console probe: available — 2 display(s),
5 GPU(s) » et lève la carte du host ; `/api/native/status` en loopback rend les
deux écrans avec GPU/encodeur/codecs. **Reste à valider par Bruno en vrai
service** (installer le binaire en service, se connecter, streamer) : un stream
natif complet passé par `CreateProcessAsUser` sous le jeton console, et la
neutralité du chemin `QProcess` pour Sunshine/Wolf/MultiSeat (inchangé, mais
`StreamWorkerHost` a bougé).

### 15.5 L'installeur Windows n'installe plus Sunshine (04/09/2026)

Conséquence produit de 15.1–15.3, faite sur le feu vert de Bruno **avant** la
validation en vrai service (le plan la faisait attendre) : l'installeur Inno ne
détecte plus Sunshine, ne le télécharge plus, ne lui écrit plus d'identifiants et
n'appaire plus rien. Il reste une page de question, le lien Internet, et une
checklist d'une ligne. Ce qui disparaît du script : la page Sunshine et ses trois
formes, le bouton Skip, la sonde d'identifiants Basic-Auth (avec son encodeur
Base64 maison), le téléchargement `/S` + `--creds`, et l'objet `sunshine` de
`provisioning.json` — donc **plus aucun mot de passe en clair écrit sur le
disque**. `Provisioning::applyOnce` lit un objet absent comme `auto_pair=false` et
marque l'étape « skipped », si bien qu'un serveur plus ancien recevant ce fichier
se comporte exactement comme si l'utilisateur avait cliqué sur Ignorer.

> ⚠️ **Périmé depuis le 08/09/2026 — voir §25.** Ce qui suit était vrai le 04/09
> et a cessé de l'être en deux jours : macOS (§20) et Linux (§19) ont eu leur
> moteur natif les 05 et 06, si bien que « ces plates-formes n'ont pas de moteur »
> ne justifie plus rien. Sunshine est sorti de l'assistant et de l'installeur
> macOS le 08/09, sur la même règle qu'ici : seulement là où la machine peut se
> diffuser elle-même.

Ce qui **ne** bouge **pas** : `SunshineInstaller` en entier, et la page Sunshine
du `SetupView`. ⚠️ **le plan disait « retrait de Sunshine de l'installeur *et* de
`SetupView` », c'est faux** : `SetupView` ne s'affiche jamais sous Windows
(`/api/setup/status` y répond `setup_completed: true` en dur, et `app.js` sort sur
`os === 'Windows'`) — c'est le premier lancement de **macOS et Linux**, les deux
plates-formes où le moteur natif répond « no backend for this platform in this
build » jusqu'à la phase I. L'y retirer ne complèterait pas H4, ça priverait ces
machines de tout hôte. Sunshine y reste donc la voie normale, et il reste partout
un **hôte** de plein droit : une machine qui le fait déjà tourner se découvre et
s'appaire depuis la page des hôtes comme avant.

Effet de bord corrigé au passage, sans quoi le retrait aurait été une
régression : `NvComputer::isLocalMachine()` répondait faux pour le host natif.
Elle compare des adresses, et le natif n'en a aucune — il *est* ce processus. Or
la mise à jour en un clic ne s'offre que si la machine a un hôte local appairé
(`_canSelfUpdate`), et depuis ce chantier la carte native est le seul hôte d'une
installation Windows fraîche : la bannière serait retombée à jamais sur « mettez à
jour le PC hôte ». `isNativeEngine()` répond maintenant vrai en premier ; au
passage `hostOs()` en profite (il court-circuite sur « local »), et
`/api/setup/status` exclut explicitement la carte native de son `sunshine.paired`,
qui doit continuer de vouloir dire ce qu'il dit.

## 16. HDR réel : scRGB FP16 → P010 BT.2020 PQ (04/09/2026)

Premier morceau de la phase I. Jusqu'ici le HDR était négocié puis **rabattu en
SDR** : le convertisseur ne savait faire que du 8 bits BT.709, et la session
ouvrait donc la capture en 8 bits pour recevoir le bureau déjà tone-mappé par
DXGI. Le chemin complet existe maintenant.

### 16.1 La chaîne, et l'ordre des étapes

DXGI livre un bureau HDR en **scRGB** : lumière linéaire, primaires BT.709, et
1.0 = 80 nits. Ce n'est **pas** là que se trouve le blanc du bureau : le
compositeur peint le contenu SDR (fenêtres, fond d'écran, pointeur) au niveau
du curseur « luminosité du contenu SDR », soit `SdrWhite` = 1.0 au réglage
d'usine et couramment 2 à 3 sur un écran qu'on a réglé (corrigé le 16/09/2026,
§16.6 — ce paragraphe disait le contraire). Les valeurs au-dessus sont les
hautes lumières ; celles **en dessous de 0** sont les couleurs hors BT.709, que
scRGB exprime en négatif.

Quatre étapes, et l'ordre n'est pas négociable :

1. **primaires** BT.709 → BT.2020, en lumière linéaire — une matrice n'est
   valide que là ;
2. **échelle absolue** : ×80/10000, PQ étant défini contre un pic de 10 000 nits ;
3. **courbe PQ** (SMPTE ST 2084), avec un `max(0)` juste avant : `pow()` d'un
   négatif donne NaN, et un NaN se propage à toute la trame ;
4. **matrice YCbCr** BT.2020 non-constant luminance, qui est définie **sur le
   signal PQ**, pas sur la lumière.

Faire PQ après la matrice YCbCr est la façon classique d'obtenir une image
presque juste et subtilement fausse dans chaque dégradé.

Sortie : **P010**, 4:2:0 10 bits, plage limitée (luma 64..940, chroma 64..960
autour de 512). Les vues de plan sont les mêmes que NV12 d'un cran plus large,
`R16_UNORM` et `R16G16_UNORM`, parce que P010 range ses 10 bits dans des mots de
16. Le code est **arrondi à l'entier** avant d'être écrit : les 6 bits bas sont
définis comme nuls, le matériel n'en lit que les 10 hauts et s'en moque, mais une
surface seulement valide par accident est une surface que personne ne peut
vérifier — et c'est exactement ce que le test relit.

Le curseur est linéarisé depuis sRGB et posé **au blanc SDR du bureau**
(`SdrWhite`, depuis le 16/09 ; à 1.0 avant, donc plus sombre que la fenêtre
qu'il survolait dès que le curseur Windows était monté) avant d'être composé —
le mélanger tel quel donne un pointeur bien trop sombre sur un bureau HDR. Le
masque d'inversion est borné contre ce même blanc : `blanc - rgb` sur une haute
lumière à 10.0 serait très négatif, et ce négatif rencontrant le `max(0)`
transformerait le curseur texte en trou noir.

### 16.2 Ce que l'encodeur doit dire, pas seulement faire

NVENC : `NV_ENC_BUFFER_FORMAT_YUV420_10BIT` (c'est P010), profil **Main10 nommé
explicitement** pour le HEVC — laissé sur Main, NVENC accepte la surface P010 et
encode 8 bits dedans, donc le HDR part à la poubelle en silence — et
`inputPixelBitDepthMinus8`/`pixelBitDepthMinus8` pour l'AV1, qui porte sa
profondeur dans sa config et non dans un GUID de profil.

Surtout, la **description couleur dans le flux** : primaires 9 (BT.2020),
transfert 16 (SMPTE 2084), matrice 9 (BT.2020 NCL), plage limitée. C'est par elle
que le navigateur sait qu'il doit inverser la courbe PQ. Sans elle il suppose du
BT.709 sRGB et peint une image plate et grisâtre : le classique « le HDR est
délavé » qui se lit comme un bug de shader et n'est en fait que trois entiers
manquants.

### 16.3 Deux pièges de type B7 fermés au passage

**AMF et oneVPL annonçaient `supports10Bit`** depuis une vraie requête matérielle
— la silicium l'a — alors qu'aucun des deux n'a de chemin P010. Le Selector aurait
donc accordé le HDR sur une Radeon et la session serait morte à `init()` : c'est
mot pour mot le bug B7, où un `supports444` par GPU tuait le flux au clic. Les
deux capacités sont désormais **fausses par construction**, avec la ligne de code
à restaurer écrite en commentaire, dans le même commit que le chemin encodeur et
jamais avant. Une capacité dit « ce pipeline sait le porter », jamais « cette puce
le pourrait ».

**HDR et 4:4:4 ne peuvent pas voyager ensemble.** Le 10 bits 4:4:4 existe (Y410,
profil HEVC 4) et aucun navigateur ne l'affiche : Chrome 152 accepte
`hvc1.4.156`, le décode en matériel et rend un rectangle vert (F0f). Le Selector
les accordait tous les deux ; il garde maintenant le HDR et rend la chroma, avec
une ligne de log. Le 4:4:4 continue de **choisir le codec** — le HEVC est retenu
parce que c'est lui qui a un chemin 4:4:4 — et n'est repris qu'ensuite.

### 16.4 ⚠️ Le bug qui rendait tout stream SDR impossible depuis un bureau HDR

Trouvé en écrivant ce chapitre, sur un écran mis en HDR pour l'occasion, et
antérieur à lui : `DxgiDuplication` prenait son format dans
`duplDesc.ModeDesc.Format`, qui décrit le **mode d'affichage**, pas la sortie de
la duplication.

`DuplicateOutput1` convertit le bureau vers le premier format de la liste qu'il
peut honorer : une session SDR, dont la liste ne contient que BGRA8, reçoit donc
vraiment du BGRA8. `ModeDesc`, lui, continue d'annoncer FP16. Le convertisseur
construisait alors une vue FP16 sur une texture 8 bits, `CreateShaderResourceView`
la refusait, et la session mourait à la première image sur « could not view the
captured frame ».

C'était **tout stream SDR depuis une machine avec le HDR Windows activé** — le cas
exact que B1 devait régler et n'a réglé qu'à moitié : B1 a cessé de *demander* du
FP16, ceci cesse de *mal lire* ce qui revient. Le correctif est étroit : une liste
à une seule entrée est le seul cas où la réponse est connaissable sans acquérir
une trame, et c'est aussi le seul où `ModeDesc` se trompe (avec la liste HDR à
deux entrées DXGI garde le format du bureau, que `ModeDesc` rapporte fidèlement,
et le repli `DuplicateOutput` ne convertit rien).

### 16.5 Vérifié, et ce qui reste

Vérifié sur bench-desk, écran mis en HDR par `scratchpad/Set-DisplayHdr.ps1` :
`--native-bench display=0,hdr=1` donne « duplication started: (HDR, FP16) » →
« colour conversion: FP16 scRGB -> P010 4:2:0 (BT.2020 PQ, limited) » →
« NVENC ready: HEVC Main10 (BT.2020 PQ) », 30 trames encodées. Le banc a une
option `hdr=0|1`.

Tests (`test_capture`, branche qui ne s'exécute que sur un écran réellement en
mode HDR, et se déclare sautée sinon) : sortie **P010** confirmée, plage de luma
relue **64..588 en codes 10 bits** — dans les bornes légales, donc ni biais oublié
ni échelle oubliée —, 6 bits bas nuls, et une keyframe **HEVC Main10 de 37 Ko**.
La branche SDR relit 16..235 sur le même bureau HDR, ce qui est la non-régression
de 16.4. 1977 vérifications natives HDR allumé, 1966 éteint.

**Reste à Bruno** : le rendu à l'œil sur un vrai client. Le HDR de bout en bout
dépend du présentateur navigateur (F0e/F0f) : AV1 10 bits passe par WebGPU en mode
`linear`, HEVC 10 bits par l'élément `<video>`. Personne n'a encore regardé une
image HDR **native** sur un écran HDR — seulement des chiffres qui disent que les
octets sont dans les bonnes bornes.

**✅ B1 vérifié en vrai le 06/09/2026** (le test « HDR Windows actif → session
SDR » que le plan laissait à Bruno) : HDR activé sur le M27Q **pendant** un
stream SDR natif (HEVC 2560×1440@60, client Chrome sur l'écran virtuel voisin).
La session survit — « duplication lost (mode change or desktop switch) — will
restart » puis « duplication started: 2560x1440 (SDR, BGRA8) » en 90 ms, deux
fois de suite (le basculement HDR change le mode deux fois) — et l'image reçue par
le client, capturée à l'écran, est **identique** à celle du même bureau en SDR.
⚠️ **Vrai seulement au réglage d'usine du curseur « luminosité du contenu
SDR »** — voir §16.6 : le 16/09, sur le même DualRTX avec le curseur monté, le
même stream SDR arrivait cramé. Ce que DXGI livre en 8 bits n'est pas « le rendu
SDR du compositeur », c'est le bureau écrêté à 80 nits, ce qui ne se voit pas
tant que le blanc SDR *est* à 80 nits.
Conséquence pour le client (toujours vraie, pour une autre raison) : un hôte
Windows en HDR streamé en SDR n'a besoin d'aucun tone-map côté navigateur — il
est fait sur l'hôte, §16.6.

**✅ F0d(2) fermée le 07/09/2026 — sans objet, et pour trois raisons qui se
recoupent.** L'item restait ouvert « à rouvrir avec un banc hôte macOS/Linux
HDR » ; ce banc existe depuis le 06/09 (§20.10), et la relecture des trois
plateformes le referme :

1. **Aucun hôte natif ne produit le cas.** C'est la négociation qui décide, pas
   l'état de l'écran. Windows : le convertisseur ramène lui-même le bureau FP16
   en SDR (§16.6 ; avant le 16/09, capture `BGRA8` écrêtée par DXGI, juste
   au-dessus). macOS : `SckCapture` demande
   `kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange` + `kCGColorSpaceSRGB` +
   matrice BT.709 quand la session est SDR — c'est le compositeur qui rend le
   SDR, même mécanisme que Windows. Linux : il n'y a pas de HDR du tout
   (`LinuxProbe` pose `hdrActive = false`, la capture lit du XRGB 8 bits).
2. **Le client ne demande jamais le HDR sur un écran SDR.** `app.js` efface
   `hdr_enabled` au lancement quand `hdrClientCapability()` refuse — écran en
   mode HDR **et** adaptateur WebGPU **et** décodeur 10 bits. La combinaison
   « flux HDR sur écran SDR » n'est donc pas atteignable par le chemin normal.
3. **Forcée en debug (`mw_hdr_request=1`), elle est déjà servie** : HEVC prend
   le mode `browser` (le tone-map du navigateur), AV1 le mode `tonemap` (ACES
   sur WebGPU, livré en F0e). Il ne resterait à couvrir que « AV1 HDR + écran
   SDR + pas de WebGPU » — or la garde du point 2 exige justement WebGPU, donc
   ce triplet ne s'atteint qu'en contournant volontairement la garde sur une
   machine sans WebGPU. Pas de shader à écrire pour ça.

### 16.6 Bureau HDR → stream SDR : le tone-map est à nous (16/09/2026)

Constaté par Bruno sur DualRTX (écran HDR, session SDR parce que l'écran
*client* est SDR) : sable et écume du fond d'écran partis en blanc pur, tout le
bureau trop clair. Le flux reçu était bien SDR (`HEVC` seul dans l'overlay), donc
rien à voir avec le gris WebKit de la veille : l'image était déjà fausse en
sortie d'hôte.

**La cause.** La session SDR demandait à DXGI du 8 bits seul
(`DuplicateOutput1` avec `B8G8R8A8` en liste unique), et ce que DXGI fait alors
n'est pas un tone-mapping : un **écrêtage à 80 nits** (scRGB 1.0). Or le
compositeur peint le contenu SDR au niveau du curseur « luminosité du contenu
SDR » (`DISPLAYCONFIG_SDR_WHITE_LEVEL`, 1000 = 80 nits), que tout le monde monte
sur un écran HDR — sinon les fenêtres sont ternes. À 2.5 (200 nits), un blanc de
fenêtre vaut 2.5 en scRGB : écrêté à 1.0, comme tout ce qui dépasse 40 % de
luminosité. D'où la vérification « identique » du 06/09 : le curseur y était au
réglage d'usine.

**Le correctif**, tout côté hôte — c'est le seul endroit où l'information existe
encore, le navigateur reçoit du 8 bits déjà coupé :

- `DxgiDuplication` ne prend plus de paramètre `hdr` : la liste est toujours
  `{FP16, BGRA8}`, DXGI garde le format du bureau (§16.4 reste vrai : avec cette
  liste `ModeDesc.Format` dit juste). Une session SDR sur bureau HDR reçoit donc
  du scRGB.
- `ColorConvert::init(…, hdr=false)` accepte FP16 : les shaders BT.709 sont
  compilés avec `MW_SCRGB_SOURCE=1` et `Scene()` devient
  `ToneMapToSdr(SceneHdr(uv))`. Même sortie NV12/AYUV, mêmes points d'entrée ;
  `toneMapsToSdr()` dit lequel des deux tourne. PQ sur du 8 bits reste refusé.
- `ToneMapToSdr` : division par `SdrWhite` (le blanc du bureau → 1.0), identité
  jusqu'à un genou à **0.9**, épaule `tanh` au-dessus, sur la luminance pour
  garder la teinte, puis encodage sRGB. **C'est la courbe `soft-clip` du client**
  (`WebGpuRenderer`, `HDR_COMMON_WGSL`), reprise à l'identique : fenêtres, texte,
  jeu SDR passent tels quels ; les hautes lumières HDR se replient dans les 10 %
  restants. Pas ACES, exprès : ACES déplace aussi les tons moyens, et sur un
  bureau les tons moyens *sont* l'image. Pas `ID3D11VideoProcessor` non plus :
  une passe de plus (le pixel shader tourne déjà sur chaque trame), pas de
  notion de blanc SDR, et une courbe qui change de pilote en pilote.
- **Le blanc SDR est lu sur l'écran** (`WindowsSession::readSdrWhite`) : DXGI
  donne le nom GDI de la sortie, `QueryDisplayConfig` la source de même nom,
  `DisplayConfigGetDeviceInfo(GET_SDR_WHITE_LEVEL)` sur sa cible. Lu au build du
  pipeline et **re-lu toutes les secondes** sur les chemins FP16 (le curseur
  Windows est vivant ; ~0,1 ms), ligne de log au changement : « the desktop's
  SDR white is 200 nits — the tone map brings it to white ».
- Le chemin HDR (P010) en profite : le **pointeur** composé y était posé à 1.0,
  donc plus sombre que la fenêtre survolée dès que le curseur Windows était
  monté ; il est posé à `SdrWhite` maintenant, masque d'inversion borné pareil.

**Ce qui ne change pas.** WGC reste 8 bits (pas de HDR sur le repli, §17) — il
subit le même écrêtage, non traité, sur les seules machines qui n'ont pas de
DDA. macOS n'a pas le problème : ScreenCaptureKit rend le SDR avec le blanc à
1.0, l'EDR n'est que de la marge au-dessus. Linux n'a pas de HDR. Le pont
inter-GPU porte du FP16 (8 octets/pixel au lieu de 4) sur une session SDR de
bureau HDR : deux fois le trafic mémoire système de ce seul cas.

**Tests.** `test_capture` : la branche SDR tourne sur le bureau tel qu'il est et
vérifie `toneMapsToSdr() == isHdrSource(format)`, plage de luma 16..235 relue sur
la sortie tone-mappée ; la branche 4:4:4 n'est plus sautée sur bureau HDR ;
l'encodage H.264 non plus. La branche HDR (P010) est inchangée.

**Reste à Bruno** : l'œil, sur DualRTX avec « Display 2 » (VDD) en HDR et le
curseur SDR monté — le fond d'écran doit revenir identique au bureau local, et
une vidéo HDR YouTube doit garder du détail dans ses blancs au lieu d'un aplat.

## 17. Windows.Graphics.Capture, le repli (04/09/2026)

Deuxième morceau de la phase I. Desktop Duplication répond
`DXGI_ERROR_UNSUPPORTED` quand l'écran n'est pas balayé par l'adaptateur auquel on
la demande : c'est l'état ordinaire d'un portable hybride, dont la dalle pend à
l'iGPU pendant que le dGPU fait tourner le jeu. Aucun contournement n'existe côté
DXGI. WGC passe par le compositeur et ne se soucie pas de qui balaye.

### 17.1 Pourquoi c'est un repli et pas le défaut

DDA réveille l'appelant **sur le présent** — la meilleure propriété de latence de
tout ce moteur. WGC livre par une file que le compositeur remplit : c'est un
réveil derrière une queue, pas le présent lui-même. Le choix est donc DDA
d'abord, à chaque ouverture **et à chaque redémarrage** : un changement de mode ou
un redémarrage de pilote est exactement le moment où le bon backend change, et
rejouer le choix ne coûte qu'une tentative DDA ratée.

**Et en plein stream (28/09/2026, plan D3D12 C8.3 bis).** Un redémarrage ne
vient que d'une capture perdue, et WGC ne se perd pas. Un worker qui n'est pas
SYSTEM, repassé sur WGC après un refus de la duplication (le bureau sécurisé,
`0x80070005`), y restait donc jusqu'à la fin, en D3D11. Désormais, après un refus
qui peut passer, la boucle cherche la duplication. `DxgiDuplication::refusalMayPass`
tient pour passager tout refus, sauf `DXGI_ERROR_UNSUPPORTED` et l'absence
d'`IDXGIOutput1`.
- Toutes les demi-secondes, un regard sur le bureau d'entrée. Sans SYSTEM, le
  bureau sécurisé ne se lit même pas : rien n'est tenté tant qu'il est là.
- Ensuite, une duplication ouverte à côté. Tant qu'elle échoue, les essais
  s'espacent de 1 à 30 s.
- Dès qu'elle s'ouvre, le redémarrage ordinaire la reprend, et la chaîne D3D12
  revient avec elle.

`MW_DDA_REFUSE=[<à>+]<secondes>` simule ce refus au banc, là où il faudrait
sinon un écran verrouillé que quelqu'un déverrouille.

⚠️ **Pas de chiffre de comparaison ici.** Deux mesures au banc sur le même écran
se sont contredites (DDA 0,19 ms puis 2,19 ms de moyenne d'`acquire`, WGC 1,81
puis 1,05), parce qu'un bureau immobile ne présente presque rien : avec 16 trames
capturées en 6 secondes, `acquire` mesure surtout **quand l'écran a présenté**, pas
l'API. L'argument structurel tient, le chiffre n'est pas acquis — il demande un
écran en mouvement continu, comme la campagne E s'en était donné un.

### 17.2 Le curseur, que WGC ne donne pas

WGC ne rapporte aucun pointeur : il sait le composer **dans** l'image, et c'est
tout. Insuffisant ici pour deux raisons — un client peut demander à dessiner le
sien (il lui faut la forme en données, pas en pixels) et un téléphone l'agrandit
(il lui faut être séparable du bureau).

Le chemin WGC lit donc le pointeur directement dans Win32 : `GetCursorInfo` pour
la position et le `HCURSOR`, `GetIconInfo` pour la forme. Les trois encodages
Windows s'y retrouvent à l'identique — monochrome (deux masques 1 bit empilés,
AND au-dessus de XOR), couleur avec vraie couverture alpha, et couleur masquée
quand le bitmap n'a pas d'alpha du tout. Le décodeur qui les réduit tous les trois
en « image RGBA + drapeau d'inversion » a été **sorti de `DxgiDuplication` dans
`CursorShape`** et sert désormais les deux backends : ce sont soixante lignes de
manipulation de bits subtile qu'il aurait fallu corriger deux fois.

La position est mise à l'échelle entre le rectangle de bureau (virtualisé DPI) et
la texture capturée (en vrais pixels), sans quoi le pointeur dessiné se retrouve à
une fraction d'écran du vrai sur tout affichage mis à l'échelle.

Vérifié (`test_win32_cursor`, sans matériel particulier) : flèche standard lue en
canevas 32×32 dont **12×19 d'encre**, hotspot dans la forme, aucun pixel à la fois
dessiné et inversant, cache de forme qui ne re-décode pas un pointeur immobile,
mise à l'échelle vérifiée à un pixel près, et pointeur d'un autre écran déclaré
invisible.

### 17.3 ⚠️ L'horodatage de WGC n'est pas celui de DDA

DDA rapporte `LastPresentTime` : l'instant où la trame **a été** mise à l'écran,
toujours dans le passé. `SystemRelativeTime` de WGC est l'estampille du
compositeur et le **devance** — mesuré ici jusqu'à ~9 ms en avance sur l'instant
où on tire la trame, parce que c'est la présentation pour laquelle elle est
planifiée, pas une qui a eu lieu.

Laissé tel quel, c'est une latence de capture **négative**, et ça ne reste pas une
curiosité cosmétique : le gouverneur de lien (E3) dérive le délai aller simple du
client de (arrivée − présent), donc un présent dans le futur gonfle la montée
qu'il lit et fait couper le débit sur un lien qui va bien. L'estampille est donc
plafonnée à « maintenant ». La latence de capture sur ce chemin est un **plancher,
pas une mesure** — ce qui mérite d'être dit, et fait une raison de plus de garder
DDA en premier.

### 17.4 Choix d'implémentation et vérification

Pas de C++/WinRT : sa projection est fondée sur les exceptions, et ce module n'en
lie aucune. Les trois interfaces nécessaires sont du COM ABI ordinaire, activées
par `RoGetActivationFactory`. Le gestionnaire `FrameArrived` **doit** être agile
(`FtmBase`) — le pool libre-thread appelle depuis l'apartment du compositeur, et
sans le marshaleur libre l'abonnement est refusé d'emblée, ce qui a été la
première panne rencontrée. Le pool a **deux** tampons, pas plus : une file est de
la latence, et une plus profonde laisserait le compositeur prendre de l'avance sur
l'encodeur.

`put_IsCursorCaptureEnabled(false)` pour garder le pointeur séparable, et
`put_IsBorderRequired(false)` pour la bordure jaune (Windows 11 seulement ; sur
Windows 10 elle reste).

**`MW_CAPTURE=wgc` force le repli** sur une machine où DDA marche parfaitement.
Sans ça le chemin WGC n'est atteignable que sur du matériel que personne ici ne
possède — et c'est ainsi qu'un repli pourrit : écrit une fois, jamais exécuté,
trouvé cassé sur la seule machine qui en avait besoin. Même intention que
`MW_CONSOLE_LAUNCH=force`.

Vérifié : flux au banc sur le chemin forcé (80 trames, keyframe, arrêt propre), et
dans les tests la relecture des pixels — **luma NV12 19..234**, donc une vraie
image et pas un écran noir, ce que « la session a démarré » n'aurait jamais
prouvé. La branche WGC des tests s'exécute sur **toute** machine, pas seulement
sur celles qui en ont besoin.

## 18. oneVPL : audit sans matériel (04/09/2026)

Troisième morceau de la phase I, et le seul qui ne pouvait pas être exécuté : il
n'y a pas de GPU Intel ici. Le travail utile était donc de relire ce chemin
contre les invariants que NVENC et AMF respectent, et un vrai défaut en est
sorti.

### 18.1 ⚠️ `setBitrate` éteignait l'intra-refresh

`setBitrate` reconstruisait le bloc de paramètres de zéro avec
`fillEncodeParams`, puis appelait `EncodeReset`. Ce bloc neuf jette deux choses
que personne ne verrait partir :

- **la chaîne d'extension**, donc l'intra-refresh s'arrête — pendant que
  `intraRefreshEnabled()` continue de répondre vrai depuis le drapeau posé à
  l'init. Le récepteur est alors informé que le flux se répare seul, encaisse
  une perte en attendant une vague qui ne viendra jamais, et abandonne sur le
  garde-fou de 15 s de G2 ;
- **les corrections du runtime**, appliquées par `EncodeQuery` à l'init et
  absentes de tout bloc fraîchement construit.

Et ça tourne en permanence : le gouverneur de lien change le débit environ deux
fois par seconde sur un lien qui bouge, donc l'intra-refresh survivait à peu près
une demi-seconde de streaming réel.

C'est exactement la forme du bug **B4** sur AMF — une ré-application en cours de
session qui laisse tomber en silence ce que l'init avait mis en place. Le fait
que le même piège se soit tendu deux fois, sur deux encodeurs écrits à des
moments différents, dit que la faute est structurelle et pas d'inattention.

**Correctif** : `applyRateControl()` écrit les champs de débit — et rien d'autre —
dans un bloc **existant**, et `setBitrate` mute `m_Params` au lieu de le
reconstruire. C'est ce que NVENC fait depuis toujours (il réutilise `m_Config`) et
ce qui rend l'erreur impossible plutôt que corrigée. `fillEncodeParams` appelle le
même helper, donc l'arithmétique n'a qu'un seul endroit.

Vérifié sans matériel (`test_vpl_params`) : après un changement de débit, la
chaîne d'extension est toujours attachée, la période d'intra-refresh intacte, une
correction simulée du runtime préservée, et les invariants de latence
(AsyncDepth 1, pas de B-frames, GOP infini) inchangés.

### 18.2 Ce qui reste vrai : ce chemin n'a jamais encodé une image

NVENC et AMF ont été mesurés sur les machines qu'ils visent. oneVPL, non — et la
ligne « ready » le dit encore à chaque session. Ni le HDR ni le 4:4:4 n'y sont
implémentés, et leurs capacités sont **fausses par construction** (§16.3) pour que
le Selector n'y route jamais une session qui mourrait à l'init.

Bruno aura du matériel Intel plus tard ; à ce moment-là, l'ordre est : lever les
capacités une par une, dans le même commit que le chemin qu'elles annoncent, et
jamais avant.

## 19. Linux : les entrées (04/09/2026)

Quatrième morceau de la phase I, entamé par la seule pièce qui pouvait l'être
sans machine Linux : **uinput ne demande aucune bibliothèque**, ce sont des ioctl
noyau. Elle compile donc pour de vrai, à `-Wall -Wextra` sous WSL Debian 13, ce
qui est un cran au-dessus de « écrit ».

### 19.1 Pourquoi uinput et pas le serveur d'affichage

XTEST marche sous X11 et nulle part ailleurs ; Wayland n'a aucun protocole
d'injection et n'en aura pas. uinput crée un **vrai périphérique** dans le noyau,
sous les deux : le même code sert X11, tous les compositeurs Wayland et une
console nue, et le compositeur applique la disposition clavier de l'utilisateur
par-dessus exactement comme pour un clavier physique. C'est aussi ce que la
manette virtuelle utilise déjà, donc un hôte Linux présente trois périphériques
ordinaires plutôt que trois cas particuliers.

**Deux périphériques, pas un** : un appareil qui déclare à la fois des axes
relatifs et absolus est lu différemment selon le compositeur — certains en
ignorent un, d'autres y voient une tablette.

### 19.2 La table clavier, et pourquoi elle est écrite en chiffres

Le navigateur envoie la **position** de la touche, exprimée comme la touche
virtuelle que cette position porte sur un clavier US. Les codes evdev sont des
positions aussi : c'est donc une correspondance position → position, et la
disposition de l'hôte ne doit pas y entrer. Un hôte français tape français depuis
un client AZERTY sans que cette table sache rien de l'un ni de l'autre. (La seule
entrée qui échappe à cette règle est le **texte** d'un clavier tactile, qui n'est
pas une position et doit connaître la disposition de l'hôte — §19.10.)

Les valeurs sont des **littéraux** et non des macros `KEY_*`, pour que l'en-tête
compile — et soit **testable** — sur une machine sans en-tête Linux, ce qu'est
toute machine sur laquelle ce moteur a été développé. Les codes sont une ABI
noyau, donc figés. C'est une affirmation, donc elle est vérifiée et non crue :
sous Linux, `UinputInput.cpp` compare 22 entrées couvrant chaque groupe de la
table à `<linux/input-event-codes.h>` par `static_assert`. Une dérive casse la
compilation au lieu de taper la mauvaise lettre sur le bureau de quelqu'un.

Vérifié aussi sous Windows (`test_evdev_keymap`, 104 codes distincts) : rangée
des chiffres qui ne commence pas à zéro, pavé numérique en ordre inverse de ses
touches virtuelles, F11/F12 non contigus à F10, modificateurs gauche et droite
distincts — les confondre casserait AltGr, donc `@` et `#` sur un clavier AZERTY —,
et **injectivité** de la table hors les trois alias voulus.

⚠️ La comparaison avec la table Windows a trouvé une divergence, et elle est
**intentionnelle des deux côtés** : `usScanCode` répond 0 pour Pause parce que
l'appelant Windows retombe alors sur `MapVirtualKey`, cette touche étant
indépendante de la disposition. Linux n'a pas de repli — uinput prend le code ou
rien — donc `KEY_PAUSE` y est nommé. Le test l'exige à **exactement une**
divergence, pour qu'une seconde, elle, échoue.

### 19.3 ⚠️ La route de capture : KMS d'abord, portail en repli

Écrit le 04/09 quand aucune machine Linux n'existait ; **caduc le soir même** :
Bruno a redémarré le bench-mini sous Ubuntu 22.04 avec sa Radeon 780M, et a tranché
« les deux, KMS puis portail ». Ce qui suit est mesuré sur cette machine.

Le plan disait PipeWire. La reconnaissance a pesé autrement : GNOME en
**Wayland**, et le portail `ScreenCast` demande un clic d'autorisation sur
l'écran de l'hôte à la première session, puis exige de tourner **dans la session
D-Bus de l'utilisateur** — l'exact problème de la session 0 Windows, en miroir.
KMS/DRM lit la sortie écran directement, sans dialogue et sans session, au prix
de `cap_sys_admin` posée ~~sur le binaire~~ par le paquet : Sunshine porte cette
capacité sur cette machine même (⚠️ **corrigé le 05/09 au soir** : pas sur le
binaire, sur un *lanceur* — une capacité sur `MoonlightWeb` lui-même casse son
rpath `$ORIGIN`, §19.8). La symétrie avec Windows s'impose — **KMS =
DDA** (image au scan-out, réveil sur le vblank), **portail = WGC** (file du
compositeur, repli).

Les licences ne bloquent pas : libpipewire, libva, libdrm, EGL, GBM sont MIT.

### 19.4 La chaîne zero-copy Linux, prouvée sur la 780M (04-05/09/2026)

Le zero-copy a **failli ne pas exister**, et l'endroit où il a plié est instructif.

`GETFB2` sur le plan primaire réussit pour tout le monde mais rend des poignées
GEM **nulles** sans `cap_sys_admin` ; avec, elles apparaissent et
`drmPrimeHandleToFD` exporte le tampon (8,9 Mo pour du 1080p, plus que 1920×1080×4
: il est tuilé). Son modificateur `0x200000010467b04` se décode en GFX11, tuile
64K_R_X, **DCC activé avec retile**. Et là : **radeonsi refuse l'import VA-API de
ce tampon** (`invalid parameter`), alors qu'il accepte un modificateur linéaire ou
invalide. L'attribut `VASurfaceAttribDRMFormatModifiers` qu'il annonce sert à
l'allocation, pas à l'import. EGL, sur le même pilote, liste ce modificateur parmi
ses six formats d'import XRGB8888 — et son import a d'abord échoué aussi, en
`EGL_BAD_MATCH`, parce qu'un tampon DCC porte **trois plans** (les pixels, puis deux
de métadonnées de compression aux offsets 8 847 360 et 8 896 512) et que je n'en
passais qu'un. Tous les plans passés, l'import réussit.

La chaîne est donc celle de Sunshine, et elle calque le chemin Windows shader pour
shader : **plan KMS tuilé → EGLImage → deux passes GLES écrivant les plans d'une
surface NV12 allouée par VA-API → encodeur**. La surface est exportée par
`vaExportSurfaceHandle` en deux couches, R8 et GR88, importées chacune comme sa
propre EGLImage et attachée à son propre framebuffer — le même tour que les vues
`R8_UNORM`/`R8G8_UNORM` de D3D11 sur un NV12. La propriété est inversée par rapport
à Windows : c'est **l'encodeur qui possède la surface** et le convertisseur qui
rend dedans, parce que VA-API alloue les siennes.

Le vertex shader **ne retourne pas Y**, contrairement au HLSL : un framebuffer GL
adossé à un DMA-BUF a sa première ligne mémoire en NDC y = −1, et écrire uv (0,0)
en (−1,−1) fait correspondre première ligne source et première ligne cible.
**Vu le 05/09** : le flux d'une session complète décodé par ffmpeg sur la machine
donne le bureau GNOME droit, barre en haut, dock à gauche, texte lisible, orange
Ubuntu et icône Chrome aux bonnes couleurs (donc l'ordre B/R du XRGB est juste).

Vérifié (`test_linux_pipeline`, qui tourne sur toute machine et se déclare sauté
sans écran actif ou sans capacité) : `KmsCapture` liste les connecteurs et leur
mode exact (clock/htotal·vtotal, pas le vrefresh arrondi), capture le plan
primaire en 3 plans, lit le **plan curseur** (256×256 ARGB linéaire — présent ici,
absent sur la VM Debian) ; `GlConvert` convertit et la relecture donne **luma
25..235** ; `VaapiEncoder` produit une **keyframe H.264 de 50 Ko avec SPS, PPS et
IDR** puis un delta de 33 Ko. 1938 vérifications vertes sous Linux, 2013 sous
Windows après l'extraction des types partagés (`CaptureTypes.h`, `CursorDraw.h`,
`EncoderOutput.h`) hors des en-têtes D3D11.

Deux faits mesurés valent d'être retenus :

- **L'horodatage du vblank est prédit**, pas lu : le noyau le calcule depuis la
  position de balayage et il devance le réveil de ~400 µs. Plafonné à
  « maintenant » pour la même raison que sur WGC — le gouverneur de lien lit le
  délai aller simple sur (arrivée − présent).
- **radeonsi 23.2 n'offre pas d'intra-refresh** par colonnes
  (`VAConfigAttribEncIntraRefresh` non supporté) : sur Linux AMD la récupération
  reste par keyframe, et `intraRefreshEnabled()` le dit. **Revu le 29/09/2026
  (§32.24)** : sous Mesa 26.2.3, radeonsi l'offre, en H.264 comme en HEVC, et
  `VaapiEncoder` la prend.

Les en-têtes SPS/PPS sont **écrits par le pilote** depuis les paramètres de
séquence, VUI comprise (`bitstream_restriction` — la leçon B8 — et timing). Les
en-têtes empaquetés que FFmpeg envoie ne le sont pas tant qu'une mesure ne montre
pas qu'il y manque quelque chose.

#### 19.9 Le plan curseur ne se lit qu'en client atomique

Le paragraphe ci-dessus disait que le plan curseur était lu ; il ne l'était pas.
L'état d'un plan — `FB_ID`, `CRTC_X`, `CRTC_Y` — vit dans ses **propriétés**, et
le noyau les rapporte à **zéro** à un client qui n'a pas demandé
`DRM_CLIENT_CAP_ATOMIC`, quoi que le compositeur ait réellement posé dessus.
`KmsCapture::start()` ne demandait que `UNIVERSAL_PLANES` — qui suffit à *lister*
les plans, d'où l'illusion : le plan curseur était trouvé et annoncé dans le log,
et `updateCursor()` lisait ensuite `FB_ID = 0` à chaque tour, donc « pas de
framebuffer », donc **pointeur invisible, pour toujours et en silence**.

Conséquence pour le spectateur : **aucun client n'avait de souris** sur un hôte
Linux. Rien à composer dans l'image pour un téléphone (où le pointeur ne peut
être que gravé, `_sendCursorMode`), et aucune forme à envoyer à un navigateur de
bureau pour qu'il la dessine. Mesuré sur la 780M sous GNOME le 07/09 : la même
lecture donne `FB_ID = 0` sans le cap et `FB_ID = 162` (256×256, encre 18×24)
avec. La correction est la ligne `drmSetClientCap(m_Card,
DRM_CLIENT_CAP_ATOMIC, 1)` : on ne fait **jamais** de modeset, le cap ne change
donc que ce qu'on a le droit de *lire*, et un pilote sans atomic le refuse sans
rien empirer.

Ce que la vérification d'origine avait manqué, et qui est maintenant dans
`test_linux_pipeline` : le test relit le plan curseur **par son propre fd**, cap
atomique compris, et exige que les deux réponses concordent — si le compositeur
a un framebuffer de curseur sur ce CRTC, la capture doit le voir (`visible`,
taille non nulle, encre non nulle). Sans le correctif il échoue en 4 points,
avec il passe ; sauté honnêtement si le compositeur n'a pas de plan curseur ou a
caché le pointeur. La leçon vaut au-delà de ce bug : **une capture qui « trouve »
un plan ne prouve rien tant que son contenu n'a pas été relu par un second
chemin.**

#### 19.10 Le texte : la seule entrée qui doive connaître la disposition (08/09/2026)

Le §19.2 dit que la disposition de l'hôte ne doit pas entrer dans la table des
touches, et c'est vrai — pour les **positions**. Il existe une entrée qui n'est
pas une position : `Type::Utf8Text`, ce qu'envoie le clavier tactile d'un
téléphone, qui n'a aucune position à envoyer. Elle tombait dans le `break` vide
de `UinputInput::inject`, avec pour commentaire « needs a layout-aware path that
does not exist on Linux yet ». Résultat pour le spectateur : **sur un hôte Linux,
un mobile ne tapait rien** — pas un caractère — pendant que les flèches, Échap et
Retour arrière du bandeau passaient, eux, parce que ce sont des positions.

Windows et macOS injectent le **caractère** (`KEYEVENTF_UNICODE`,
`CGEventKeyboardSetUnicodeString`) : l'hôte n'a besoin d'aucune touche capable de
le produire. Linux n'a pas d'équivalent. uinput rapporte une position, et c'est
le compositeur qui la lit à travers la disposition de l'utilisateur. Pour faire
apparaître un `a` il faut donc savoir quelle touche produit un `a` **ici** — sur
l'hôte AZERTY de référence, celle qu'un clavier US appelle Q. Une table US aurait
tapé `q`.

`XkbTextMap` compile la disposition avec **libxkbcommon** et parcourt une fois
chaque touche, niveau par niveau (`xkb_keymap_key_get_syms_by_level`), pour bâtir
`caractère → touche + modificateurs`. Trois décisions valent d'être écrites :

- **Quels modificateurs on accepte de tenir** : Shift et Mod5 (AltGr), rien
  d'autre. `xkb_keymap_key_get_mods_for_level` peut proposer un masque contenant
  Lock ; atteindre une majuscule en basculant le Verr. Maj. laisserait le clavier
  de l'hôte dans un état que le spectateur n'a pas demandé et ne voit pas. Un
  masque qu'on refuse est un niveau qu'on n'utilise pas.
- **D'où vient la disposition** : `XKB_DEFAULT_*` si la session la pose, sinon
  `/etc/default/keyboard`, sinon le défaut de libxkbcommon. Les noms sont passés
  explicitement plutôt que laissés à libxkbcommon, qui lit l'environnement par
  `secure_getenv` — vide pour un processus porteur d'une capacité ambiante
  (§19.8). Le résultat est **journalisé** (« fr+azerty (from /etc/default/keyboard),
  115 caractères atteignables ») : c'est une supposition sur le bureau de
  quelqu'un, et si un hôte tape la mauvaise lettre, la ligne dit en un coup d'œil
  quelle disposition a été crue. ⚠️ GNOME garde sa propre copie du réglage dans
  dconf ; un utilisateur qui change de disposition **après** l'installation peut
  la faire diverger du fichier. La lire voudrait dire lancer `gsettings` en fils
  d'un processus qui porte `CAP_SYS_ADMIN` — pire échange que de se tromper sur
  un hôte qui peut poser `XKB_DEFAULT_LAYOUT`.
- **Le repli pour ce qui n'est sur aucun niveau** : les touches mortes. Un `ê` n'a
  pas de touche sur un clavier français, et une personne le tape en deux temps —
  accent circonflexe, puis `e`. La carte fait pareil : les keysyms morts, que le
  parcours ignore puisqu'ils ne portent aucun caractère, sont gardés à part, et
  une table des précomposés Latin-1 les recompose. Les deux moitiés doivent être
  atteignables, sinon on ne tape rien : la moitié d'un caractère est pire que
  rien. Pour ce qui reste hors d'atteinte — un emoji, un idéogramme — il n'y a
  **pas** de repli : uinput n'a pas de mode Unicode, et la séquence
  Ctrl+Maj+U d'IBus n'existe que dans certaines applications, où la manquer
  écrirait `u` suivi de chiffres dans le champ visé. Le caractère est abandonné,
  et une ligne de log le dit une fois.

libxkbcommon est chargée par `dlopen`, jamais liée — la propriété du §19.1 tient
donc toujours : le moteur compile sur un Linux sans le moindre paquet `-dev`, et
un hôte sans la bibliothèque garde exactement le comportement d'avant (le texte
est ignoré, les touches marchent). Le garde-fou « chargée mais n'exporte pas les
appels » n'est pas décoratif : il a attrapé, à la première exécution,
`xkb_keymap_min_key_code` — qui s'appelle en réalité `xkb_keymap_min_keycode`.

⚠️ `Type::LockKeySync` reste ignoré sous Linux, **sciemment** : aligner les
verrous de l'hôte reviendrait à basculer Verr. Maj. et Verr. Num. sur un vrai
bureau depuis un état que le client croit connaître.

Vérifié sur le bench-mini (GNOME Wayland, `fr+azerty`) : `a` → touche 16 (le Q d'un
clavier US), `q` → 30, `1` → touche 2 + Shift (les chiffres sont en niveau haut
sur AZERTY), `@` → touche 0 + AltGr, `é` en direct sur la touche 2, `ê` = touche
morte 26 **puis** touche 18. `test_xkb_text_map` tient les deux moitiés : le
décodage UTF-8 et la lecture de `/etc/default/keyboard` sont testés partout
(octets tronqués, séquence à quatre octets, valeurs non guillemetées), la carte
réelle seulement là où il y a une libxkbcommon — et elle exige l'alphabet dans
les deux casses, les chiffres, l'espace, la majuscule **sur la même touche** que
la minuscule plus un modificateur, et aucun modificateur hors Shift/AltGr.

### 19.5 La couture plateforme : `LinuxProbe` et `LinuxSession` (05/09/2026)

`Unimplemented.cpp` ne sert plus sous Linux dès que les bibliothèques graphiques
sont là (`MW_NATIVE_PLATFORM "linux"`). La sonde répond aux trois questions de
`WindowsProbe` depuis trois autres endroits : les GPU depuis `/dev/dri/card*`
(PCI vendor/device et nom de pilote par libdrm, nom lisible extrait de la chaîne
vendeur VA-API — « AMD Radeon Graphics (gfx1103_r1) »), les écrans depuis KMS
(connecteurs branchés, mode exact, le primaire = celui à l'origine du bureau), les
encodeurs depuis VA-API sur le render node. **Seul H.264 est annoncé** tant que
`VaapiEncoder` n'a pas de chemin HEVC/AV1 — la leçon B7 : une capacité que
l'encodeur n'honore pas est une session morte à l'init ; le silicium HEVC et AV1
est dit dans le log seulement. `hasInteractiveSession()` n'a plus le sens Windows
(« ce processus atteint-il un bureau ? ») mais « y a-t-il un CRTC allumé ? » —
KMS capture qui que ce soit et sans serveur d'affichage, c'est le point. La
capacité `cap_sys_admin` est vérifiée une fois dans la sonde pour que la carte
d'hôte dise pourquoi, plutôt qu'un échec au clic.

`LinuxSession` est le portage étage par étage de `WindowsSession` — même thread
unique sans file, même garde de cadence, même plancher écran fixe et rafale de
raffinement, mêmes trois couches de débit, même redémarrage sur écran perdu — avec
les différences propres à la plateforme marquées là où elles vivent : l'image que
le chemin pointeur-seul reconvertit est **le dernier tampon KMS tenu par son fd**,
pas une copie (`KmsCapture::acquire` ne ferme le tampon précédent qu'au moment
d'exporter le suivant) ; l'encodeur possède la surface ; pas d'invalidation de
référence ni d'intra-refresh sur radeonsi (une image perdue coûte une keyframe, et
`SessionInfo` le dit) ; clavier/souris uinput et manette uinput construits comme
`Win32Input`/`VigemGamepad` ; le son par PipeWire depuis le 05/09 au soir (§19.7) ;
pas de HDR.

**Le piège qui a coûté la première image** : la session a d'abord produit un flux
de 1,5 Ko pour 13 images — du noir pur, luma 0, pas 16. Le contexte EGL est rendu
courant par `init()` sur le thread qui construit la session, et `convert()` tourne
sur le thread de capture : sans contexte courant **tout appel GL est un no-op
silencieux**, `glGetError()` compris, et l'encodeur lit une surface que rien n'a
écrite. Le test pipeline, sur un seul thread, ne pouvait pas le voir. Le
convertisseur lie maintenant le contexte au thread qui l'appelle
(`makeCurrent`), le relâche à la fin de `init()`/`bindTarget()` et le thread de
capture le rend avant de finir (`detachThread`), sinon `stop()` sur l'autre thread
se heurte à `EGL_BAD_ACCESS`.

Vérifié (`test_linux_session`, une session par `NativeHost::probe()` →
`createSession()` → 2,7 s → fichier) : 13 images, 2 keyframes (la première et
celle demandée), ordre des numéros tenu, latence hôte au pire 8,1 ms, 560 Ko, et
l'image décodée décrite en 19.4. 1952 vérifications vertes sous Linux, 2005 sous
Windows (les deux tests Linux s'y déclarent sautés). ffmpeg n'intervient que sur le
banc, pour regarder le flux : le paquet n'en dépend pas.

### 19.6 Ce qui reste

~~Le portail PipeWire en repli~~ : **fait, câblé et vérifié en flux navigateur
réel (§19.15)** — une machine sans capacité streame, et le jeton de consentement
est rangé, donc **un clic par installation**.
~~Le paquet~~ : traité en §19.8 le 05/09 au soir (constaté le même jour : le job
Linux de `release.yml` n'installait aucune des `-dev`, le `.deb` et le `.rpm`
publiés embarquaient le stub). ~~HEVC~~ : §19.11. ~~Le premier flux navigateur,
image et son~~ : §19.12. ~~L'invalidation de référence~~ : §19.14.

**Ce qui reste pour une prochaine version**, par ordre de ce que l'utilisateur
sent — le détail et le pourquoi de chacun sont en §19.16 :

| # | Manque | État |
|---|---|---|
| 1 | **Couper le son côté hôte** | rien d'écrit ; `HostMute` est Windows seulement, donc `mute_host_audio` — coché par défaut chez le client — est ignoré en silence et le son joue dans la pièce |
| 2 | **Le clic de consentement rejoué** | le sens serveur → worker est prouvé en flux réel ; le sens retour (un grant **neuf** jusqu'à `settings.json`) attend un dialogue humain |
| 3 | **AV1** | écrit, ⛔ bloqué par le pilote (§19.13) ; à rouvrir sur un Mesa plus récent, ce qui demande de mettre à jour le banc |
| 4 | **Le multi-écran absolu** | corrigé et testé unitairement (§23.3), **jamais exécuté sur un vrai hôte Linux à deux écrans** |
| 5 | **HDR** | inexistant : `LinuxProbe` pose `hdrActive = false` en dur, la capture est XRGB 8 bits, rien en aval n'existe |
| 6 | **Wayland et le pointeur relatif** | ⛔ sans solution par conception — aucun client Wayland ne peut lire ni déplacer le pointeur d'un autre |

### 19.11 HEVC par VA-API (08/09/2026)

`renderHevc()` était un refus écrit d'avance, et `LinuxProbe` n'annonçait donc que
H.264 en le disant — « HEVC (silicon, not yet driven) ». Le silicium du 780M
encode HEVC depuis toujours ; il manquait le jeu de paramètres. Écrit en miroir
des choix déjà mesurés sur H.264 (pas de B-frames, une référence, GOP infini,
CBR au VBV d'une image), avec cinq différences qui ne sont pas cosmétiques :

| Point | H.264 | HEVC | Pourquoi ça compte |
|---|---|---|---|
| Unité de bloc | macrobloc 16 | **CTB 64** | la bande d'intra-refresh se compte dedans |
| Horloge VUI | tick = un **champ** (`time_scale = 2·fps`) | tick = une **image** (`= fps`) | un facteur 2 sur la cadence annoncée au décodeur |
| `slice_type` | I 2, P 0 | **I 2, P 1** | la numérotation est inversée entre les deux specs |
| Recadrage | fenêtre de crop dans la séquence | **rien** | la taille codée doit être un entier de blocs minimaux |
| MV temporels | — | éteints, `collocated_ref_pic_index = 0xFF` | prédire depuis l'image collocated casse de plus quand une référence se perd, or ce flux répare en **nommant** l'image perdue (E2) |

Le recadrage absent est le seul point qui change un comportement visible : le
buffer de séquence VA-API HEVC n'a pas de fenêtre de crop **et** c'est le pilote
qui écrit le SPS, donc `init()` aligne la taille codée à 8 **vers le bas** pour ce
codec et le journalise. Toute résolution courante en est déjà un multiple —
1920×1080 compris — donc ça ne coûte rien là où ça ne coûte rien, et ça perd au
pire 7 colonnes ou lignes là où l'alternative serait de donner au décodeur une
taille que le flux ne sait pas exprimer.

La VUI porte `bitstream_restriction` comme sur H.264 : c'est la leçon B8 (200 ms
de latence de décodage sur NVENC faute de ce drapeau) et elle vaut pour tout codec
remis au décodeur matériel d'un navigateur.

**Mesuré sur le bench-mini** (Radeon 780M, Mesa 23.2.1, libva 1.14) : test de session
84 images / 2 keyframes / première image clé 26 Ko, relues par `ffprobe` en
`hevc / Main / 1920×1080 / 84 images` ; **flux navigateur réel** depuis
Chrome/Windows, « Negotiated video codec: hevc », décodage matériel, 1920×1080 à
60 fps, **8,4 ms**, image juste. Le pilote émet VPS/SPS/PPS à chaque IDR et le
correctif HEVC du relais les trouve sans avoir à les reconstruire.

Voir §19.13 pour AV1, et §19.14 pour l'invalidation de référence.

### 19.15 Le portail ScreenCast : la poignée de main, et ce que coûte le consentement (08/09/2026)

Le portail est la **seule** route de capture d'une AppImage, qui ne peut porter
aucune capacité (§19.8). Il n'est joignable que par **D-Bus**, ce qui a imposé
une décision de licence avant toute ligne de code — voir `native-host/LICENSE.md`
§ « L'exception sd-bus » : les trois façons de parler D-Bus en C sont copyleft ou
pires, et **sd-bus (LGPL-2.1+) a été accepté le 08/09 comme exception bornée**, à
ce seul chemin, plutôt que d'écrire 600 à 1000 lignes de protocole.

**Le piège de la conversation.** Un appel au portail ne rend pas la réponse : il
rend un **chemin d'objet**, et la réponse arrive plus tard en signal dessus.
Chaque étape doit donc dériver ce chemin — depuis notre propre nom de bus unique,
« : » retiré et points en underscores —, s'y abonner, **puis** appeler. Un
abonnement posé après l'appel rate la réponse et attend indéfiniment.

Ce qui est négocié, et pourquoi : `types=1` (un moniteur, jamais une fenêtre —
c'est un hôte de bureau) · `cursor_mode=4` **METADATA**, donc le pointeur arrive
*à côté* de l'image et le client continue de dessiner le sien, comme sur toutes
les autres plateformes · `persist_mode=2`, qui est ce qui achète le silence.

**Le consentement, mesuré** (bench-mini, portail ScreenCast v4, GNOME 42) :

| Passage | Résultat |
|---|---|
| premier, dialogue accepté à la main | nœud 67, 1920×1080, **restore token de 37 octets** |
| rejoué avec le jeton | **aucun dialogue** |
| rejoué encore | aucun dialogue — le jeton survit à son usage |
| **sans** le jeton | **le dialogue revient** |

Donc : **un clic par installation, pas par session**, à condition de ranger le
jeton. C'est la différence entre un repli acceptable et un produit qui demande la
permission à chaque lancement — et c'est pour ça que `persist_mode=2` n'est pas
un détail.

**Où le jeton est rangé, et par quel chemin (08/09/2026).** Le worker n'a pas de
fichier de réglages : il en est un processus séparé, et sur une install en
service il ne tourne même pas sous le même jeton. Le jeton fait donc l'aller-
retour que fait déjà le TTL de l'hôte, à ceci près qu'il est **persisté** :

```
AppSettings["portal_restore_token"]
  → cfg["portalRestoreToken"]            (la ligne de configuration du worker)
  → StreamSession::setPortalRestoreToken
  → NativeMediaEngine::StartParams
  → SessionConfig::portalRestoreToken
  → PortalCapture::setRestoreToken       → aucun dialogue

et au retour, seulement si le portail a VRAIMENT demandé :
  Session::setPortalGrantCallback        (posé AVANT start(), voir plus bas)
  → NativeMediaEngine::portalGrantReceived
  → StreamSession::portalGrantReceived
  → événement JSON {"event":"portalGrant"} sur stdout
  → StreamWorkerHost::portalGrantReceived
  → AppSettings::setPortalRestoreToken
```

⚠️ **Le rappel se pose avant `start()`**, pas après comme tous les autres :
demander un screencast **est** ce qui lève le dialogue, donc le consentement
revient de l'intérieur de `start()`. Un écouteur posé ensuite n'est pas en retard
d'un peu, il a manqué le seul appel qu'il y aura jamais.

⚠️ **Et le grant n'est signalé que s'il est nouveau.** Mesuré le 08/09 : GNOME 42
**ne fait pas tourner le jeton** — rejouer un jeton valide rend exactement la même
chaîne (md5 identique avant/après). Sans la garde `granted != stocké`, chaque
session réécrirait `settings.json` pour rien. Un jeton identique est donc le cas
**normal** d'une machine qui marche, et `setPortalRestoreToken` ne réécrit pas le
fichier quand la valeur ne change pas (vérifié par un test qui réécrit le fichier
en JSON compact et regarde s'il a été ré-indenté).

Le consentement est rangé **où qu'il apparaisse** : la session du propriétaire et
celle d'un joueur invité le remontent toutes les deux, parce qu'il appartient à
la **machine** et non au spectateur — sans quoi l'écran de l'hôte lèverait un
dialogue que personne n'est là pour répondre.

**La route complète, et ce qu'elle décide en chemin.** `IScreenCapture` sépare
les deux sources — le lecteur de scanout et le portail — parce que la session
pose les mêmes questions aux deux ; tout ce qui est propre à une route (chemin
de carte et connecteur pour KMS, jeton de consentement pour le portail) reste
sur la classe concrète. La sonde décide (`caps.capture`), `ResolvedTarget` le
porte, la session obéit : une règle, un endroit.

⚠️ **La liste d'écrans se réduit à une entrée sur la route portail**, et ce n'est
pas une simplification : le portail ne laisse pas l'application choisir un
moniteur, c'est l'utilisateur qui le fait dans le dialogue. Offrir les trois
écrans énumérés serait offrir trois boutons qui font la même chose.

⚠️ **Et une décision qui ne peut pas être prise avant la négociation** : le
compositeur peut donner de la **mémoire partagée** plutôt qu'un DMA-BUF, et EGL
ne sait pas importer ça. La paire GPU devient alors impossible quoi que le
Selector ait choisi, et la session bascule sur la paire CPU en le disant — donc
**H.264 seulement**. GNOME 42 fait exactement ça sur le banc. `SessionInfo`
rapporte l'encodeur **réel**, pas celui choisi, pour que le client ne se voie pas
promettre un codec qu'il ne recevra pas.

**Corrigé le 29/09/2026 (C13.3 bis, §32.21)** : GNOME donnait de la mémoire
partagée parce que ce moteur ne lui demandait rien d'autre. Un compositeur ne
donne un DMA-BUF qu'à un client qui annonce les modificateurs qu'il importe. La
session les annonce maintenant (`PortalCapture::offerDmabuf`), et GNOME 46
donne un DMA-BUF : la paire GPU tourne par le portail comme par le scanout. La
mémoire partagée reste le repli d'un compositeur qui n'en prend aucun, et la
conversion Vulkan sait la lire (§32.22).

#### 19.15.1 ⚠️ Le codec doit suivre la paire (08/09/2026)

Trouvé au **premier vrai flux navigateur** par le portail, et c'est le genre de
défaut qu'aucun test unitaire n'aurait attrapé parce que le test choisissait son
codec : Chrome préfère HEVC, le Selector le lui accorde — le GPU offre bien HEVC
—, puis la paire CPU répond « OpenH264 encodes H.264 only, not HEVC » et la
session meurt avant la première image. Sur une AppImage, c'est **toute** première
session de tout utilisateur.

Le correctif est en deux endroits, et le second est le vrai :

1. `buildPipeline` abaisse le codec **avec** la paire : si la mémoire partagée
   force le CPU et que le codec choisi n'est pas H.264, la session encode en
   H.264 et le journalise. `SessionInfo::codec` rapporte alors H.264 — le client
   apprend ce qu'il va recevoir, jamais ce que le GPU aurait pu faire.
2. ⚠️ **`NativeHost::createSession` ne réduit plus `clientCodecs` au codec
   choisi.** Cette ligne (`resolved.clientCodecs = {selection.codec}`) était une
   normalisation bien intentionnée — « le backend ne rejoue pas la politique » —
   mais le choix voyage déjà par `ResolvedTarget::codec`, donc elle n'achetait
   rien et coûtait la vérité : elle faisait dire à la configuration que le client
   ne décode qu'un seul codec. Avec la liste réduite, la seule réponse
   disponible à « ce client prendrait-il du H.264 ? » était « il a demandé du
   HEVC », et la session mourait sur une machine dont le navigateur décode le
   H.264 parfaitement.

La règle générale, qui vaut au-delà de Linux : **une contrainte découverte tard
doit pouvoir être arbitrée tard**, et pour ça les faits sur le client (ce qu'il
décode) doivent survivre jusqu'au backend — seules les *décisions* se
normalisent. Un client qui n'aurait nommé que HEVC est refusé explicitement,
avec la raison ; il n'est pas servi un flux qu'il ne peut pas décoder.

**Mesuré, les deux routes** :

| Route | Comment | Résultat |
|---|---|---|
| KMS | binaire avec la capacité | 3587/3587, session VA-API HEVC inchangée |
| **Portail** | **une copie du binaire, donc sans capacité — l'AppImage exactement** | bascule automatique, nœud ouvert **sans dialogue**, mémoire partagée détectée, paire CPU, codec abaissé, **3535/3535** dont la descente HEVC→H.264 et le refus d'un client HEVC-seul |
| **Portail, vrai navigateur** | Chrome/Windows → l'app complète sur le banc, jeton lu dans `settings.json` | ouverture **sans dialogue**, `CODEC: H264`, **1920×1080, 8,1 ms**, le bureau GNOME du banc à l'écran |

⚠️ **Le piège AT_SECURE**, qui a d'abord fait croire à l'absence de portail : une
capacité de **fichier** met le processus en `AT_SECURE`, et libsystemd refuse
alors l'adresse du bus venue de l'environnement (`secure_getenv`). Mesuré sur les
trois cas — sans capacité `AT_SECURE=0`, portail v4 ; capacité sur le binaire
(le cas de `mw-native-tests`) `AT_SECURE=1`, « No medium found » ; **à travers
`moonlightweb-launch`, comme le paquet livre, `AT_SECURE=0`, portail v4**. L'app
livrée est du bon côté parce que le lanceur passe la capacité par un exec qui ne
gagne rien, ce qui n'est pas un exec sécurisé (§19.8).

### 19.13 AV1 : écrit, et bloqué par le pilote (08/09/2026)

Le jeu de paramètres AV1 est écrit — séquence, image, groupe de tuiles, sur le
même modèle que les deux autres. Il n'est **pas annoncé** par la sonde, et ce
n'est pas de la prudence : c'est mesuré.

Un profil avec un point d'entrée d'encodage n'est pas un encodeur configurable.
L'encodeur AV1 se décrit par des attributs à lui — `VAConfigAttribEncAV1` (52)
et ses deux extensions — et **Mesa 23.2.1 sur gfx1103 répond « non supporté » à
ces attributs tout en annonçant le profil**. Les deux bouts le confirment :

| Implémentation | Où elle s'arrête |
|---|---|
| la nôtre | `vaEndPicture` → « invalid VAContextID » |
| **FFmpeg 7.1.1**, complète, écriture des OBU comprise | « Driver does not support some wanted packed headers (wanted 0xb, found 0x3) » puis « **Attribute type:52 is not supported** » |

Qu'une implémentation de référence échoue sur le même pilote est ce qui tranche :
le manque est **du côté du pilote**, et annoncer le codec serait le bug B7. La
sonde le dit dans le log en distinguant les deux cas — « profile advertised, but
this driver has no AV1 encode attributes — unusable » ici, « silicon and
attributes, never driven here » sur une machine où ils existeraient.

Ce qui resterait à écrire le jour où un pilote les décrit : les **OBU d'en-tête**
de séquence et d'image en packed headers, avec les décalages de bits
(`bit_offset_qindex` et ses voisins) pointant dedans pour que le contrôle de
débit y écrive ce qu'il décide. C'est un écrivain de flux binaire, pas un
paramètre — et c'est la seule partie qui manque.

### 19.14 Invalidation de référence : une perte coûte un delta (08/09/2026)

`LinuxSession::invalidateReference` forçait une keyframe. Windows répare par un
delta depuis le 06/09 sur les trois encodeurs (E2) ; Linux était le seul à payer
une image clé entière à chaque perte — au moment précis où elle coûte le plus
cher, un lien qui souffre. Or **VA-API donne la liste de références à
l'application, image par image** : il n'y avait rien à demander au pilote, juste
un DPB à tenir et un choix à faire.

Cinq surfaces de reconstruction au lieu de deux (quatre références + la
courante), `max_num_ref_frames = 4`. Chaque slot retient l'image sous **ses deux
noms** : celui que le récepteur connaît (`EncodedFrame::frameNumber`, le seul
avec lequel il peut nommer ce qu'il n'a pas reçu) et celui du flux (`frame_num`
H.264 / POC HEVC, qui repart de la dernière IDR). Toute l'astuce est là :
l'invalidation parle la première langue, les buffers de paramètres parlent la
seconde. Invalider `n` invalide **`n` et toute la suite** — chaque image encodée
après `n` a pu prédire depuis elle.

**Mesuré** (HEVC réel, `mw_drop_test=120`) : 25 pertes nommées, 25 réparations
par delta, **0 repli sur keyframe**, 0 « Requesting IDR » du client, 0 erreur de
décodage, **une seule image clé dans toute la session** — celle d'ouverture.

Et la question qui compte vraiment — le pilote honore-t-il la liste, ou fait-il
comme AMF qu'il fallut juger sur l'image et non sur l'index (§9.10.1) ? Après ces
25 réparations, l'image du client est **identique à celle d'une IDR fraîche** :
signature de luma 16×9 en pleine résolution, écart moyen **0,043 niveau**, pire
cellule 0,2. Aucune dérive.

⚠️ La paire CPU (OpenH264) répond `false` : elle écrit sa propre liste de
références, et `SessionInfo` le dit au client comme avant.

### 19.12 Le premier flux navigateur depuis un hôte Linux : le son (08/09/2026)

L'image était prouvée deux fois (VM Debian par la chaîne CPU, bench-mini par
VA-API) ; **le son ne l'avait jamais été dans un navigateur**, seulement en test
unitaire par libopus. Relevé le 08/09 sur le bench-mini, tonalité 440 Hz d'amplitude
0,25 jouée dans le sink par défaut, mesure par `AnalyserNode` sur le `MediaStream`
que la page joue réellement :

| Tonalité côté hôte | Crête | RMS | Fondamentale |
|---|---|---|---|
| jouée | **0,2538** | 0,1732 | **445 Hz** (bin de 11,7 Hz) |
| **coupée** | **0** | **0** | — |
| rejouée | 0,2539 | 0,1764 | 445 Hz |

Une sinusoïde d'amplitude 0,25 a une RMS de 0,177 : ce qui sort du décodeur du
navigateur est le signal de l'hôte, pas un artefact de mesure — et l'A/B/A le
prouve mieux qu'une seule lecture, parce qu'une chaîne qui invente du bruit ne
sait pas se taire sur commande.

### 19.7 Le son : le moniteur de la sortie par défaut, par PipeWire (05/09/2026)

Le choix de la bibliothèque est une décision de licence avant d'être une décision
technique : **libpulse est LGPL et hors de la liste blanche** de `LICENSE.md`,
**libpipewire est MIT et dedans**. Ça tombe bien : PipeWire est le serveur audio
de tous les bureaux actuels (Fedora depuis 34, Ubuntu depuis 22.10, Debian depuis
12, Arch, SteamOS, Bazzite), et les applications PulseAudio y tournent par
`pipewire-pulse`. L'inverse n'est pas vrai, et c'est **la limite à connaître** : sur
une machine où PulseAudio tient encore la carte son — Ubuntu 22.04, notre banc
même —, le démon PipeWire tourne (pour les portails) mais son graphe n'a **aucune
sortie** ; le flux de capture est refusé (« no node available »), la session
streame en silence et le log dit en clair qu'il faut `pipewire-pulse`. Le tap
réessaie toutes les deux secondes : une sortie peut apparaître (HDMI branché,
serveur basculé).

La source (`src/audio/linux/PipeWireCapture`) est un `pw_stream` en capture avec
`stream.capture.sink = true` — **le moniteur d'une sortie, jamais un micro** — et
sans cible nommée, pour que le gestionnaire de session l'accroche à la sortie par
défaut et **la déplace** quand l'utilisateur change de sortie en cours de session
(le « suit le périphérique par défaut » de WASAPI, fait par le serveur). On demande
du float32 entrelacé, 48 kHz, deux canaux : l'adaptateur de PipeWire convertit
depuis ce que la sortie fait tourner, donc une sortie 44,1 kHz ou 5.1 coûte un
resampler dans le graphe et rien chez nous. Les tampons sont lus contre le format
**négocié**, jamais contre le format demandé ; un mono ou un 5.1 qui passerait
quand même est ramené en stéréo par `interleavedToStereo` (`AudioInterleave.h`,
mêmes trois décisions que le planaire, testé partout). PipeWire appelle sur son
propre thread, quand le graphe tourne : c'est une API *push* comme le tap de
ScreenCaptureKit, donc la cadence vit dans `PacedOpusSink` (§20.8), et libopus se
construit maintenant sous Linux aussi.

**Mesuré sur le bench-mini** (Ubuntu 22.04 basculé sur `pipewire-pulse` 0.3.48 pour
l'occasion, sink nul `mw_null` en sortie par défaut, une sinusoïde 440 Hz à −12 dBFS
en boucle dedans ; `test_linux_session`, 2,8 s) : format négocié **F32 entrelacé,
2 canaux, 48 000 Hz** ; **128 échantillons par tampon** (2,7 ms — le graphe a donné
moins que les 240 demandés) ; **135 168 échantillons en 1 056 tampons**, soit
48 kHz à l'échantillon près sur la durée ; **575 paquets, 0 jeté, 13 trames de
silence** — les 65 ms entre le départ du pacer et l'état *streaming* du flux, pas
une perte en régime ; et le signal **décodé en retour par libopus dans le test** :
crête **0,261**, RMS **0,175** pour une sinusoïde d'amplitude 0,25 (79 octets par
paquet ; le même test sans tonalité lit crête 0,000 et 3 octets par paquet).
Avant `pipewire-pulse`, sur le même banc : cadence parfaite, 3 octets par paquet,
crête 0,000 — exactement le symptôme de §20.8, cette fois pour une vraie raison
(pas de sortie dans le graphe).

Deux pièges de banc, retenus dans `mac-m1-bench`/la mémoire Linux : `pgrep -f` et
`pkill -f` attrapent **le shell SSH lui-même** dont la ligne de commande contient le
motif (la première tentative a tué son propre script avant de lancer la tonalité) ;
et libopus construit depuis un tarball sans `.git` s'annonce « libopus unknown » —
la version vient de `git describe`, rien à corriger côté module.

Le test de session décode désormais les paquets avec le même libopus et imprime la
crête : c'est la seule preuve, ce côté-ci d'un navigateur, qu'un tap capture du son
et pas un silence parfaitement cadencé. Au passage, **les tests de session étaient
muets depuis le 04/09** : `test_capabilities` remettait le puits de log à `nullptr`
pour prouver qu'il est optionnel et ne le restaurait pas — corrigé
(`installTestLogSink()`), les lignes « [native] » des sessions Linux et macOS
réapparaissent dans la sortie de `mw-native-tests`.

Reste : le premier flux Linux vers un vrai navigateur (image **et** son), le
`node.latency` que le graphe n'honore pas forcément (la file du pacer, 40 ms,
absorbe un quantum par défaut de 21 ms), et le paquet (§19.8).

### 19.8 Le paquet : la capacité, le lanceur, et ce que linuxdeploy ne doit pas embarquer (05/09/2026)

Le constat du matin : le job Linux de `release.yml` n'installait aucune des
bibliothèques de développement du backend — le CMake du module avertit et
construit le stub, et c'est le stub que chaque `.deb`, `.rpm` et AppImage publié
portait. Corrigé en ajoutant `libdrm-dev libva-dev libegl1-mesa-dev
libgles2-mesa-dev libgbm-dev libpipewire-0.3-dev` (tous présents sur l'image
`ubuntu-22.04` du job, vérifié sur le banc qui est la même distribution), et
surtout en rendant la régression **impossible en silence** : le job lit la sortie
de configuration et refuse de paquetiser si « Linux graphics backend ON » ou
« Linux audio (PipeWire » n'y sont pas, puis vérifie par `readelf` que le binaire
assemblé a bien `libdrm`, `libva`, `libEGL`, `libgbm` et `libpipewire-0.3` en
`NEEDED` — le stub n'en lie aucun. ⚠️ **Corrigé le 05/09 au soir, en rejouant le job
sur le banc** : la première version de la garde exigeait aussi `libGLESv2.so.2`, et
le binaire ne l'a **pas** — le module passe bien `-lGLESv2` et 33 symboles `gl*` sont
référencés, mais Qt6Gui tire `libOpenGL.so.0` (glvnd) plus tôt sur la ligne de lien,
qui exporte les mêmes points d'entrée GLES, et `--as-needed` écarte libGLESv2 comme
redondante. La répartition vers le GLES du pilote passe par glvnd dans les deux cas ;
la garde ne nomme plus libGLESv2, la dépendance `libgles2` du paquet reste (inoffensive,
et c'est ce que le module demande).

**Le piège, mesuré avant d'écrire une ligne.** §19.3 supposait « `setcap` sur le
binaire, comme Sunshine ». Un binaire qui *gagne* une capacité à l'exec est lancé
par glibc en **mode sécurisé** (`AT_SECURE`), et dans ce mode `$ORIGIN` n'est
développé que si le binaire vit dans un répertoire système (`/usr/lib`…). Notre
`MoonlightWeb` vit sous `/opt/moonlightweb/bin` et trouve son Qt embarqué par
`RUNPATH=$ORIGIN/../lib`, réécrit par linuxdeploy. Reproduit sur le bench-mini avec
un programme de trois lignes et sa bibliothèque à côté :

```
$ ./bin/app                          # answer 42
$ sudo setcap cap_sys_admin+p bin/app
$ ./bin/app
./bin/app: error while loading shared libraries: libanswer.so: cannot open shared object file
```

Le paquet aurait installé une application qui ne démarre plus. Sunshine ne le
rencontre pas : son binaire est dans `/usr/bin` et lie les bibliothèques du système.
Le même mode sécurisé ignore aussi `LD_LIBRARY_PATH`, donc aucun contournement par
l'environnement.

**Le lanceur.** `backend/packaging/linux/moonlightweb-launch.c`, 16 Ko compilés,
lié à la seule libc (qui vit, elle, dans un répertoire de confiance). C'est *lui*
qui porte `cap_sys_admin+p` — **permitted seulement**, la posture exacte de
Sunshine (`getcap /usr/bin/sunshine` sur le banc : `cap_sys_admin,cap_sys_nice=p`).
Il met la capacité dans son ensemble *inheritable* (`capset`, permis puisqu'il la
tient permitted), la lève dans l'ensemble **ambiant** (`prctl(PR_CAP_AMBIENT,
RAISE)`), et `execv` le `MoonlightWeb` **à côté de lui** (résolu par
`/proc/self/exe`, jamais par `PATH` : une capacité ne doit pas suivre un nom dans
un répertoire que quelqu'un d'autre écrit). Un exec qui ne gagne rien que son
parent n'avait déjà n'est pas un exec sécurisé : l'application démarre
normalement, rpath compris, avec `CAP_SYS_ADMIN` permitted, effective et ambiant.
Sans capacité sur le fichier (arbre construit à la main, AppImage), la levée échoue
et le lanceur exec simplement — la sonde dit ensuite que la capture est
indisponible et pourquoi. Mesuré sur le banc : avec la capacité sur le lanceur,
l'application charge sa bibliothèque `$ORIGIN` **et** lit
`CapPrm/CapEff/CapAmb = 0x200000` (bit 21) ; par un lien symbolique aussi (ce que
`/usr/bin/moonlightweb` devient) ; sans capacité, tout à zéro et `answer 42`.

**Ce que l'application en fait** (`common/LinuxCapabilities.cpp`, première ligne
de `main()`, quand le thread principal est encore seul — les capacités sont *par
thread* et les threads héritent de celui qui les crée) : elle **garde** permitted
et inheritable, **retire** effective, et **abaisse** l'ambiant — `LOWER` sur
`CAP_SYS_ADMIN` seulement, pas `CLEAR_ALL`, parce qu'une unité systemd peut avoir
donné `CAP_NET_BIND_SERVICE` par le même mécanisme et celle-là doit rester. Le
serveur HTTP, la signalisation, les relais tournent donc **sans** la capacité
effective, et **rien de ce que le processus lance** ne l'hérite — `xdg-open` et le
navigateur derrière, `gio`, un installeur — sauf un seul enfant : le worker natif,
auquel `StreamWorkerHost` la rend par `QProcess::setChildProcessModifier` (dans
l'enfant forké, avant l'exec, en appels bruts). Le worker, même binaire, même
`main()`, se confine à son tour ; et `KmsCapture` lève la capacité dans l'ensemble
effectif **de son thread** le temps d'un `GETFB2` — `ScopedSysAdmin`, quatre
sites, `capget`/`capset` bruts, pas de libcap (rien à lier, et le `capset` de
libcap synchronise tous les threads, l'inverse du but). La séquence a été rejouée
en C sous le vrai lanceur avant d'être écrite en C++ :

| Étape | CapPrm | CapEff | CapAmb |
|---|---|---|---|
| à l'entrée, tel que le lanceur le donne | ✔ | ✔ | ✔ |
| après le confinement | ✔ | — | — |
| enfant A, `fork`+`exec` ordinaire (= `xdg-open`) | — | — | — |
| enfant B, levée ambiante puis `exec` (= le worker) | ✔ | ✔ | ✔ |
| un thread pendant `ScopedSysAdmin` | ✔ | ✔ | — |
| le thread principal au même instant, et le thread après | ✔ | — | — |

Et la preuve sur le module lui-même : `mw-native-tests` avec `cap_sys_admin+p`
**seulement** (le banc posait `+ep` jusqu'ici) — capture KMS, conversion, encodeur
et session complète, **2133/2133**.

**Ce qui doit rester au système.** linuxdeploy embarque tout ce qui n'est pas sur
sa liste d'exclusion, et une bibliothèque de pilote embarquée est pire qu'absente :
`libva` charge `radeonsi_drv_video.so` et `libpipewire` ses modules de protocole
depuis des chemins **compilés dans la bibliothèque** — une copie faite sur Ubuntu
cherche sous `/usr/lib/x86_64-linux-gnu` et ne trouve rien sur le `/usr/lib64` de
Fedora. La liste de pkg2appimage (lue le 05/09) contient `libdrm.so.2`,
`libEGL.so.1`, `libgbm.so.1`, `libpipewire-0.3.so.0` mais **pas** `libva.so.2`,
`libva-drm.so.2` ni `libGLESv2.so.2` : ces trois-là sont exclues explicitement, et
le job vérifie qu'aucune des sept n'a atterri dans `AppDir/usr/lib`, pour qu'un
changement de la liste amont ne puisse pas en embarquer une sans bruit. En face,
les paquets les **déclarent** : `libdrm2 libva2 libva-drm2 libgles2 libgbm1
libpipewire-0.3-0` (+ `libcap2-bin` pour `setcap`) côté `.deb`, les sonames côté
`.rpm`, `libdrm libva mesa pipewire libcap` côté AUR. `libpipewire` n'est que la
bibliothèque cliente : une machine encore sous PulseAudio l'a aussi, et l'hôte y
streame muet avec son log (§19.7) plutôt que de ne pas démarrer.

**Le reste du paquet** : `/usr/bin/moonlightweb`, l'entrée `.desktop`, l'unité
systemd et le `systemd-run` du postinst pointent le lanceur ; le postinst pose
`setcap cap_sys_admin+p` sur lui **à chaque installation et mise à jour** — ni
dpkg ni rpm ne restaurent une capacité de fichier depuis la charge utile —, avec
`/usr/sbin:/sbin` ajoutés au `PATH` du gestionnaire de paquets, et un avertissement
lisible sinon (Fedora a `setcap` dans `libcap`, toujours présent ; openSUSE dans
`libcap-progs`) ; l'AUR a son `.install` pour la même raison (pacman non plus). Le
lanceur est aussi dans l'AppImage, où il n'est qu'un exec : une AppImage est un
montage FUSE `nosuid`, ce qui désactive aussi les capacités de fichier — **pas de
capture depuis une AppImage**, et `install.sh` le dit au moment de l'installer
plutôt que de laisser chercher une carte d'hôte qui n'apparaît pas ; le portail
PipeWire sera sa route. L'image Docker n'est pas touchée : sans écran ni GPU, le
stub y est le bon backend.

**Vérifié le 05/09 au soir, en rejouant le job Linux étape par étape sur le banc**
(bench-mini, Ubuntu 22.04 — la distribution de l'image `ubuntu-22.04` du job —, Qt
6.6.3 local, fpm 1.18, linuxdeploy `continuous` extrait sans FUSE) : configure « ON »
× 2, build sans warning, ctest 3/3, AppDir avec `libqoffscreen.so`, aucune des sept
bibliothèques système embarquée (66 libs dans `usr/lib`, `RUNPATH=$ORIGIN/../lib`),
`.deb` et `.rpm` de 44 Mo avec les quinze dépendances attendues. Puis `dpkg -i`
**par-dessus le 0.2.1 déjà installé** — le vrai chemin de mise à jour : `prerm` a
arrêté l'ancien `--autostart`, `postinst` a posé `cap_sys_admin=p` sur le lanceur
(rien sur `MoonlightWeb`), `/usr/bin/moonlightweb` et l'entrée `.desktop` pointent
le lanceur, et `systemd-run --user` a relancé l'app dans la session GNOME Wayland de
l'utilisateur (Qt en `xcb` par XWayland, tray créé, page admin ouverte). Le
processus final s'appelle `/opt/moonlightweb/bin/MoonlightWeb` (argv0 = chemin,
parent = systemd utilisateur) et porte exactement l'état de la table ci-dessus :
`CapInh 200000 / CapPrm 200000 / CapEff 0 / CapAmb 0`. Le log dit « `[caps]
CAP_SYS_ADMIN held (permitted): the screen can be captured through KMS; dropped
from the effective set; withheld from child processes except the native worker »
puis « Native host available: bench-mini — MoonlightWeb Host », et
`/api/native/status` répond `available`, `DRM/KMS`, `HDMI-A-1 — 1920×1080 · 60 Hz`,
`VA-API`, `AMD Radeon Graphics (gfx1103_r1)`, codecs `H.264` — la sonde a donc bien
levé la capacité par thread dans le serveur, sans qu'elle soit effective ailleurs.
`ldd` : Qt depuis `/opt/moonlightweb/lib`, libva/libOpenGL depuis le système.

**Reste non vérifié** : le job GitHub lui-même (au premier `workflow_dispatch` de
`release.yml`, plateforme `linux`, après le push) — la seule différence avec le
banc est Qt 6.11 à la place de 6.6.3 ; et le **premier flux navigateur** depuis cet
hôte (§19.6), qui est la prochaine étape.

**Une seconde capacité, `CAP_SYS_NICE` (28/09/2026, plan D3D12 §9-9 et §9-18).**
Sous un jeu qui sature le GPU, la conversion attend deux images du jeu : 46 ms sur
le 780M (banc §8o.1). Un contexte GPU au-dessus de la priorité normale passe devant
l'image suivante (23 ms en GLES), et amdgpu, i915 et xe ne le créent que pour un
processus qui tient `CAP_SYS_NICE` — Sunshine la porte déjà (`cap_sys_admin,
cap_sys_nice=p`). Elle suit exactement la route de l'autre : `setcap
cap_sys_admin,cap_sys_nice+p` sur le lanceur, levée ambiante des deux, confinement
des deux au démarrage (permitted et inheritable gardés, effective retiré, ambiant
abaissé), rendue au seul worker natif. Le moteur la lève sur son thread le temps
de créer le contexte (`platform/linux/ScopedCapability.h`, partagé désormais avec
`KmsCapture`) ; la priorité est fixée à la création, rien ne la garde ensuite.
`GlConvert` ne demande HIGH (`EGL_IMG_context_priority`) que s'il la tient :
Mesa 23.2 relit « HIGH » pour un contexte que le noyau a refusé, la réponse du
pilote ne prouve rien. `mw-native-tests linux_pipeline` sur le banc : « priority
normal (no CAP_SYS_NICE…) » avec `cap_sys_admin+p`, « priority high
(CAP_SYS_NICE) » avec les deux, 61/61 dans les deux cas. Un paquet installé avant
ne pose que `CAP_SYS_ADMIN` : la conversion y tourne à la priorité normale, et le
log dit pourquoi.

---

### 19.16 Ce qui manque encore à la plateforme Linux (08/09/2026)

Écrit après le premier flux navigateur complet par le portail, quand la
plateforme est utilisable de bout en bout : image, son, clavier, souris, manette,
réparation sans keyframe, et une route pour les machines qui ne peuvent pas lire
leur scanout. Ce qui suit n'est pas une liste de bugs, c'est ce qu'un hôte Linux
ne sait **pas encore** faire, et pourquoi.

**1. ~~Couper le son côté hôte — rien n'est écrit.~~** ✅ **livré le 08/09, voir
§19.17.** Ce paragraphe est conservé parce qu'il se trompait, et sur le point qui
décidait de tout : « couper le sink par défaut au **volume** couperait aussi la
capture, puisque le moniteur entend ce que le sink joue ». C'est la sémantique de
**PulseAudio**. PipeWire fait l'inverse par défaut, et la mesure le dit ; le
sink nul, annoncé ici comme la seule voie, n'est en fait que le repli.

**2. Le consentement du portail, dans le sens du retour.** Le trajet
`settings.json` → worker → portail est prouvé en flux navigateur réel : le jeton
est relu, rejoué, et la session s'ouvre sans dialogue (§19.15). Le trajet inverse
— un grant **neuf** qui remonte jusqu'à `settings.json` — est écrit, testé
unitairement, et ne peut être exercé qu'en levant un vrai dialogue, donc en
cliquant à la main sur la machine. C'est une vérification, pas un doute : sans
elle, le pire cas est que le dialogue revienne, ce qui est le comportement
d'avant.

**3. AV1 — bloqué ailleurs que chez nous.** §19.13. Rien à corriger ici ; il faut
un pilote qui décrive ses attributs d'encodage AV1, donc un banc plus récent
qu'Ubuntu 22.04 / Mesa 23.2.

**4. Le multi-écran absolu, jamais vu tourner.** `displayPointToDesktop()` et
`desktopToAbsoluteRange()` (§23.3) sont justes par construction et couverts par
`test_absolute_map.cpp`, mais aucun hôte Linux à deux écrans n'a jamais exécuté
ce code — le banc n'en a qu'un. Un test unitaire prouve l'arithmétique, pas la
convention du compositeur.

**5. HDR : inexistant, et ce n'est pas un oubli.** `LinuxProbe` pose
`hdrActive = false` en dur, la capture est XRGB 8 bits, et rien en aval ne sait
produire du P010 ni du Main10 par VA-API. C'est le chantier de §16 à refaire
entièrement côté Linux. ⚠️ Et le banc ne pourra rien en juger sans un écran HDR
branché dessus.

**6. Wayland et le pointeur relatif — ⛔ sans solution.** `X11Pointer` est chargé
en `dlopen` ; sur une session Wayland il ne trouve rien, et le rapatriement du
pointeur entre écrans n'a pas lieu. Ce n'est pas un manque à combler : **aucun
client Wayland ne peut lire ni déplacer le pointeur d'un autre**, par conception
du protocole. L'injection, elle, marche partout — uinput est un périphérique
noyau, que le compositeur voit comme une vraie souris.

**Et une contrainte assumée, à ne pas relire comme un manque** : PipeWire est
**requis** pour le son. Sur une machine encore sous PulseAudio pur, le graphe n'a
aucune sortie, le flux est refusé, la session streame en silence avec un journal
explicite et réessaie toutes les 2 s. C'est une décision de licence — libpulse
est LGPL, hors de la liste blanche de `backend/native-host/LICENSE.md` — pas un
défaut.

### 19.17 Couper le son côté hôte : le moniteur est en amont du volume (08/09/2026)

Troisième et dernière plateforme à recevoir `HostMute`, et la troisième réponse
différente à la même question — *où est le tap, par rapport au réglage qui fait
taire les haut-parleurs ?* Windows le prélève sur le moteur audio (§24), macOS sur
le flux applicatif (§20.14), Linux sur le **moniteur d'un sink** (§19.7). Aucune
des deux réponses précédentes ne se transporte, et celle que §19.16 avait écrite
d'avance était fausse.

#### La mesure, avant d'écrire

Banc bench-mini, WirePlumber 0.4.8 sur PipeWire 0.3.48, une tonalité 440 Hz à 0,25
jouée en continu, RMS de ce que rend le moniteur, 2 s par état. Deux sinks
mesurés côte à côte, le vrai et un nul :

| état | vraie sortie ALSA | sink nul |
|---|---|---|
| départ | 0,1755 | 0,1755 |
| sink **muet** | **0,1755** | **0,0000** |
| volume 0 % | **0,1755** | **0,0000** |
| restauré | 0,1755 | 0,1755 |
| `monitor.channel-volumes` | *absent* | `true` |

Une sinusoïde d'amplitude 0,25 a une RMS de 0,177 : le moniteur de la vraie
sortie rend le signal **entier**, muet ou pas.

La propriété est toute l'explication. `monitor.channel-volumes` décide si
l'adaptateur applique le volume et le mute du nœud à ses ports moniteur ; elle
vaut **false par défaut**. Sur toute sortie réelle — ALSA, HDMI, USB, Bluetooth —
le moniteur est donc pris **en amont** du volume, et un mute n'atteint jamais la
capture. Les seuls sinks qui la posent à `true` sont les sinks virtuels créés par
la couche de compatibilité PulseAudio, qui gardent exprès la sémantique de
Pulse — celle que §19.16 avait prise pour la règle générale.

#### Deux stratégies, choisies en lisant cette propriété

1. **SinkMute** — `monitor.channel-volumes` n'est pas vrai : on coupe la sortie
   par défaut. Rien ne bouge dans le graphe de l'utilisateur, son niveau de
   volume est intact, et ce qu'il voit est l'icône de haut-parleur barrée. C'est
   le cas de tout bureau ordinaire.
2. **NullSink** — le moniteur porte bien le volume, donc couper tuerait la
   capture avec (mesuré ci-dessus). Une sortie qui ne joue nulle part est créée
   (`support.null-audio-sink`, nœud `moonlightweb-host-muted`) et devient la
   sortie par défaut de la session : le gestionnaire de session **déplace les
   flux en cours** dessus, la pièce se tait, et le tap — qui suit la sortie par
   défaut — atterrit sur son moniteur.
3. **None** — l'hôte s'entend, et le journal dit pourquoi.

`engage()` avant l'ouverture du tap (la stratégie 2 déplace la sortie à laquelle
le tap s'attache), `release()` après sa fermeture (sinon le gestionnaire de
session ramènerait un tap encore vivant sur la vraie sortie).

#### Le piège qui a coûté deux passes : où s'écrit un mute

Le premier jet écrivait `SPA_PROP_mute` sur le **nœud** du sink. Vérifié de
l'extérieur pendant une vraie session, le résultat était : `node.mute=True`
pendant, `False` après — et `pactl get-sink-mute` répondait **`no`** tout du
long. Un mute que le bureau ne voit pas.

Ce que fait le bureau, lu au même endroit :

| | `pactl` | `node.mute` | `node.softMute` |
|---|---|---|---|
| au repos | no | False | False |
| après `pactl set-sink-mute 1` | **yes** | True | True |
| notre 1ʳᵉ version | no | True | False |
| notre version livrée | **yes** | True | True |

Un mute vit sur la **route de la carte** (`SPA_PARAM_Route`, avec l'index et le
`card.profile.device` du sink), pas sur le nœud : c'est là que les réglages du
système l'écrivent, là que l'icône le lit, là qu'une carte munie d'un mute
matériel l'applique en matériel. Le démon le répercute ensuite **lui-même** sur
le nœud, `softMute` compris — d'où la dernière ligne du tableau, obtenue par une
seule écriture. Un sink sans carte (virtuel) n'a pas de route : celui-là est
coupé sur son nœud, en écrivant les deux propriétés à la main.

⚠️ Ce détour n'est pas cosmétique. `softMute` est l'étage qui retire réellement
les échantillons envoyés au périphérique ; `mute` seul annonçait une sourdine que
personne n'appliquait. Et écrire là où le bureau écrit donne la seule preuve
disponible sur une machine sans oreilles : **l'état obtenu est identique, propriété
par propriété, à celui que produit le mute de l'utilisateur**. Le silence des
haut-parleurs n'est pas observable en logiciel — le seul consommateur de la sortie
d'un sink est le matériel — donc l'équivalence est la preuve, et c'est pour ça
qu'elle vaut le code qu'elle coûte.

#### Ce qui a été vérifié, et comment

- **Stratégie 1, vraie session** (sonde de banc, session KMS + VA-API complète) :
  `hostMuted=true`, `pactl` passe à `yes` pendant et revient à `no` après, le
  moniteur reste à **0,1767** pendant la sourdine, et l'audio encodé porte
  **80,5 octets/paquet** contre 3,0 en silence — la capture entend tout.
- **Stratégie 2, vraie session**, en forçant le cas (sortie par défaut = un sink
  nul, donc `monitor.channel-volumes = true`) : `moonlightweb-host-muted`
  apparaît, devient la sortie par défaut, **le flux déjà en cours migre dessus**
  (sink 44 → 2802), le son continue de partir à 80,4 o/paquet, et à l'arrêt la
  sortie par défaut est rendue **et notre sink a disparu**.
- Tests : **3610/3610** sur le banc Linux, **3431/3431** sous Windows.
- Ce que ni l'un ni l'autre ne prouve : que la pièce se tait. Le banc n'a pas
  d'enceinte branchée, et aucun logiciel ne peut écouter la sortie d'un sink.
  C'est le seul point qui attend une oreille, comme sur macOS (§20.14).

#### Deux propriétés qui tombent en prime

Le sink nul est créé avec `object.linger = false` : il meurt avec notre
connexion, donc **un worker tué ne laisse pas la machine sur une sortie
silencieuse** — ce que la version Windows, elle, ne garantit pas (§24). En
revanche un mute de stratégie 1 survit à un worker tué, exactement comme sous
Windows ; l'utilisateur le défait d'un clic, puisque c'est son propre mute.

#### Ce dont ça dépend

D'un gestionnaire de session qui publie l'objet metadata `default` — c'est ce qui
nomme la sortie par défaut. WirePlumber le fait, sur tous les bureaux actuels. Le
banc tournait encore sous `pipewire-media-session` 0.4.1, retiré depuis, qui ne
le fait pas : là, `pactl set-default-sink` sort en erreur, il n'y a aucune
metadata à lire, et `HostMute` répond `None` avec ces mots plutôt que de couper
un sink dont il ne peut pas prouver que c'est celui que l'utilisateur écoute. Le
banc a été basculé sur WirePlumber pour cette raison.

## 20. macOS : ScreenCaptureKit → VideoToolbox, sans étage de conversion (05/09/2026)

Le troisième backend, écrit sur bench-desk et construit sur le Mac M1 Pro de test
(macOS 15.6.1, SDK 15.5, Apple clang 17). Cinq fichiers Objective-C++ :
`SckCapture.mm`, `VtEncoder.mm`, `CgInput.mm`, `MacProbe.mm`, `MacSession.mm`, plus
la table clavier `MacKeyMap.h` (C++ pur, testée partout). Même couture plateforme
que Windows et Linux (`Probe.h`, `Session.h`), même boucle de session portée étage
par étage.

### 20.1 La chaîne, et pourquoi il n'y a pas de convertisseur

Sous Windows et Linux la capture livre le bureau tel qu'il est scanné (BGRA, ou un
DMA-BUF tuilé) et un shader le transforme en NV12 pour l'encodeur. ScreenCaptureKit
est la sortie du compositeur lui-même, et le compositeur sait écrire du NV12 : on
demande `'420v'` (4:2:0 bi-planaire, video range, matrice BT.709) à la résolution du
flux, et chaque image arrive comme un `CVPixelBuffer` sur IOSurface que VideoToolbox
lit en place — mise à l'échelle, conversion couleur et, si on le demande, pointeur
composé, déjà faits par WindowServer sur le GPU. La chaîne est donc **capture →
encodeur**, rien entre les deux ; la seule copie restante est le bitstream qui sort
de la VRAM (`copiesPerFrame = 1`, comme sur les deux autres OS).

SCK pousse ses images sur une file dispatch ; la boucle est écrite contre un
`acquire(timeout)` bloquant — c'est ainsi que DXGI et KMS répondent — donc la
dernière image est **parquée sous un mutex et remplacée** par la suivante si elle
n'a pas été prise : aucune file, la règle « dernière image, jamais d'arriéré ». Les
images `Idle` (rien n'a changé) sont comptées et ignorées ; `Complete` et `Started`
portent la sortie du compositeur. L'horodatage de présentation vient de l'attache
`SCStreamFrameInfoDisplayTime` (mach time), ramené sur l'horloge stable par « il y a
combien de temps ». La fréquence demandée à SCK est celle du panneau (120 Hz sur ce
M1 Pro, ProMotion) : la garde de cadence de la session voit chaque présent et décide
lesquels le flux porte, exactement comme sous Windows.

Pas de statut `PointerOnly` : avec le pointeur dans l'image, un mouvement de souris
**est** une nouvelle image (c'est ce que veut le mode composé) ; pointeur exclu, le
client dessine le sien, et la **forme** lui vient d'AppKit
(`NSCursor.currentSystemCursor`, rastérisé à l'échelle du panneau, haché ; nommé —
`default`, `text`, `pointer`, `ew-resize`… — quand son hachage est celui d'un
curseur standard appris une fois au démarrage). Le pointeur agrandi pour téléphone
(`cursorFramePx`) n'a pas de route ici : SCK dessine le pointeur à sa taille, et la
session le dit une fois dans le log.

### 20.2 VideoToolbox : ce qu'il a, ce qu'il n'a pas

Les mêmes décisions de latence que les quatre autres encodeurs : `RealTime`,
`AllowFrameReordering = false` (pas de B, une image en vol), `MaxKeyFrameInterval` et
sa durée poussés à « jamais » (les keyframes sont à la demande), `ExpectedFrameRate`,
`MaxFrameDelayCount = 0`, `PrioritizeEncodingSpeedOverQuality`, High/CABAC en H.264,
Main sans open-GOP en HEVC, matériel exigé
(`RequireHardwareAcceleratedVideoEncoder`). Le VBV est celui de `RateControl.h`
exprimé dans le vocabulaire de VideoToolbox : `DataRateLimits = [octets, secondes]`
avec une image de budget sur une image de temps. Le débit se change à chaud par
`AverageBitRate` + `DataRateLimits`, sans reconstruction. Chaque `encode()` **bloque**
jusqu'au bitstream (`CompleteFrames` puis attente du callback) : VideoToolbox est
asynchrone par construction, et l'attente de l'image que l'on vient de soumettre est
ce qui rend vraie la « une image en vol » de la boucle.

Ce qu'il n'a pas : ni intra-refresh, ni invalidation de référence, ni QP par image.
Une image perdue coûte une keyframe et `SessionInfo` le dit ; la rafale de
raffinement converge sur la taille et son plafond de passes seul (`RefineConvergence`
était déjà écrit pour un encodeur muet sur le QP — AMF fut le premier). Pas d'AV1 :
aucun encodeur Apple n'en produit. HEVC et H.264 seulement, dans cet ordre.

**AVCC → Annex B en place.** VideoToolbox sort des NAL préfixés de leur longueur, les
paramètres (SPS/PPS, VPS en HEVC) à part dans la description de format ; le
navigateur et tous les relais veulent de l'Annex B avec les paramètres devant chaque
keyframe. Un préfixe de longueur fait 4 octets, un start code aussi : une image
delta est **réécrite dans le tampon même de l'encodeur** et livrée sans copie ; une
keyframe — rare — est assemblée dans un tampon de travail, paramètres devant.

### 20.3 Les entrées : Quartz, et les deux choses que macOS laisse à l'émetteur

`CGEventPost` au HID tap, thread-safe et à la microseconde : la seule route. Deux
choses que Windows et Linux font pour nous et que macOS non :

1. **Les modificateurs sont des drapeaux, pas des touches.** Un appui sur Shift se
   poste en `kCGEventFlagsChanged` avec le nouvel état, et chaque événement clavier
   ou souris qui suit doit porter les modificateurs tenus dans son propre champ —
   WindowServer ne s'en souvient pas pour nous. `CgInput` tient le masque et
   l'estampille sur tout ce qu'il poste.
2. **Le double-clic se déclare, il ne se détecte pas.** Un second appui dans
   l'intervalle doit dire `clickState = 2`, sinon ce sont deux clics simples ; un
   déplacement bouton enfoncé est un `…Dragged`, pas un `MouseMoved`.

La table clavier `MacKeyMap.h` est écrite en chiffres (codes ADB, inchangés depuis le
premier Macintosh) et non en `kVK_*` pour être testée sur toute machine ; même
raisonnement position → position que les deux autres tables, la disposition de
l'hôte étant appliquée par l'OS. Win → Command, Alt → Option, Ctrl → Control ;
Impr. écran / Arrêt défil. / Pause vont là où un clavier Apple met F13–F15 ; la
touche menu et les touches média n'ont pas de place et sont écartées plutôt que
devinées. Caps Lock est un **état**, réglé par IOKit (`IOHIDSetModifierLockState`),
pas une frappe. Pas de manette : tranché le 02/09 (extension DriverKit signée,
entitlement Apple), `probeVirtualGamepad` répond `supported = false`.

### 20.4 La sonde : trois pièges, tous rencontrés le premier jour

- **La session graphique.** Un binaire lancé par SSH est dans une autre session
  d'audit : `CGSessionCopyCurrentDictionary` répond quand même « sur la console »
  (il parle de l'*utilisateur*), puis `CGGetActiveDisplayList` ne trouve aucun écran
  et la sonde aurait dit « Mac sans écran ». `hasInteractiveSession()` pose donc une
  seconde question, celle du *processus* : `SessionGetInfo` et son bit
  `sessionHasGraphicAccess`. Sous SSH la réponse est maintenant « pas de session
  interactive » ; les tests se lancent par `launchctl bootstrap gui/<uid>`, qui est
  la session de l'agent `com.moonlightweb.agent`.
- **L'écran endormi.** Un panneau en veille sort de la liste *active* : le Mac laissé
  dix minutes disparaissait de la liste des hôtes (« no active display », capot
  ouvert, écran noir). La sonde énumère la liste *online* (branché et utilisable) et
  note « (asleep) » dans le détail ; la session **réveille** le panneau au départ
  (`IOPMAssertionDeclareUserActivity`) et tient une assertion
  `PreventUserIdleDisplaySleep` tant qu'elle tourne — quelqu'un qui streame ce Mac
  l'utilise, quoi qu'en pense son minuteur.
- **La permission.** Screen Recording (TCC) est la seule chose que le programme ne
  peut pas s'accorder : la sonde répond `CapturePermission`, demande une fois par
  processus l'invite système (`CGRequestScreenCaptureAccess`), et dit dans quel
  panneau des Réglages Système est l'interrupteur — l'octroi ne vaut que pour les
  processus lancés *après*. Les entrées ont le même mur, Accessibility, vérifié dans
  `CgInput::start()` (`CGPreflightPostEventAccess`) avec la même phrase.

### 20.5 Ce que le banc a appris avant même la première image

- **TCC accroche l'octroi au *designated requirement* de la signature.** Une
  signature ad hoc (ce que fait l'éditeur de liens sur Apple Silicon, et ce que fait
  `codesign -s -` dans `release.yml`) a pour exigence le hachage du binaire lui-même :
  **chaque rebuild reperd l'autorisation et redemande**. Le banc signe tests et app
  avec une identité auto-signée stable (« MoonlightWeb Dev », trousseau dédié) dont
  l'exigence est `identifier … and certificate leaf = H"…"` ; un octroi, tous les
  builds. ⚠️ **Conséquence produit** : tant que le `.pkg` est signé ad hoc, chaque
  mise à jour de MoonlightWeb sur un Mac redemandera Screen Recording et
  Accessibility. Sunshine a le même problème sans Developer ID. À arbitrer avant la
  release macOS du host natif.
- **`FrameSender(Options options = {})` ne compile pas chez Apple clang** (« default
  member initializer needed within definition of enclosing class ») alors que MSVC
  et GCC l'acceptent : `Options` porte des initialiseurs de membre et le défaut est
  analysé avant qu'ils soient complets. Le seul appelant passe ses options ; le
  défaut est retiré. C'était une casse latente du job macOS de la CI depuis C4.
- **Le Mac de test n'a ni Homebrew utilisable ni sudo** : CMake et Ninja depuis leurs
  archives officielles sous `~/tools`, OpenSSL statique compilé sur place, Qt 6.10.3
  par `aqt` sous `~/Qt`, et l'app livrée sous `~/Applications` avec le LaunchAgent
  repointé, `/Applications/MoonlightWeb.app` appartenant à root.
- `.clang-format` n'avait pas de section Objective-C : les `.mm` n'étaient jamais
  formatés (« configuration does not support Objective-C »). Ajoutée, même style.

### 20.6 Le premier flux (05/09/2026)

Chrome 152 sur bench-desk → rendez-vous (`stream.moonlightweb.top/<id>`, ICE en LAN
sur le port media du banc) → l'app complète construite sur le Mac (Qt 6.10.3, OpenSSL
statique, signée avec l'identité de banc). Session `Built-in Retina Display
2560x1440@60 HEVC via VideoToolbox on Apple M1 Pro`, VBV 666 kbit, décodeur
Chrome `hev1.1.176.L153.B0` matériel, première image décodée en NV12, écran de
verrouillage du Mac **droit et aux bonnes couleurs** dans le navigateur. Cadence
lue : « 120 Hz display, 60 fps stream — 83 presents in 9.1 s, 1 not carried » ;
SCK : 84 images, 459 idle, 1 remplacée avant d'être prise ; première keyframe
123 Ko ; le gouverneur de lien a coupé une fois (« delay rising », 25,6 Mbit/s) puis
remonté par pas de 5 %.

⚠️ **Ce qui a été faux d'abord** : la première session a livré dix secondes de noir
(keyframes de 1,5 Ko), puis SCK a arrêté le flux (« Failed to find any displays »,
-3815) et la session a bouclé 45 tentatives jusqu'à ce qu'un `caffeinate -u`
externe rallume le panneau. Cause : `IOPMAssertionDeclareUserActivity` **relâchée
aussitôt déclarée** ne réveille rien. L'assertion est maintenant gardée pour la
session et redéclarée à chaque tentative de redémarrage ; rejoué avec l'écran
endormi par `pmset displaysleepnow` : image dès la première seconde, écran
rallumé, 123 Ko de première keyframe.

Autres constats de ce flux : `MaxFrameDelayCount = 0` est refusé par l'encodeur
matériel (-12900, en debug, sans conséquence visible) ; la sortie SCK suit le format
demandé par le client (2560×1440 puis 2218×1440 quand le front a réaligné le
rapport d'aspect sur l'écran 3600×2338).

### 20.7 Les entrées, validées — et le piège TCC qui les bloquait (05/09/2026)

Session de 213 s depuis Chrome : **10 233 présents pour un flux à 60 fps sur un
écran 120 Hz, 9 non portés** par la garde de cadence ; **12 768 images livrées par
SCK, 0 « idle », 2 534 remplacées avant d'être prises** — la règle « dernière image,
jamais d'arriéré » à l'œuvre ; 48 événements d'entrée injectés ; première keyframe
114 Ko ; fin propre.

Souris et clavier vérifiés **à l'œil, pas seulement dans le log** : un clic a mis les
Réglages Système au premier plan puis a actionné des boutons de dialogue, et
« clavier ok » s'est écrit dans leur champ de recherche. `CgInput` est donc juste de
bout en bout : position absolue mise à l'échelle du panneau, modificateurs portés
par chaque événement, texte par `CGEventKeyboardSetUnicodeString`.

⚠️ **Le piège, et il coûtera cher à un utilisateur** : TCC n'accroche pas une
autorisation à un identifiant de bundle mais à une **exigence de code** (le
*designated requirement*). Sur ce Mac, l'entrée « Accessibilité » de MoonlightWeb
avait été créée par l'ancienne application de `/Applications`, signée ad hoc, donc
enregistrée comme `cdhash H"9d8aba5f…"`. La nouvelle build, signée par certificat
(`identifier "com.moonlightweb.server" and certificate leaf = H"d88095f4…"`), ne
correspond pas : l'interrupteur était bien coché dans les Réglages, et macOS jetait
quand même tous les événements — silencieusement, ce qui donne un flux qui s'affiche
et ne répond pas. `CGPreflightPostEventAccess()` est ce qui le détecte, et
`kTCCServicePostEvent` est la ligne à regarder dans la base.

La réparation ne peut pas se faire à la main dans la base : **SIP la rend illisible
en écriture même pour root** (« attempt to write a readonly database »). La séquence
qui marche est `tccutil reset Accessibility com.moonlightweb.server`, puis laisser
l'application redemander (elle réapparaît dans la liste avec la bonne exigence),
puis cocher. Les trois lignes portent ensuite la même exigence que le binaire qui
tourne.

Conséquence produit, à trancher avant la release macOS : tant que le `.pkg` est
signé ad hoc, **chaque mise à jour change le cdhash et reperd Screen Recording et
Accessibility**, sans le dire. Un Developer ID, ou n'importe quelle identité stable,
supprime le problème d'un coup. C'est le même constat qu'en §20.5, mais mesuré cette
fois sur ses conséquences réelles.

Enfin, un fait de banc qui n'est pas de notre code : **Sunshine ne finit pas son
démarrage quand l'écran du Mac est en veille** — il s'arrête après le test des
encodeurs et n'ouvre aucun port. La veille écran est désormais désactivée sur cette
machine (`pmset -a displaysleep 0 sleep 0`).

### 20.8 Le son : le tap de ScreenCaptureKit, mis en cadence (05/09/2026)

macOS n'a pas de périphérique de boucle à ouvrir. Le seul moyen supporté
d'enregistrer ce que le Mac joue est le **tap audio de ScreenCaptureKit** (macOS
13+), qui n'est pas une capture à part mais **une seconde sortie du même flux** :
`capturesAudio = YES`, `sampleRate = 48000`, `channelCount = 2` sur la
configuration, et un `SCStreamOutputTypeAudio` ajouté sur une file qui lui est
propre (une image garée ne doit jamais retarder un paquet). C'est pourquoi
`setAudioSink()` vit sur la capture d'écran : un flux, une autorisation — celle
de « Screen & System Audio Recording », pas de micro —, et le son qui s'arrête et
repart avec l'image au lieu de dériver quand l'écran s'en va.

**La cadence, elle, n'a pas de plateforme.** Le relais avance l'horloge RTP d'une
trame par paquet : un paquet manquant n'est pas un paquet en retard, c'est une
horloge fausse. Windows tient ce contrat dans le fil WASAPI lui-même — le
périphérique signale, le même thread encode. Les API *push* (SCK ici, PipeWire
plus tard) appellent quand elles veulent, sur leur file : le tic doit vivre
ailleurs. C'est `PacedOpusSink` (`src/audio/`), neutre de plateforme : `push()`
depuis la capture, un thread à nous qui sort une trame Opus toutes les 5 ms,
silence compris.

Deux pièges, tous deux mesurés sur le banc :

- **`CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer` refuse une
  structure de taille fixe.** Une `AudioBufferList` dimensionnée pour huit
  canaux — largement de quoi tenir le stéréo — reçoit
  `kCMSampleBufferError_ArrayTooSmall` (-12737) : la taille que CoreMedia veut
  couvre plus que les tampons eux-mêmes. Il faut la **forme en deux appels** (le
  premier demande la taille). Le symptôme, sinon, est propre et trompeur : la
  chaîne entière tourne, 200 paquets/s exactement, 0 perdu, **3 octets par
  paquet** — du silence numérique parfaitement cadencé.
- **Core Audio livre du planaire** : 2 tampons d'1 canal, 960 échantillons (20 ms)
  à la fois. D'où `AudioInterleave.h`, où sont écrites les trois décisions qui
  comptent (mono dupliqué dans les deux oreilles, pas panoramiqué à gauche ; au-delà
  du stéréo on garde les deux frontaux ; un plan absent est du silence, pas une
  lecture par un pointeur nul) — arithmétique pure, donc testée partout.
- **La file du pacer était calibrée pour WASAPI.** Quatre trames = 20 ms, soit
  exactement une rafale SCK : chaque rafale remplissait la file à ras bord et la
  moindre gigue la débordait. Mesuré : **1 170 images capturées jetées ET 1 214
  trames envoyées en silence dans la même minute**, la file pleine et vide tour à
  tour, dix pour cent du flux dans chaque sens. Huit trames (40 ms) → **121
  jetées et 165 silences sur 16 536 paquets**, soit un dixième de ce qu'elle
  perdait. Le plafond ne borne que le pire cas : en régime établi la file se vide
  à chaque rafale, elle n'ajoute pas de latence.

Vérifié de bout en bout depuis Chrome sous Windows, un son bouclé sur le Mac :
**16 536 paquets en 82,6 s — 200,0/s exactement**, 3 958 080 échantillons captés
(61,8 s à 48 kHz sur la session précédente, sans un trou), et le signal **mesuré à
la sortie du décodeur du navigateur** : crête 0,235, RMS moyen 0,011 sur 413
blocs. Le son du Mac est audible dans le navigateur.

Reste à améliorer : le pour-cent de trames encore jeté ou envoyé en silence — de
la gigue d'ordonnancement, pas une erreur de débit (les deux horloges tiennent
48 kHz).

### 20.9 Ce qui restait — fermé le 06/09/2026

Le HDR (§20.10), le pointeur agrandi (§20.11), le résidu audio (§20.12), la
signature stable et les tests avec Screen Recording (§20.13). La plateforme est
au niveau des deux autres, à une exception près qui n'est pas du code : le
`.pkg` n'est pas notarié (Developer ID payant), donc un double-clic sur le
téléchargement affiche « développeur non identifié » ; le chemin Homebrew n'en
souffre pas.

Hors de ce module, vu au passage et **corrigé depuis** : Internet Access se
désactivait entièrement quand l'enregistrement PowerDNS échouait, rendez-vous
compris, alors que le rendez-vous n'a aucun besoin du sous-domaine. La
rétro-compatibilité DNS côté client a été retirée le 05/09/2026 — plus aucune
installation n'écrit dans PowerDNS ni ne lance ACME, et ce chemin de coupure
n'existe plus. **Vérifié en vrai le 06/09** : l'app complète à jour déployée
sur le Mac de banc (`.env` réduit à `MW_DOMAIN` + `MW_PDNS_TOKEN`, les seules
clés que le client lit encore), le consentement v2 redonné par
`MoonlightWeb --enable-internet --yes`, et la ligne de rendez-vous levée sans
qu'aucun enregistrement DNS ne soit écrit.

### 20.10 HDR : le 10 bits PQ de ScreenCaptureKit → HEVC Main10 (06/09/2026)

macOS n'a pas d'interrupteur HDR. Un panneau capable d'aller au-dessus du blanc
SDR l'est toujours — c'est l'*Extended Dynamic Range*, et
`NSScreen.maximumPotentialExtendedDynamicRangeColorComponentValue` dit de
combien (le Liquid Retina XDR du banc répond 5 ; un panneau SDR répond 1). La
sonde traduit donc `hdrActive` par « headroom > 1 **et** macOS 15 », parce que
c'est la capture qui gate la plateforme : `SCStreamConfiguration.captureDynamicRange`
n'existe que depuis macOS 15, et sans elle le compositeur ne livre que du 8 bits.
`supports10Bit` suit la même règle (HEVC matériel **et** capture 10 bits), jamais
« la puce le pourrait » (§16.3).

La chaîne reste sans étage de conversion : on demande au compositeur du
`x420` (4:2:0 10 bits dans des mots de 16, la disposition P010) avec
`SCCaptureDynamicRangeHDRCanonicalDisplay` — la référence fixe à 1 000 nits,
pas le headroom local, pour que le flux ne suive pas l'état de luminosité du
panneau — et l'espace `ITUR_2100_PQ`. ⚠️ CGDisplayStream n'a jamais eu de
constante de matrice BT.2020 ; la propriété prend les mêmes chaînes que
CoreVideo attache aux tampons, et `kCVImageBufferYCbCrMatrix_ITU_R_2020` est
acceptée. La première image est **relue et journalisée** (format, primaires,
transfert, matrice) : `x420 2218x1440, primaries ITU_R_2020, transfer
SMPTE_ST_2084_PQ, matrix ITU_R_2020` — c'est la ligne qui prouve que la
description couleur de l'encodeur dit la vérité.

VideoToolbox : profil **Main10 nommé** (un profil Main accepte la surface 10
bits et encode 8 bits dedans, §16.2) et les trois propriétés couleur posées sur
la session — `ColorPrimaries ITU_R_2020`, `TransferFunction SMPTE_ST_2084_PQ`,
`YCbCrMatrix ITU_R_2020` — pour qu'elles atterrissent dans le VUI. En SDR rien
n'est posé : VideoToolbox lit alors les attaches du tampon, et une description
qui différerait d'elles déclencherait une conversion couleur silencieuse.

**Vérifié depuis Chrome/Windows sur le M27Q en HDR** : session « HEVC HDR
(Main10, BT.2020 PQ) », première keyframe 98 Ko, overlay client « HEVC HDR »
(présentateur `<video>`, le chemin F0e), image aux bonnes couleurs — les
séquoias de l'économiseur du Mac, pas délavés. Le pointeur agrandi (§20.11)
passe par le même chemin 10 bits (courbe PQ, matrice BT.2020 sur le signal PQ,
blanc SDR à 203 nits).

### 20.11 Le pointeur agrandi : dessiné par le moteur (06/09/2026)

Le §20.1 disait « pas de route » : ScreenCaptureKit compose le pointeur à sa
taille, et il n'y a pas de convertisseur où le grossir. La route est celle-ci :
quand le client demande une taille (`cursorFramePx`, le téléphone), la capture
est mise en `showsCursor = NO` et le moteur **dessine lui-même** le curseur
système — l'image `NSCursor` déjà rastérisée pour le mode client-dessiné — dans
le tampon du compositeur, juste avant l'encodage. `convert/CursorBlend.h`, pur
et testé partout : la forme est préparée **une fois** par changement dans les
valeurs de code de la cible (luma/chroma × couverture, BT.709 8 bits ou BT.2020
PQ 10 bits), puis chaque image coûte un échantillonnage bilinéaire à l'échelle
voulue et un multiplier-ajouter par plan sur l'empreinte du pointeur — quelques
centaines de pixels dans une image 4K, verrouillage `CVPixelBufferLockBaseAddress`
compris.

Trois détails qui ont une raison :

- **Le tampon est le compositeur's, et il est ré-encodé.** Le plancher écran
  fixe et la rafale de raffinement ré-encodent la dernière image ; un pointeur
  brûlé dedans laisserait une traînée. Le blend sauve d'abord le rectangle
  qu'il couvre (`PlanePatch`) et le **restaure avant le blend suivant**, tant que
  le tampon est le même (numéro de série de l'image tenue).
- **Un mouvement de pointeur redevient une image.** Pointeur hors capture, un
  déplacement sur un écran fixe ne produit plus d'image ; la boucle regarde
  alors le pointeur (position à chaque tour, forme toutes les 50 ms) et
  ré-encode l'image tenue quand il a bougé, au plus à la cadence du flux —
  l'équivalent du statut PointerOnly de DXGI, obtenu autrement.
- **L'échelle** : taille naturelle dans l'image = raster × (image / pixels de
  l'écran) ; cible = taille demandée / côté long de l'encre ; plafond ×2,5 comme
  le convertisseur Windows.

**Vérifié en vrai** : « drawing the pointer at x2.50 of the frame (96 px asked,
shape 34x46, ink 34x46) », le pointeur suit la souris injectée depuis le client,
en SDR comme en HDR. ⚠️ Fausse alerte du banc : il est sorti **vert** — la
couleur de remplissage personnalisée du pointeur de ce Mac (Accessibilité ›
Pointeur, `cursorFill` dans `com.apple.universalaccess`), fidèlement reproduite,
ce qu'une sonde de relecture (raster → contributions → plans, en debug) a
tranché en une ligne : `BGRA 0 255 0 255` à la source.

### 20.12 Le résidu audio : la grâce du pacer (06/09/2026)

Le pour-cent du §20.8 avait une cause, pas une gigue. ScreenCaptureKit livre
20 ms de son à la fois, à son heure ; avec `pop()` seul, une rafale arrivée 3 ms
après le tic qui en avait besoin envoyait une trame de **silence** — et ce
silence n'était pas gratuit : l'horloge avançait sans consommer la file, qui
gardait dès lors une trame **de plus**, pour toujours, jusqu'à ce que le plafond
en jette une. Chaque rafale tardive ajoutait une trame, chaque trame jetée était
cet ajout qui ressortait — d'où deux compteurs jumeaux (165 silences, 121
jetées sur 83 s) sur un hôte dont les deux horloges étaient exactes.

`AudioPacer::take()` : une trame due que la file ne peut pas remplir est
**différée** jusqu'à deux périodes (10 ms) avant que le silence parte ; le
`PacedOpusSink` dort alors jusqu'à la fin de la grâce **ou** jusqu'au `push()`
suivant, qui le réveille. Le récepteur tient 35 ms de tampon de gigue : il ne
voit rien. `pop()` est conservé tel quel pour WASAPI (un seul fil, pas de rafale
à attendre). Test : 20 ms de rafales avec 9 ms de gigue pendant 2 s → zéro
silence, zéro jetée.

**Mesuré sur 325 s de flux réel (tonalité jouée sur le Mac)** : 65 138 paquets,
**7 jetées, 57 silences** (0,01 % et 0,09 %, dont 4 et 50 dans la première
minute — la rafale de démarrage de SCK), 2 344 attentes. Les minutes suivantes :
1 / 1, puis 2 / 2. Le dixième de pour-cent restant est la rafale initiale et
quelques rafales à plus de 10 ms de retard ; il n'y a plus d'accumulation.

### 20.13 Signature stable, tests avec Screen Recording, banc à jour (06/09/2026)

**La signature.** Le §20.5 et le §20.7 disaient le problème : TCC accroche ses
octrois à l'*exigence de code*, et une signature ad hoc en change à chaque
build, donc chaque mise à jour reperdait Screen Recording et Accessibility (la
seconde en silence). `release.yml` signe désormais l'app avec un certificat
**auto-signé stable** (« MoonlightWeb », valable jusqu'en 2041 ; identité
générée le 06/09/2026, conservée hors dépôt dans `.secrets/` chez le mainteneur,
publiée dans les secrets d'environnement `MACOS_SIGN_P12` (base64) et
`MACOS_SIGN_PASSWORD`). Exigence désignée résultante :
`identifier "com.moonlightweb.server" and certificate root = H"d051d7d8…"` —
« root » parce qu'un auto-signé est sa propre racine ; constante d'une release
à l'autre. Gatekeeper n'y voit aucune différence avec l'ad hoc (« développeur
non identifié » au double-clic, rien via Homebrew) ; le `.pkg` lui-même reste
non signé, une signature d'installeur n'ayant de sens qu'avec un Developer ID
Installer. Une installation qui vient d'une build ad hoc redemandera les deux
autorisations **une dernière fois**. Répété sur le banc avant d'être écrit dans
le workflow, avec deux pièges : `security import` refuse un `.p12` moderne
(PBES2/AES-256, ce qu'OpenSSL 3 produit par défaut — « MAC verification
failed ») et veut du SHA-1/3DES ; et `codesign` répond `errSecInternalComponent`
tant que le trousseau temporaire n'est pas dans la **liste de recherche**, même
nommé par `--keychain`. Secret absent → ad hoc comme avant, avec un avertissement.

**Les tests.** `mw-native-tests` n'avait pas Screen Recording et l'octroi à la
main ne survivait pas au rebuild (§20.5). TCC identifie un exécutable nu par
son **chemin**, un bundle par son identifiant + son exigence : enveloppé dans un
`.app` minimal qui porte l'identifiant de l'app (`com.moonlightweb.server`) et
signé de la même identité, le binaire de tests satisfait **l'octroi existant de
l'app** — même ligne dans `TCC.db`, aucune nouvelle. `scripts/mac-native-tests.sh`
fait l'enveloppe, la signature et le lancement par `launchctl` dans la session
graphique (la seule où SCK voit un écran) ; mesuré : les tests de session
capturent au premier essai, 2 254/2 254.

**Le banc.** Le clone du Mac est passé au HEAD de la machine Windows par
`git bundle` (les commits ne sont pas poussés), l'app complète rebâtie et
déployée : c'est la première fois que le Mac tourne le code du jour et non un
`native-host` superposé à un serveur vieux d'une semaine.

### 20.14 Couper le son côté hôte : le tap n'est pas sur le chemin (08/09/2026)

Le réglage `mute_host_audio` — coché par défaut chez le client, envoyé depuis
toujours — n'était lu que par Windows (§24). Sur un Mac, la case ne faisait
**rien, en silence** : le spectateur entendait le jeu, et la pièce aussi.

**La mesure d'abord, parce que la réponse de Windows ne se transporte pas.** Là-bas
le loopback WASAPI prélève la sortie du *moteur*, donc le volume principal
atteint la capture et seul un mute fait par le pilote lui échappe. Le tap de
ScreenCaptureKit est ailleurs : c'est une seconde sortie du même flux (§20.8),
alimentée par les applications. Une sonde de banc (une tonalité 440 Hz jouée en
continu, RMS du tap sur 2,5 s par état) tranche :

| État | RMS du tap |
|---|---|
| départ | 0,2997 |
| point de sortie **muet** | 0,3027 |
| volume 0,5 | 0,3028 |
| volume **0** | 0,3029 |
| restauré | 0,3030 |

Identique au bruit près : **ni le mute ni le volume du périphérique de sortie
n'est sur le chemin de la capture**. Deux conséquences, et elles simplifient le
code par rapport à Windows :

- le **volume** est utilisable ici, alors qu'il ne l'a jamais été là-bas ;
- il n'y a **pas besoin** de la stratégie « router vers un périphérique qui ne
  pilote aucun haut-parleur » : rien, dans le périphérique de sortie, ne peut
  retirer le son du flux, donc rendre muet celui que l'utilisateur écoute suffit.

⚠️ La première passe de la sonde avait écrit 0 sur un volume qui **lisait déjà
0,000** : elle ne prouvait rien du volume, et le tableau ci-dessus est la
seconde, qui monte à 0,5 avant de redescendre.

**Ce qui est livré.** `audio/macos/HostMute.{h,cpp}` — même forme et même
contrat que la classe Windows du même nom, deux stratégies : `EndpointMute`
(`kAudioDevicePropertyMute` sur la sortie par défaut, élément maître ou, à
défaut, sa paire stéréo) et, pour une sortie qui n'a pas de mute (certains HDMI,
AirPlay), `VolumeZero`. Déjà muet ou déjà à zéro = revendiqué **sans rien
sauvegarder**, pour que `release()` ne relève pas un mute qu'il n'a pas posé. Au
relâchement, un réglage que l'utilisateur a changé entre-temps est laissé tel
quel. `MacSession` engage avant la capture et relâche après — sur macOS l'ordre
n'a aucune importance, il n'est là que pour que les deux plateformes se lisent
côte à côte.

⚠️ Piège attrapé en câblant : `MacSession::start()` remet `m_Info` à zéro
**après** le bloc audio (l'ordre inverse de Windows), donc le drapeau
`hostMuted` posé à l'engagement était effacé sans bruit. Il est relu de
`m_HostMute.strategy()` là où `m_Info.audio` est rempli.

**Vérifié.** `mw-native-tests` 3179/3179 sur le Mac, 3385/3385 sur Windows (le
test est commun aux deux plateformes depuis ce chapitre). Le test *s'arrange*
sa précondition : sur une machine déjà muette, il lève le mute pour que la
branche qui écrit soit celle qui est exercée, et repose ce qu'il a trouvé.
Et en vrai, sur l'app déployée, l'état lu **de l'extérieur** (`osascript`,
une fois par seconde) : `muted=false` avant, `true` de la première à la douzième
seconde de session, `false` à l'instant de l'arrêt et ensuite — pendant que
l'audio continuait de partir (2 961 paquets, 0 jeté, 29 trames de silence de
démarrage), une vidéo YouTube jouant sur le Mac.

**Confirmé à l'oreille par Bruno, devant la machine** (« le son est bien coupé,
ça fonctionne ») : c'est la seule moitié de ce chapitre qu'aucune sonde ne peut
produire. Un `kAudioDevicePropertyMute` à 1 dit ce que l'OS a enregistré, pas ce
que la pièce entend.

**Reste** : Linux (rien — `HostMute` n'est inclus que par les sessions Windows
et macOS), et un worker tué de force laisse le mute posé, comme sous Windows.

### 20.15 Les deux autorisations sont demandées ensemble, à l'installation (19/09/2026)

Demande de Bruno : « est-ce qu'il est possible de forcer la demande de toutes les
autorisations lors de l'installation et pas uniquement quand on doit lancer un
stream ? » — oui, et rien ne l'empêchait : c'est **où** on appelait l'API qui
décidait du moment.

Screen Recording était déjà demandée au démarrage (`MacProbe::enumerate`, §20.5),
donc au premier lancement que le `postinstall` provoque. Accessibility, elle,
n'était demandée que par `CgInput::start` — c'est-à-dire **à l'ouverture d'une
session**, la seule des deux que l'utilisateur ne voit pas venir : une boîte de
dialogue au milieu d'un stream, sur une machine dont il s'est souvent déjà
éloigné, et contre une image qui s'affiche parfaitement pendant que rien ne
répond. `CGPreflightPostEventAccess()` + `CGRequestPostEventAccess()` sont donc
appelés dans la sonde, à côté de la capture, une fois par processus.

⚠️ **Accessibility n'est pas un `Unavailability`**, et ce choix est le cœur du
correctif. Un Mac qu'on peut regarder sans le piloter **est** un hôte : le
déclarer indisponible le retirerait de la liste, donc retirerait aussi l'endroit
où dire ce qui manque. C'est un champ à part, `Capabilities::inputPermission`
(vrai partout ailleurs : Windows et Linux ne demandent rien à personne), porté
par `NativeCapabilitiesJson` — une sonde plus ancienne qui n'écrit pas la clé est
lue comme « rien à signaler », jamais comme un refus — et rendu par
`/api/setup/status` (`native.needs_input_permission`) et `/api/native/status`
(`input_permission`, publié **même quand le moteur est indisponible** : un Mac
neuf à qui il manque les deux ne doit pas répondre « Screen Recording » seule).

L'assistant de première configuration dit les deux, séparément, avec un bouton
par panneau (`/api/system/open-screen-recording` existait, `open-accessibility`
est son jumeau, même garde localhost) : le prompt du système n'est montré
**qu'une fois**, donc celui qui l'a écarté n'a plus que ce chemin-là. Les deux
lignes coexistent sur la dernière page — l'installation fraîche, c'est
exactement les deux à la fois.

Ce que ça ne répare pas, et qu'il faut dire : macOS n'applique Screen Recording
qu'au **prochain** démarrage du programme (§20.5) ; demander plus tôt ne change
rien à ça, ça change seulement le moment où on pose la question. Reste ouvert :
prévenir aussi **pendant** un stream si l'octroi a disparu entre-temps (une
mise à jour signée autrement, §20.7) — aujourd'hui seul le log le dit.

## 21. Intel Quick Sync : la première exécution, et ce qu'elle a cassé (07/09/2026)

Le banc `bench-intel` (Intel N95, UHD Graphics 24 EU, pilote 32.0.101.7088,
Windows 11) est le premier GPU Intel de la flotte. Le chemin oneVPL y a été
exécuté pour la première fois. Le §14 du plan v2 le disait « écrit, jamais
exécuté » ; il n'a rien fonctionné du premier coup, et chacun des cinq défauts
était invisible sans matériel.

Ordre des symptômes, tel que la machine les a donnés — c'est aussi l'ordre dans
lequel un autre vendeur les redonnera :

### 21.1 « no usable encoder » sur une machine qui a Quick Sync

La sonde répondait `available:false`, « no video encoder this engine can drive
on any GPU », sur les **trois** adaptateurs Intel que DXGI énumère (un vrai, et
un par pilote d'écran indirect — Parsec VDD et Virtual Display Driver).

`MFXVideoCORE_SetHandle(MFX_HANDLE_D3D11_DEVICE)` répondait
`MFX_ERR_UNDEFINED_BEHAVIOR` (-16), documenté « the same handle is redefined …
or an internal handle has been created before this function call ».

Deux causes empilées, et il fallait les deux :

1. **Le device n'avait pas `D3D11_CREATE_DEVICE_VIDEO_SUPPORT`.** Sans ce
   drapeau un device D3D11 n'expose pas d'`ID3D11VideoDevice`, et le runtime
   Intel ne peut rien en faire. Aucun autre encodeur de cet arbre ne l'exigeait,
   donc personne ne l'avait posé — ni la sonde (`VplCapabilities`) ni la capture
   (`DxgiDuplication`, qui est le device que l'encodeur reçoit en session
   réelle). Les deux le posent maintenant ; c'est gratuit sur un GPU qui s'en
   moque.
2. **Le device doit être donné au dispatcher, pas à la session.** Sur oneVPL 2.x
   le dispatcher crée le device lui-même en créant la session, et un `SetHandle`
   ultérieur arrive trop tard. Les deux propriétés qui le lui donnent à temps
   — `mfxHDL` et `mfxHandleType` — appartiennent au dispatcher et non à
   `mfxImplDescription`, donc elles n'apparaissent nulle part dans les en-têtes.

`VplSession::open` essaie les deux routes dans cet ordre et **demande ensuite au
runtime quel device il utilise** (`GetHandle`), puis compare le LUID de son
adaptateur à celui demandé. C'est ce qui départage les trois « Intel(R) UHD
Graphics » de la liste, et c'est ce qui empêcherait une session de tourner en
silence sur un device dont nos textures ne sont pas.

⚠️ **`MFXVideoCORE_GetHandle` n'incrémente pas le compteur COM**, contrairement à
ce que sa documentation promet. Relâcher la référence qu'on n'a jamais reçue
libère le device sous son propriétaire : mesuré comme une violation d'accès dans
`d3d11!CDevice::Release` à la seconde où la sonde lâchait son propre `ComPtr`.
Le handle est emprunté, jamais possédé.

### 21.2 La première frame tuait le processus

Corruption de pile (`0xC0000409`), sans log, dès le premier `EncodeFrameAsync`.

En `MFX_IOPATTERN_IN_VIDEO_MEMORY` une surface ne porte pas de pixels : elle
porte un `Data.MemId`, que le runtime traduit en texture **par l'allocateur de la
session**. Sans allocateur enregistré, le runtime utilise le sien et lui présente
notre MemId, qu'il relit comme une de ses propres structures. Ce n'est pas un
chemin d'erreur, c'est un pointeur sauvage.

`VplFrameAllocator` (nouveau) pose le contrat : **dans ce moteur, un MemId est
toujours un `mfxHDLPair` {ID3D11Texture2D\*, sous-ressource}**, et `GetHDL` est
la ligne qui le dit. L'allocateur sert aussi les surfaces que l'encodeur alloue
pour lui-même (images reconstruites), en `D3D11_BIND_DECODER` comme le fait
l'allocateur des samples Media SDK, avec repli en RT+SRV.

### 21.3 Le débit ne bougeait jamais

`MFXVideoENCODE_Reset` répondait `MFX_ERR_INCOMPATIBLE_VIDEO_PARAM` (-14) à
**chaque** changement du gouverneur de lien — soit environ deux fois par seconde
sur un lien qui souffre. L'encodeur Intel ignorait donc complètement le lien,
et la seule trace était un avertissement qu'un log de session fait défiler.

Trois causes, toutes réelles :

1. **Le modèle HRD.** Avec `NalHrdConformance` actif, oneVPL traite un changement
   de débit comme une nouvelle séquence et refuse tout ce qui n'est pas une IDR.
   Il est désormais explicitement à `OFF` (`mfxExtCodingOption`, chaîné à chaque
   session, intra-refresh ou pas). L'alternative — forcer la nouvelle séquence —
   achèterait la conformité au prix d'une keyframe deux fois par seconde, c'est-
   à-dire exactement le pic de débit qu'un lien congestionné ne peut pas encaisser.
2. **Le VBV.** Rebâtir `BufferSizeInKB` au nouveau débit est une réallocation, et
   `Reset` refuse l'appel entier pour ça. `applyBitrateOnly()` ne touche donc que
   `TargetKbps`/`MaxKbps` ; le VBV reste où l'init l'a mis.
3. **Le bloc de paramètres.** `Reset` compare au bloc réellement en vigueur, y
   compris les champs que le runtime a remplis lui-même à l'init (profil, niveau,
   nombre de références). `init()` relit maintenant ce bloc par
   `EncodeGetVideoParam` et c'est lui que `Reset` reçoit.

### 21.4 Trop lent de moitié

Premier chiffre mesuré : **16,3 ms** par frame en HEVC 1080p60, TU7. Un stream à
60 fps ne tient pas dans ça.

`mfx.LowPower = MFX_CODINGOPTION_ON` — le moteur à fonction fixe (VDENC) plutôt
que celui à shaders — le ramène à **10,5 ms**, et le 1440p de 21,4 à 13,4 ms.
C'est le même arbitrage que partout ailleurs dans ce moteur : moins de passes,
moins de latence, quelques bits de plus. Une génération sans VDENC pour ce codec
le dit à `Query`, et `init()` refait la demande sans — journalisé, jamais avalé.

### 21.5 La session mourait au bout de douze frames

`SyncOperation` répond `MFX_WRN_IN_EXECUTION` (1) quand son délai passe avec la
frame encore dans l'encodeur — « redemande », exactement comme
`MFX_WRN_DEVICE_BUSY`. Le code en faisait une erreur fatale : premier flux
navigateur réel, douze frames, puis `session ended: waiting for the encoded
frame failed: still executing (1)`. Sur un N95 qui encode, décode et fait tourner
le navigateur sur les mêmes quatre cœurs, une frame sur quelques centaines
dépasse 100 ms.

Le délai reste court — un encodeur vraiment mort doit être vu vite — mais il est
redemandé jusqu'à dix fois, et la première lenteur est dite une fois par session.

### 21.6 Faire monter le budget par image — trois routes, une seule marche

`Reset` refuse aussi toute cible **au-dessus** de celle de l'init (« requires
additional memory allocation »), et refuse l'appel entier. Or le budget par
cadence réelle (E4) demande légitimement plus que le débit réglé quand l'image
bouge moins vite que le flux : sur ce banc il demandait 32000 kbps pour un stream
réglé à 20000, deux fois par seconde. Ce n'est **pas** une demande d'envoyer plus
par seconde — le débit sur le fil ne bouge pas — c'est « cette seconde ne contient
que 30 images, chacune peut être deux fois plus grosse ».

Trois routes essayées sur l'N95, dans cet ordre :

| Route | Résultat |
|---|---|
| Déclarer un `MaxKbps` plus haut à l'init pour laisser de la place | ⛔ en CBR le runtime le rabat aussitôt sur `TargetKbps` — « ignored », comme la doc l'autorise. Vérifié par relecture : `budget up to 20000` alors qu'on avait demandé 120000 |
| Laisser le débit tranquille et dire à `Reset` que la cadence a baissé (arithmétiquement le même budget) | ⛔ refusé aussi (-14). `Reset` refuse **tout ce qui déplace le budget par image**, quel que soit le champ où c'est écrit |
| Dimensionner le tampon de bitstream à l'init pour la hausse | ✅ **c'est celle-là**. 20000 → 40000 refusé avec 85 Ko de tampon, accepté avec 250 Ko |

Le tampon est aussi le VBV (§9.x, RateControl.h), donc la marge n'est pas « tout
ce que quelqu'un pourrait demander » — ce serait six fois le débit, six temps
d'image pour une seule image — mais **exactement la plage où travaille
`EffectiveCadence`** : son plancher est 30 fps, donc au plus deux fois pour un
flux à 60. `budgetCeilingKbps()` le calcule, `budgetBufferKbps()` dit de combien
le tampon doit dépasser ce plafond pour que `Reset` l'accepte (trois fois,
mesuré), et `setBitrate` plafonne au lieu de se faire refuser.

⚠️ **Et cette marge a été RETIRÉE le jour même, après mesure.** Elle fait ce
qu'elle promet — 40,6 → 55,4 Ko par image, +37 % de bits pour le même débit sur
le fil — mais le tampon est aussi le VBV : le pic par image passe de 60-64 Ko à
104-155 Ko, soit **26 → 42 ms d'occupation du lien** pour une seule image à
20 Mbit/s (banc §8f). Règle de Bruno : « qualité légèrement moindre sur écran
fixe acceptable ; aucune augmentation volontaire de la latence pour gagner en
netteté ». Donc `kBudgetHeadroom = 1` : le VBV revient à la règle partagée, le
budget par image ne monte pas, et `setBitrate` **plafonne** au lieu de se faire
refuser — ce qui reste strictement meilleur que le point de départ, où un
`Reset` refusé laissait le débit là où il était.

L'arithmétique de la marge est conservée entière, parce que c'est un arbitrage et
non un fait : `kBudgetHeadroom` porte la mesure et ce qu'un changement coûte.
### 21.6b Invalidation de référence — Intel est le plus simple des trois

`NumRefFrame` valait **1**, ce qui rendait la réparation par delta impossible par
construction : sans image plus ancienne à laquelle se raccrocher, une perte ne
pouvait être répondue que par une keyframe. Il vaut maintenant 4, comme le DPB
NVENC et pour la même raison.

Le mécanisme est `mfxExtAVCRefListCtrl`, attaché **par image** au
`mfxEncodeCtrl` : `LongTermRefList` marque l'image courante comme référence long
terme, `RejectedRefList` refuse celle que la perte a gâtée, `PreferredRefList`
nomme celle sur laquelle prédire, et `NumRefIdxL0Active = 1` fait de cette
préférence une obligation.

Et c'est là qu'Intel est plus commode que les deux autres : **oneVPL nomme les
images par `FrameOrder`**, c'est-à-dire par le numéro que le récepteur connaît
déjà. Pas de traduction index de slot ↔ numéro d'image — la source de deux
allers-retours sur matériel AMD (§9.10.1). `ReferenceSlots`, écrit pour AMF, se
réutilise tel quel pour l'arithmétique de portée ; seule la façon de nommer
change. Le support est **demandé au runtime** (query mode 1 avec le buffer
attaché, comme son en-tête le prescrit) et non supposé : un runtime qui n'en veut
pas laisse la session exactement comme avant, keyframes comprises, et `/start`
répond `ref_invalidation:false`.

✅ **Vérifié en vrai le 07/09** : `mw_drop_test=120` depuis bench-desk sur un flux
HEVC 1080p de l'hôte Intel — cinq pertes nommées, cinq réparations
(« oneVPL healed frame 205 with a delta against frame 204 »), **zéro IDR
demandée, zéro erreur de décodeur**, flux vivant à 16,8 ms. Coût mesuré à
l'encodage : nul (10,99 ms contre 11,22 sans).

⚠️ **Jamais en HEVC sous intra-refresh (29/09/2026).** Une réparation qui tombe
pendant une vague bloque l'encodeur HEVC d'Intel pour de bon : l'image ne
sort jamais du runtime, sans TDR, et le stream meurt (banc §8n.30). H.264, AV1
et le HEVC sans vague font les mêmes réparations sans broncher. Un tel stream
ne marque donc aucune référence longue (`longTermRepairsSafe`, `VplSession.h`),
et `/start` répond `ref_invalidation:false`. Le client ne nomme plus ses
pertes : il demande une image clé, comme face à tout encodeur sans
invalidation. C'est l'option A de Bruno, le 29/09 : une image clé au lieu d'un
delta, dans ce seul cas. Le journal le dit à l'ouverture : « no reference
invalidation (a repair during an intra-refresh sweep hangs HEVC: keyframes
instead) ».

### 21.7 Ce qui est prouvé, et ce qui ne l'est pas

Prouvé sur le banc, le 07/09/2026 :

- sonde : `available:true`, **HEVC et H.264** en matériel, pas d'AV1 (Alder
  Lake-N décode l'AV1 mais ne l'encode pas) ;
- banc `--native-bench`, bureau fixe, 20 Mbit/s, intra-refresh, 8 s par passe :

  | Codec | TU | Taille | fps | encode moy / p95 / p99 (ms) |
  |---|---|---|---|---|
  | HEVC | 1 | 1920×1080 | 59,6 | 13,33 / 18,43 / 24,58 |
  | HEVC | 4 | 1920×1080 | 59,8 | 12,59 / 18,43 / 20,48 |
  | HEVC | 7 | 1920×1080 | 59,7 | **11,46** / 15,36 / 18,43 |
  | H.264 | 1 | 1920×1080 | 58,1 | 15,53 / 20,48 / 26,62 |
  | H.264 | 4 | 1920×1080 | 59,8 | 12,61 / 16,38 / 22,53 |
  | H.264 | 7 | 1920×1080 | 59,7 | 13,38 / 18,43 / 18,43 |
  | HEVC | 7 | 2560×1440 | 59,6 | 13,43 / 18,43 / 20,48 |
  | HEVC | 1 | 2560×1440 | 58,3 | 16,47 / 20,48 / 26,62 |

  **Le TargetUsage ne se voit pas** : 1,9 ms d'écart maximum, du même ordre que
  la dispersion entre passes, et le défaut du moteur (TU7) est déjà le bord
  rapide. Même verdict que sur AMD — rien à appliquer. ⚠️ le pilote Intel ne
  rapporte **pas** de QP moyen, donc ce banc n'a aucune mesure objective de
  qualité, exactement comme AMF (§8c du banc) ;
- **premier flux navigateur depuis un hôte Intel** : Chrome 152 sur la machine
  elle-même, HEVC `hvc1.1.144.L123.B0`, `descLen=111` (VPS/SPS/PPS extraits de
  la keyframe), première image décodée 1920×1080 NV12 en matériel, 65,7 s de
  session, **1689 présents tous portés**, audio 13 142 paquets / 0 jeté, aucune
  erreur de décodeur, arrêt propre ;
- **puis depuis une autre machine, en LAN** (Chrome sur bench-desk → hôte Intel,
  appairage par PIN, `webrtc-dc-udp`) : **latence affichée 11,4 à 14,2 ms**,
  deux sessions de 289 s et 320 s, 2327 puis 2558 présents **tous portés**,
  4831 frames émises, encode 7,05 / 11,26 / 14,34 ms et total hôte
  8,84 / 13,31 / 18,43 ms (moy/p95/p99), 57 800 paquets audio / 0 jeté,
  **63 événements d'entrée injectés** (souris et clavier depuis le navigateur
  distant), arrêt propre. ⚠️ c'est la mesure qui compte : les chiffres en
  loopback (41 à 55 ms) étaient ceux d'un N95 qui encodait, décodait et servait
  la page en même temps.

  C'est aussi la seule condition où le gouverneur de lien a pu **remonter** :
  16000 → 20000 kbps en cinq paliers, toutes les hausses appliquées. En loopback
  la machine était saturée et il ne faisait que descendre — la moitié montante
  du correctif de `Reset` n'y était pas prouvée.

Pas prouvé, et à ne pas supposer :

- ⚠️ **HDR : plus vrai depuis le §21.10** — le P010 Main10 BT.2020 PQ est
  livré et vérifié le jour même. Le **4:4:4** reste refusé, pour une raison
  qu'aucun matériel ne change : la conversion produit de l'AYUV, qu'oneVPL ne
  prend pas en entrée d'encodeur ;
- **les chiffres de latence** viennent d'un N95 à 4 cœurs qui encodait, décodait
  et servait la page en même temps. Ils disent que le chemin tient 60 fps en
  1080p et en 1440p ; ils ne disent rien d'un Intel de bureau ou d'un Arc.

### 21.8 Deux bugs trouvés en montant la mesure clic→photon

Aucun des deux n'est propre à Intel ; les deux étaient invisibles jusqu'ici.

**La vue Réglages plantait dans TOUT build debug.** `SettingsView.render()`
construisait `veAlgoHtml` — le sélecteur d'algorithme d'Enhancer, qui n'existe
qu'en debug — en lisant `veCheckboxDisabled`, un `const` déclaré dix lignes plus
bas. Lire un `const` avant sa déclaration est une exception (zone morte
temporelle), donc la vue entière mourait sur « Cannot access
'veCheckboxDisabled' before initialization ». En Release `veAlgoHtml` vaut `''`
et l'expression n'est jamais évaluée : le bug ne pouvait se voir que là où on
allait justement chercher la sonde de latence. Les trois déclarations sont
remontées avant leur usage.

**Une image lente tuait la session.** `SyncOperation` répond
`MFX_WRN_IN_EXECUTION` quand son délai passe sans que l'image soit prête ; le
§21.5 avait porté le plafond de 100 ms à une seconde. Avec le clip 1440p60 qui
joue sur l'hôte **et** le navigateur qui décode sur les mêmes quatre cœurs, des
images ont dépassé la seconde, puis trois. Le plafond est maintenant de dix
secondes : un flux à une image par seconde est inutilisable, mais l'arrêter est
pire que le laisser se dégrader — la cadence, le gouverneur de lien et le
réglage de résolution du spectateur sont là pour ça.

### 21.9 Le clic→photon n'a pas été obtenu sur ce banc

Demandé, monté, non acquis — et il ne faut pas en inventer un chiffre.

Le drapeau clic→photon (`LatencyFlag`) est gaté sur `Q_OS_WIN && QT_DEBUG`. Un
vrai build Debug est inutilisable comme banc ici : notre propre passe de
conversion passe de 0,4 à 10,6 ms et l'acquisition de 0,24 à 7 ms. Un arbre
Release ne portant que `-DQT_DEBUG` a donc été bâti pour la mesure (jamais
livré). L'hôte journalise bien chaque clic reçu — « [LatencyFlag] injected click
at 1711,1056 » — mais la sonde du navigateur ne voit **jamais** les trois bandes
dans l'image décodée : `timeout` sur toutes les tentatives, avec le clip **comme**
sur un bureau fixe où le pipeline tourne à 60 fps et 11 ms d'encodage.

Ce qui a été éliminé : la page cliente n'est pas gelée (les premières tentatives
l'étaient — Chrome dé-priorise une fenêtre occultée et `requestAnimationFrame`
s'arrête ; relancé avec `--disable-features=CalculateNativeWinOcclusion`) ; le
clic part et arrive ; la session vit.

Ce qui n'est pas tranché : le drapeau est-il dessiné ? Une vérification par
capture d'écran sur le banc n'a rien vu, **mais elle ne prouve rien** — `BitBlt`
(ce qu'utilise `CopyFromScreen`) ne capture pas une fenêtre *layered*, alors que
Desktop Duplication, elle, la capturerait. La prochaine étape utile est donc de
regarder l'écran du banc autrement (Desktop Duplication, ou l'œil), pas de
recommencer la même mesure.

⚠️ **Complément du même jour, et il déplace la question.** La sonde a été
instrumentée : elle lit bien de vrais pixels, au bon endroit
(Canvas2DRenderer._readProbePixels échantillonne y = 2,5 % de la hauteur et
x = 46,5 / 50,5 / 54,5 % — exactement le rectangle du drapeau), 244 lectures sur
les 1,5 s qui suivent un clic. Elles rendent toutes **R = G = B autour de 140**,
alors que le haut de l'écran de l'hôte est à ce moment-là une page blanche. Ce
que la sonde échantillonne ne correspond donc pas au haut de l'image de l'hôte,
quel que soit le drapeau. C'est une piste **client**, indépendante d'Intel.

#### ⚠️ Élucidé le 07/09 au soir — et ce n'était ni Intel ni le client

Le drapeau n'était créé que sur l'écran **principal**. La session, elle, streame
l'écran que le spectateur a choisi : sur tout autre écran le drapeau n'était pas
dans l'image du tout. Le banc Intel a **deux adaptateurs d'écran virtuels** en
plus du sien, et un stream peut atterrir sur le mauvais — le gris uniforme lu par
la sonde était le haut d'un autre
écran que celui qu'on regardait, et la page blanche était sur le principal.

Prouvé sur bench-desk avec un harnais qui exerce le vrai `LatencyFlag` et relit
chaque sortie en Desktop Duplication : avant, l'écran secondaire rendait
`rgb(1,64,108) rgb(2,66,112) rgb(1,67,115)` — trois valeurs voisines et banales,
la même signature que le « 140 gris » du banc ; après, les deux écrans rendent
bleu/blanc/rouge. Détail dans `docs/design/glass-to-glass.md` §5 bis.

Le même harnais répond à la question laissée ouverte deux paragraphes plus haut :
**le drapeau est bien peint, et la Desktop Duplication le capture**. `BitBlt` ne
voyait rien parce qu'il ne voit pas une fenêtre *layered*, pas parce qu'il n'y
avait rien à voir.

Côté sonde, un échantillon écarté porte désormais `saw` et `via` : les pixels
lus et la surface qui les a rendus. Les trois causes possibles d'un `timeout`
(drapeau absent, mauvaise image, surface non dessinée) se lisaient toutes
« timeout » — c'est ce qui a coûté deux fausses pistes.

**Reste** : le relevé clic→photon sur le banc Intel lui-même, à refaire avec ce
correctif.

### 21.10 HDR sur Intel : FP16 scRGB → P010 → HEVC Main10 (07/09/2026)

Le §21 disait « HDR refusé par construction ». Corrigé le même jour : la chaîne
existait déjà des deux côtés — la passe de conversion sait produire du P010
BT.2020 PQ depuis la capture FP16 (§16), et le runtime Intel dit oui au 10 bits.
Il ne manquait que le chemin entre les deux.

**Trois choses, et pas une de plus** :

- `FrameInfo` en `MFX_FOURCC_P010`, `BitDepthLuma/Chroma = 10`, `Shift = 1` (le
  P010 range ses dix bits dans le HAUT de chaque échantillon 16 bits) ;
- `CodecProfile = MFX_PROFILE_HEVC_MAIN10`, nommé plutôt que laissé au runtime ;
- `mfxExtVideoSignalInfo` chaîné : `ColourPrimaries = 9` (BT.2020),
  `TransferCharacteristics = 16` (PQ), `MatrixCoefficients = 9`,
  `VideoFullRange = 0`. ⚠️ **Ces trois entiers ne sont pas un détail** : un flux
  10 bits dont la VUI dit encore BT.709 sRGB n'est refusé par personne, il est
  *affiché délavé* — ce qui se lit comme un bug de shader. Mêmes valeurs, même
  raisonnement, que les chemins NVENC et AMF.

La sonde de capacités demande maintenant le 10 bits **au runtime**, et seulement
pour HEVC : c'est le seul codec pour lequel ce chemin a un profil Main10, et
aucune puce Intel du banc n'encode l'AV1 (§21.7). `supports10Bit` répond `true`
sur l'UHD Graphics de l'N95.

**Vérifié en vrai, HDR Windows activé sur le M27Q du banc (HDMI)** :

- sonde : `[HDR]` sur le display, `hdrActive:true`, `supports10Bit:true` ;
- banc : `duplication started: 2560x1440 (HDR, FP16)` →
  `colour conversion: FP16 scRGB -> 1920x1080 P010 4:2:0 (BT.2020 PQ, limited)` →
  `oneVPL ready: HEVC HDR (Main10, BT.2020 PQ)`, keyframe 41 Ko, 131 images ;
- flux réel depuis bench-desk : le navigateur configure
  **`hvc1.2.144.L123.B0`** — profil 2 = Main10 — `descLen=115`, `hdr=true`,
  première image décodée 1920×1080. Le client sait donc que c'est du PQ BT.2020
  parce que le flux le lui dit.

⚠️ **Coût mesuré** : l'encodage 1080p60 passe de ~11 à **18,7 ms** par image sur
cette puce. Le 10 bits n'est pas gratuit sur un iGPU d'entrée de gamme.

⚠️ **Ce qui n'est PAS prouvé** : l'image sur un écran client HDR. Le M27Q est
partagé — HDMI vers le banc Intel, DisplayPort vers bench-desk — et n'affiche
qu'une entrée à la fois, donc le client était sur écran SDR : Chrome a annoncé
`hdrMode=browser` et a ramené le PQ à la main, ce qui donne l'image plate et
délavée attendue dans ce cas. Structure, couleurs et géométrie sont justes ; le
rendu HDR final demande de basculer l'entrée du moniteur et d'y activer le HDR,
ce qui est la manœuvre de Bruno, pas la mienne.

## 22. L'étage de repli : quand aucun GPU n'encode (07/09/2026)

Jusqu'ici une machine dont aucun GPU n'avait d'encodeur que nous savons piloter
était refusée (`Unavailability::NoEncoder`, « there is no software fallback »).
Deux machines de Bruno étaient dans ce cas : le portable **Windows on ARM**
(Snapdragon 7c, Adreno 618 — aucun SDK constructeur) et la **VM Debian sous
Hyper-V** (`hyperv_drm`). Demande : un repli automatique, avec la latence la plus
basse possible, ces machines étant du bas de gamme.

### 22.1 Un étage à côté des GPU, jamais devant

`Capabilities::fallbacks` est une liste de `FallbackEncoder` (API, codecs,
matériel ou non, nom), **consultée uniquement quand aucun GPU n'encode**
(`Capabilities::anyGpuEncodes()`, partagé entre `probe()` et `select()`). Elle
vit à côté de `GpuInfo::encoders` et non dedans, et c'est le point de
conception : la règle du Selector est « le GPU de l'écran, sauf s'il ne peut pas
encoder » — une entrée logicielle sur la liste d'un iGPU ferait choisir le CPU
alors qu'un NVENC dort dans la même machine. Tenu à part, le repli est
inatteignable tant qu'un GPU répond, et chaque sélection existante est identique
à l'octet près (six tests le verrouillent).

Garanties : matériel avant CPU quel que soit l'ordre de la sonde ; le GPU de
l'écran est **conservé** (capture et conversion y tournent, seul l'encodeur a
bougé — aucune copie inter-GPU introduite) ; ni HDR ni 4:4:4 ; H.264 en tête de
ce qui est offert (un hôte trop faible pour encoder en matériel ne doit pas
pousser le client vers un décodeur logiciel) ; et **jamais de suragrandissement**
— un client réglé en 1440p devant un écran 1080p aurait fait encoder 1,8× les
pixels pour aucune information (mesuré : 21 ms par image sur le Snapdragon).

Arbitrage de Bruno : « les deux, MF d'abord » — sur Windows, Media Foundation
matériel → Media Foundation logiciel → OpenH264 ; sur Linux, OpenH264. Chaque
descente est dite dans le log. Clés de banc `fallback=1|mf|mfsw|mfcpu|cpu`
(`EncoderTuning::Fallback`) : la machine est mise dans l'état exact où l'étage
sert — chaque GPU dépouillé de ses encodeurs — plutôt qu'un cas spécial.

### 22.2 Media Foundation : le silicium dont on n'a pas le SDK

`MfEncoder` est deux choses derrière une interface. Énuméré avec
`MFT_ENUM_FLAG_HARDWARE` c'est le transform d'un constructeur sur son silicium,
et les textures D3D11 du convertisseur y entrent telles quelles (DXGI device
manager) ; sans le drapeau c'est le transform logiciel de Microsoft, qui prend de
la mémoire système — chaque image traverse alors une texture de staging. La
classe les distingue en interrogeant le transform (`MF_SA_D3D11_AWARE`), jamais
son nom. `mfplat.dll` est chargé à l'exécution et jamais lié : absent des éditions
N de Windows, un import statique empêcherait MoonlightWeb de **démarrer**. Seuls
`mfuuid` et `strmiids` (tables de GUID sans DLL) sont liés.

Latence dans le vocabulaire MF : `AVLowLatencyMode`, aucune B-frame, CBR, VBV
d'une image (la règle de `RateControl.h`), GOP sans keyframe périodique, keyframe
à la demande ; chaque refus d'un transform est dit, pas fatal.

Trois pièges mesurés :

- **l'énumération matérielle est machine-entière** : sur l'écran NVIDIA de
  bench-desk elle rendait `AMDh264Encoder`, qui refusait ensuite tout type de sortie
  (son device n'est pas le nôtre). `MFTEnum2` + `MFT_ENUM_ADAPTER_LUID` demande
  le transform de l'adaptateur des images ; NVIDIA n'ayant pas de MFT, la session
  retombe proprement sur le transform logiciel ;
- **le transform AMD (H.264 et HEVC) accepte une image puis se tait** — un seul
  `METransformNeedInput`, jamais de sortie, texture ou mémoire système, 1 s
  d'attente. Ce n'est pas le pompage d'événements. Il est donc **mis à l'épreuve à
  l'init** sur une image noire ; muet, il coûte à la machine l'étage suivant au
  lieu d'une session morte. AMD n'est pas la cible (AMF le sert) ; le garde-fou
  protège le cas Qualcomm, qui est un asynchrone du même genre ;
- **le transform Qualcomm annonce 18 `NeedInput` avant sa première sortie** (la
  profondeur de sa file) ; l'attente de la première image (1 s) l'absorbe.

**Prouvé sur le banc ARM** : `QCOM Hardware Encoder - H264` / `- HEVC` trouvés,
matériels, asynchrones, D3D-aware. Banc 720p60 8 Mbit/s : 131 images / 8 s,
12,5 ms de moyenne, p99 16,8 ms. **Flux navigateur réel** depuis bench-desk par le
rendez-vous : le client préfère HEVC, le transform HEVC répond, 2 650 images
entrées / 2 650 sorties, encode 20,9 ms de moyenne à 1440p (suragrandi — d'où la
règle du §22.1), 29,5 ms de latence affichée, bureau du Snapdragon à l'écran.
Une machine où Sunshine encode en x264 logiciel streame en **matériel**.

### 22.3 OpenH264 : le dernier étage, sur le CPU

Sous-module `cisco/openh264` épinglé v2.6.0 (BSD-2, déjà sur la liste blanche),
et un CMake écrit par nous — upstream n'a que Makefile et meson —, encodeur seul
(`third_party/openh264.cmake`, listes de `codec/*/targets.mk`). Noyaux NASM sur
x86-64 (nasm cherché sur le PATH, dans `MW_NASM` et dans le cache d'outils de
vcpkg), `.S` NEON sur AArch64 avec gcc/clang, **C pur sous MSVC ARM64** — la
raison de l'arbitrage « MF d'abord » : sur le Snapdragon on n'aurait eu que le
C, et l'assembleur GAS d'upstream ne passe pas `armasm64`. Le configure dit fort
quand il compile sans noyaux.

`OpenH264Encoder` est neutre : il prend une image **I420** en mémoire système
(pas NV12, seul encodeur ici) et ne sait rien des textures ; chaque plateforme
possède la copie qui l'y amène. Latence : threads **par tranches** (une image sur
les cœurs, jamais un pipeline d'images), **aucun saut d'image** — OpenH264
avertit que sans saut « le débit ne peut pas être contrôlé » : il veut dire qu'une
image trop grosse dépasse au lieu de disparaître, ce que le gouverneur de lien
absorbe, alors qu'une image disparue se lit comme un gel —, CAVLC,
`LOW_COMPLEXITY`, denoise/scène/arrière-plan/AQ éteints, CBR, GOP sans keyframe
périodique, VUI BT.709 limité.

Deux manies mesurées : le plafond doit être **strictement** supérieur à la cible
(+1 %, au moins un kilobit) ; et `SPATIAL_LAYER_ALL` n'écrit que le chiffre
global alors que le contrôle lit la couche 0 — quatre appels pour un nombre, et
l'ordre dépend du sens (plafond d'abord à la hausse, cible d'abord à la baisse).
Sans cela la rafale de raffinement et le gouverneur étaient refusés.

`SoftwareEncoder` (Windows) : NV12 texture → staging → Map → trois plans I420 en
une passe, le chroma entrelacé séparé pendant la lecture. Mesuré sur bench-desk :
3,3 ms/image synthétique 1080p sur 4 threads, 10,7 ms/image bureau réel
relecture comprise ; flux navigateur réel 2560×1440 en `avc1.42c033`, 16,3 ms.

### 22.4 Linux sans render node : KMS → DMA-BUF mmap → CPU

La VM Debian n'a que `card0` : pas de VA-API, mais **pas d'EGL non plus** — ni
conversion ni encodage GPU. ⚠️ Le plan disait « capture X11/XShm » ; c'était
faux. Mesuré avec une sonde C (`kmsdump`) : le scanout de `hyperv_drm` est
**XR24 linéaire (modifier 0)**, l'export PRIME passe et le `mmap` du dma-buf rend
les vrais pixels (8 Mo en 4,2 ms à froid). Sur le bench-mini le même mmap est refusé
(amdgpu, tuilé) — la voie CPU est bien celle des machines sans GPU, et seulement
d'elles. Donc `KmsCapture` reste tel quel — aucun serveur d'affichage requis, la
même propriété « capture avant le login » que la voie GPU — et ce qui change est
qui lit le buffer : `CpuConvert` (mmap du premier plan, `DMA_BUF_IOCTL_SYNC`, une
passe BGRA→I420 BT.709 en bandes de lignes sur 4 threads, `BgraToI420.h` testé
sous Windows aussi), puis `OpenH264Encoder`. Dans `LinuxSession` la conversion et
l'encodage deviennent un objet, `VideoPipeline`, parce que les deux paires
inversent la propriété de l'image (la surface de l'encodeur pour VA-API, les
plans du convertisseur pour OpenH264).

**Le premier flux a reconstruit la chaîne 5 440 fois en 50 s.** `hyperv_drm` n'a
pas de vblank (`drmWaitVBlank` → `EOPNOTSUPP`, lu comme « le CRTC s'en va » →
`Lost`) et n'a **qu'un framebuffer**, dans lequel le compositeur dessine sur
place : son id ne change jamais. Aucun des deux signaux qu'`acquire()` lit
n'existe. **Mode scruté** : un refus du vblank au premier `acquire` bascule la
capture en scrutation à la cadence de l'écran, et c'est le **contenu** qui
témoigne — le buffer tenu est mappé pour la durée de la tenue et replié en un
nombre (XOR × premier, chaque mot compte) ; empreinte nouvelle = image nouvelle,
même empreinte = `Timeout`. ~1 ms par scrutation en 1080p. Le contrat de la boucle
tient sans qu'elle bouge.

**Prouvé sur la VM** (`.deb` 0.3.0.i10, lanceur avec la capacité) : hôte natif
levé là où l'ancien build disait « operating system predates… » ; flux navigateur
depuis bench-desk par le rendez-vous : 1920×1080 `avc1.42c02a` décodé en matériel,
**8,1 ms** de latence affichée, bureau XFCE à l'écran ; un clic dans le flux
déplace le pointeur (dans l'image sur ce pilote), le dock apparaît, l'horloge
avance — l'empreinte détecte le mouvement. Étages hôte sur 291 images : convert
0,41 / 4,10 / 6,66 ms, encode 5,36 / 15,4 / 18,4 ms (moy./p95/p99).

### 22.5 Ce qui est prouvé, et ce qui ne l'est pas

Prouvé : les trois machines nommées streament en natif (Snapdragon en matériel,
VM et bench-desk-sans-GPU en CPU) ; aucune machine qui encodait déjà n'a changé
d'un octet ; la descente MF matériel → MF logiciel → OpenH264 et ses raisons dans
le log.

Non prouvé, ou non fait : le pointeur n'est pas composé dans l'image sur la voie
CPU (le client le dessine — défaut bureau ; en mode jeu il manque) ; aucun
plafond automatique quand le CPU ne suit pas (E4 réduit le budget par image et le
gouverneur le débit, mais rien ne baisse la résolution — à mesurer sur l'N95) ;
`/api/native/status` n'affiche pas l'encodeur de repli (codecs vides sur le GPU) ;
une édition N de Windows sans `mfplat.dll` n'a pas été essayée ; le transform AMD
muet n'est pas élucidé (sans conséquence : AMF le sert).

---

## 23. Glisser une fenêtre d'un écran streamé à l'autre (07/09/2026)

Deux displays de l'hôte natif streamés en parallèle, un panneau navigateur
chacun : déplacer la souris de l'un à l'autre donne déjà la sensation de deux
écrans. La demande était la suite naturelle — prendre la barre de titre d'une
fenêtre sur le display 1, bouton gauche tenu, la déposer sur le display 2, comme
on le fait sans y penser sur deux écrans physiques.

Le geste voulu, dans le détail qui compte : hors des deux panneaux **rien ne
bouge**, les images restent figées ; au survol du second panneau la fenêtre
reprend sa course ; et un bouton relâché entre les deux panneaux doit être connu
avant même que le pointeur ne revienne sur une image.

### 23.1 Trois des cinq étapes ne demandaient aucun code

- **Hors image, rien ne part.** `_absoluteMouseMessage()` (`StreamView.js`)
  renvoie `null` dès que le point sort du rectangle de l'image. Aucune position
  n'est jamais envoyée depuis un endroit que le spectateur ne regarde pas.
- **Le bouton tenu survit à la traversée.** Un bouton est un état du *bureau*,
  pas d'une session : `SendInput` a posé un `LEFTDOWN` global et rien ne le
  relâche. Le chien de garde (`InputWatchdog`, `kStaleMs = 250`) ne lâche que sur
  **silence** du client, et le panneau d'origine bat toutes les 100 ms tant qu'un
  bouton est tenu (`_sendInputState`).
- **Les deux sessions ne se marchent pas dessus.** Chacune a son `Win32Input`,
  son `m_HeldButtons` et son watchdog ; un panneau qui pose une position
  n'enregistre aucun bouton, et son `releaseAll()` de fin de session ne touche
  pas celui du voisin. La dé-duplication de `Win32Input::sendMouseButton` nommait
  déjà le cas « deux sessions qui se recouvrent ».

### 23.2 La mesure, et le mur

La seule inconnue était le navigateur, pas nous : une page autonome qui compte
les événements, ouverte dans deux fenêtres Chrome côte à côte, y répond
fidèlement et sans build. Elle a répondu trois choses d'un coup :

1. La fenêtre de départ reçoit bien son `mouseup` — 25 s après l'appui, alors que
   le bouton a été relâché sur le bureau, entre les deux fenêtres. **L'étape 5
   fonctionne**, et un bouton ne peut pas rester collé.
2. Survoler l'autre fenêtre ne fait **pas** perdre le focus à celle qui tient le
   drag. On croyait devoir empêcher `_onWindowBlur` de relâcher les boutons
   souris en plein vol : faux problème, rien à corriger.
3. La fenêtre survolée ne reçoit **rien du tout**. Zéro `mousemove`, zéro
   `mouseover`, pendant toute la durée du drag.

Le troisième point est le mur. Chrome pose une capture souris au niveau de l'OS
sur la fenêtre où l'appui a eu lieu, et **aucune API web ne permet de la rendre**.
Le geste est donc inatteignable entre deux fenêtres navigateur indépendantes —
non par un choix de conception de notre côté.

Deux routes ont été écartées explicitement, et il vaut la peine de dire pourquoi
plutôt que de les redécouvrir : **prolonger la position hors image** ferait
apparaître le curseur sur le second écran avant que le pointeur du spectateur n'y
soit arrivé, ce qui se voit ; **faire dialoguer les onglets** (origines d'écran
publiées dans les capacités, `BroadcastChannel` entre panneaux) est de la
machinerie fragile pour ce qu'elle rend.

Reste **une seule forme exacte** : les deux panneaux dans un *même document*. La
capture reste alors sur le document, le panneau qui tient le drag reçoit les
positions même au-dessus de son voisin, et il lui délègue la seule position —
chaque panneau calculant contre son propre `_mediaRect()`. Aucune extrapolation,
aucun canal entre onglets, aucune origine d'écran à publier, et au-dessus de rien
il n'y a pas de panneau donc rien n'est envoyé. Décision produit **non prise** :
cette forme impose les deux flux dans une seule fenêtre navigateur.

### 23.3 ⚠️ Ce que la mesure a trouvé au passage : l'absolu Linux ignorait l'origine

`UinputInput` mettait la position à l'échelle du device absolu — l'espace fixe
`0..32767` de la convention tablette — **sans jamais ajouter l'origine du
display**. L'en-tête l'assumait : « an absolute position is expressed in the
display's own space and needs no offset ».

C'est faux, et le raisonnement l'était de la même façon qu'un curseur de tablette
l'est : un périphérique noyau ne rapporte qu'une **fraction de son propre axe**,
et le compositeur l'étale sur le **bureau entier**, exactement comme une tablette
couvre tout le sous-main. Viser le display capturé revenait donc à viser le
bureau : correct sur un hôte à un seul écran — d'où l'invisibilité — et faux
partout ailleurs. Sur un hôte Linux à deux écrans, streamer le second plaçait
déjà le pointeur n'importe où, sans qu'aucun glissement soit en jeu.

Le correctif est celui que Windows applique depuis toujours, en deux temps
(`X11Pointer.h`, deux fonctions libres à côté de `clampIntoRect` — arithmétique
pure, donc testée sur les trois plateformes) :

1. `displayPointToDesktop()` — la surface de référence du client mise à l'échelle
   du display, **origine comprise**, et clampée dedans pour qu'un client dont le
   ratio diffère d'un pixel ne marche pas sur l'écran voisin.
2. `desktopToAbsoluteRange()` — ce point bureau exprimé en fraction du bureau,
   bornes sur bornes, la convention même de `Win32Input::desktopToAbsolute`
   contre `SM_CXVIRTUALSCREEN`.

Le bureau vient de l'union des sorties **actives** de la carte
(`LinuxSession::readInputRects()`, poussée par `IInputSink::setDesktopRect()` au
démarrage et à chaque redémarrage de capture). Lue **hors du verrou d'entrée** :
énumérer les connecteurs ouvre le périphérique DRM, et `inject()` attend sur ce
même verrou. Bureau inconnu = hôte à un écran, où le display *est* le bureau et
le calcul se réduit à ce qu'il était.

La plage du device reste `0..32767` : elle n'a pas à changer, ce qui évite de
recréer le périphérique quand la disposition des écrans bouge. Windows et macOS
n'avaient rien à corriger — `toAbsolute()` ajoute déjà l'origine, et `CgInput`
interpole entre les bornes du display en coordonnées globales.

Limite assumée : seules les sorties de la carte capturée sont comptées. Un bureau
étalé sur deux GPU compterait un bureau trop petit — et se tromperait alors du
même pointeur mal placé qu'on vient de corriger, pas de pire.

## 24. Couper le son de l'hôte sans couper la capture (07/09/2026)

Depuis D4 (§13) l'hôte natif capture sa sortie par défaut en loopback WASAPI —
et continue de l'entendre. Sunshine coupe les haut-parleurs quand le client le
demande (`localAudioPlayMode=0`), et MoonlightWeb a ce réglage depuis toujours
(`mute_host_audio`, coché par défaut) : il partait vers les hôtes GameStream et
n'était **pas lu** par le natif. Le chapitre consistait à savoir *comment* le
lire, parce que la réponse évidente est fausse.

### 24.1 La mesure qui tranche

Le loopback WASAPI prélève la **sortie du moteur audio**, avant l'endpoint. Tout
ce que le moteur fait à cette sortie atteint donc la capture ; tout ce que le
pilote fait après, non. Sonde écrite pour le mesurer (une tonalité 440 Hz jouée
par un autre processus, RMS du loopback sur 2 s par état), sur la sortie par
défaut de bench-desk — le HDMI du M27Q, pilote AMD :

| État de l'endpoint | RMS loopback | Ce que ça dit |
|---|---|---|
| Rien | 0,1726 | référence |
| `IAudioEndpointVolume::SetMute(TRUE)` | **0,1726** | le mute est fait **par le pilote**, après le prélèvement : haut-parleurs muets, capture intacte |
| `SetMasterVolumeLevelScalar(0)` | 0,0175 | le volume est appliqué **par le moteur** : la capture s'éteint avec les haut-parleurs |
| `Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE)` | — | `AUDCLNT_E_DEVICE_IN_USE` : impossible tant qu'un flux partagé (le jeu) existe |

Ce qui distingue les deux premières lignes est déclaré par le pilote :
`QueryHardwareSupport()` répond `ENDPOINT_HARDWARE_SUPPORT_MUTE` (0x2) sans le
bit volume sur cet endpoint. Un endpoint qui coupe en logiciel aurait une
première ligne à zéro — et l'on n'a **aucun** moyen de couper ses haut-parleurs
sans couper la capture. Le mode exclusif, parfois proposé pour « prendre » la
sortie, est écarté par construction : il refuse dès qu'une application partagée
joue, c'est-à-dire exactement pendant un stream.

### 24.2 Trois stratégies, dans l'ordre (`audio/windows/HostMute.h`)

1. **Mute matériel** — la sortie par défaut annonce le mute matériel : `SetMute`
   pour la session, remis à la fin. Aucune dépendance, un seul effet visible :
   l'icône du haut-parleur.
2. **Sortie virtuelle** — pas de mute matériel, mais un périphérique de lecture
   qui n'a pas de haut-parleur existe (Steam Streaming Speakers, VB-Cable,
   VoiceMeeter, Virtual Audio Cable, Virtual Desktop Audio, reconnus par leur
   nom) : il devient la sortie par défaut (rôles Console et Multimédia ;
   Communications n'est pas touché, un appel en cours n'a rien à faire sur un
   périphérique que personne n'entend) par la même interface `IPolicyConfig`
   non publiée que Sunshine et tous les commutateurs de sortie utilisent, et le
   loopback s'ouvre **dessus** — d'où l'ordre : `engage()` avant `WasapiLoopback`.
   La sortie d'avant est remise à la fin.
3. **Rien** — l'hôte continue de s'entendre, et le journal dit pourquoi en une
   phrase (« *mutes in software (the capture would go quiet too) and there is no
   virtual output to route to* »).

Deux règles de restitution : ce que l'utilisateur a changé pendant la session
lui appartient (un mute levé à la main n'est pas remis ; une sortie changée à la
main reste), et la destruction de l'objet relâche aussi — une session qui meurt
par une exception ne laisse pas la pièce muette. Ce qui n'est pas couvert : un
worker **tué** (crash, `taskkill`) laisse le mute ou la sortie en place, à
remettre dans le panneau Son.

`SessionConfig::muteHostAudio` porte le réglage (`Session.cpp` le prend dans
`m_Config.muteHostAudio`, la même source que le GameStream), `SessionInfo::
hostMuted` dit ce qui a été obtenu, la ligne « streaming … » du moteur porte
`[host muted]`.

### 24.3 Vérifié

- `test_host_mute.cpp` : la stratégie prévue a toujours une phrase ; aller-retour
  `engage()`/`release()` avec l'état de l'endpoint lu de l'extérieur avant, pendant
  (muet) et après (identique à l'avant) ; idempotence ; destructeur. 2671 checks.
- **Flux réel** Display 1 (AMF HEVC, RX 7600) depuis Chrome par le rendez-vous :
  « speakers muted on "4 - M27Q (2- AMD High Definition Audio Device)" (hardware
  mute — the capture keeps hearing the mix) », endpoint lu `muted=1` pendant le
  stream, **loopback RMS 0,274 avec la tonalité** — la capture entend ce que la
  pièce n'entend plus ; à l'arrêt, `muted=0`.

### 24.4 Ce qui reste

- ~~**macOS et Linux** : rien.~~ ✅ **macOS livré le 08/09 (§20.14)** — et la
  supposition écrite ici était fausse dans les deux sens : il n'a fallu ni
  changer de périphérique de sortie par défaut, ni s'inquiéter du volume. Le tap
  de ScreenCaptureKit n'étant sur le chemin ni du mute ni du volume (mesuré),
  rendre muette la sortie que l'utilisateur écoute suffit. ✅ **Linux livré le
  08/09 (§19.17)** — et là aussi la phrase écrite ici (« jamais un mute au
  volume, le moniteur entend ce que le sink joue ») était fausse : c'est vrai de
  PulseAudio, pas de PipeWire, dont les ports moniteur sont pris **en amont** du
  volume sauf sur les sinks virtuels. Le mute est donc la stratégie 1 et le sink
  nul le repli, à l'envers de ce qui était prévu. Les trois plateformes ont
  maintenant un `HostMute`, et **aucune des trois n'a la même réponse** — la
  seule règle qui se transporte est de mesurer avant d'écrire.
- La stratégie 2 n'a été vérifiée que par la sonde (Steam Streaming Speakers
  existe sur bench-desk mais le HDMI passe en stratégie 1) : `SetDefaultEndpoint`
  et la remise sont écrits, pas exercés en flux réel.
- Un endpoint qui **dit** matériel et refuse `SetMute` retombe sur la stratégie
  2 puis 3 — chemin écrit, jamais vu.

## 25. Sunshine sort aussi de l'installation macOS/Linux (08/09/2026)

Constat de Bruno sur une capture d'écran de l'assistant : « je pensais que
Sunshine ne faisait plus partie du processus d'installation ». Il avait raison,
et §15.5 avait cessé d'être vraie. Le 04/09, retirer Sunshine du seul installeur
Windows était le geste **complet** : macOS et Linux répondaient « no backend for
this platform in this build », et le leur retirer les aurait privées de tout
hôte. Le 05 et le 06, ces deux plates-formes ont eu leur moteur (§19, §20). La
raison est tombée, la page est restée.

### 25.1 Le verdict n'est pas « disponible », c'est « possible »

Trois endroits demandaient Sunshine : l'assistant in-app (`SetupView`), le pane
« Sunshine » du `.pkg` macOS, et le postinstall macOS qui téléchargeait un DMG et
écrivait `sunshine --creds`. Aucun ne pouvait simplement être supprimé : une
machine qui ne peut pas se diffuser doit continuer d'être aidée — **l'AppImage**
en premier, qui ne porte aucune capacité et n'a donc aucune capture (§19.8).

`/api/setup/status` gagne donc un objet `native`, et le champ sur lequel
l'assistant branche n'est **pas** `available` :

```json
"native": { "available": false, "reason": "…", "needs_permission": true, "possible": true }
```

`possible` = `available`, ou l'une des deux raisons qui veulent dire « le moteur
est là, il attend quelque chose que l'utilisateur peut donner » :
`CapturePermission` et `NoInteractiveSession`. C'est le piège que la première
version aurait eu : **macOS répond `available: false` jusqu'à ce que
l'enregistrement d'écran soit coché** (`CGPreflightScreenCaptureAccess`, §20.4),
et c'est exactement l'état d'un premier lancement. Un assistant qui branche sur
`available` enverrait chaque nouveau Mac installer un second serveur de streaming
au moment précis où il est à une case à cocher de se diffuser lui-même. Toutes
les autres raisons — pas d'API de capture, aucun encodeur, OS trop vieux, build
sans backend — veulent dire que cette machine a besoin d'un hôte à côté d'elle,
et Sunshine y est offert exactement comme avant.

### 25.2 Ce qui change pour l'utilisateur, écran par écran

| Où | Avant | Après |
|---|---|---|
| Assistant, section « Sunshine » | identifiants + case « installer automatiquement » | section « Diffuser cet ordinateur » : une ligne verte « cet ordinateur peut diffuser son propre écran » ; sur un Mac sans permission, la phrase qui dit quoi cocher et qu'il faut **relancer** l'app |
| Assistant, réapparition | revenait tant que Sunshine n'était pas installé | ne revient que si la machine n'a **aucun** hôte : `native.possible` compte autant que `sunshine.installed` |
| Assistant, écran final (macOS) | « ouvrez Sunshine et accordez-lui… » | la même phrase pour **MoonlightWeb**, et seulement quand la permission manque vraiment |
| `.pkg` macOS, pane latéral | « Sunshine » : identifiants, téléchargement du DMG, bouton Skip, sonde Basic-Auth | « Internet » : la seule question, et le texte de consentement récupère toute la bande que les identifiants occupaient |
| `.pkg` macOS, postinstall | montait un DMG, copiait `Sunshine.app`, lançait `--creds`, écrivait le mot de passe en clair dans `provisioning.json` et dans `/tmp` | plus rien de tout ça — **aucun mot de passe en clair n'est écrit sur ce disque** |
| `install.sh`, `.deb`/`.rpm` | « Sunshine n'a pas été installé : cet hôte n'a pas d'écran » | « cet hôte ne peut pas se diffuser lui-même » — la même chose, sans nommer un logiciel qui n'était pas en cause |

Ce qui **ne** bouge **pas** : `SunshineInstaller` en entier, `/api/setup/
sunshine-check`, et tout le chemin Sunshine/Apollo/Wolf. Sunshine reste un hôte
de plein droit partout, découvert et appairé depuis la page des hôtes ; l'AppImage
et les machines sans encodeur voient l'assistant d'avant, mot pour mot.

### 25.3 Vérifié, et ce qui attend la CI

- `/api/setup/status` sur bench-desk : `{"available": true, "needs_permission":
  false, "possible": true, "reason": "available"}`. Backend TNR 1082, sécurité
  405, build vert.
- Front : 5 tests neufs (`SetupNativeHost.test.js`) — la machine qui se diffuse
  ne montre aucun champ Sunshine ; un Sunshine installé **à côté** du moteur
  n'est plus ni installé ni appairé par l'assistant ; le cas permission macOS
  montre la phrase et pas l'offre Sunshine ; une machine sans moteur garde
  l'assistant d'avant ; un `status` **sans** objet `native` est traité comme
  « ne peut pas se diffuser » (un serveur plus ancien aide au lieu de se taire).
  584 tests front au total.
### 25.4 Vu à l'écran, sur le banc Mac (08/09/2026)

Les deux réserves du §25.3 sont levées, l'une entièrement, l'autre à moitié.

**L'assistant.** App au commit de l'assistant en deux pages, bâtie et déployée sur
le M1 (identité « MoonlightWeb Dev », donc les octrois TCC tiennent), puis parcourue
par Bruno : « l'assistant fonctionne bien ». Deux drapeaux ont dû être remis pour
qu'il y ait quelque chose à voir — `setup_completed`, évidemment, mais aussi
`internet_access_enabled` : **une machine dont le lien est déjà actif ne voit jamais
la page 1**, `_configPage()` l'envoie droit sur la seconde. Le raccourci est voulu ;
il cache simplement la moitié du parcours à qui veut le relire.

Ce que ce passage prouve au-delà du rendu : le consentement enregistré dans
`settings.json` est **mot pour mot ce qui était à l'écran** — 967 caractères, le
corps du texte suivi de `/ Allow the Internet link (recommended)`, la phrase même
que le bouton Accepter engage. C'était jusqu'ici la propriété d'un test unitaire
sur des clés `text:setup.*` ; elle est maintenant vérifiée de bout en bout, du
navigateur au fichier, sur une vraie machine.

**Le `.pkg`, à moitié.** `MWInternetPane.m` compile propre sur le banc en
`-Wall -Wextra` (bundle Mach-O arm64), `postinstall` passe le contrôle de syntaxe,
le `.xib` référence bien la classe renommée, et les seules occurrences de
« Sunshine » qui restent sous `installer/macos` sont de la prose (« client des hôtes
Sunshine, Apollo et Wolf ») et des commentaires d'historique — plus une ligne de
logique. ~~⚠️ **L'assemblage lui-même reste non fait**~~ ✅ **fait par la CI le
08/09** (run `34273892962`, job « Package macOS arm64 », 4 min 45) : `xcrun ibtool`
demande Xcode complet, que le banc n'a pas, mais le runner l'a. Le `.pkg` produit a
été ouvert et vérifié :

- `MWInternetPane.nib` **compilé** est dans le bundle, avec le binaire
  `MoonlightWebInstaller` (Mach-O arm64, signé), `NSMainNibFile = MWInternetPane`
  et `NSPrincipalClass = InstallerSection` ;
- `InstallerSections.plist` place le volet entre `PackageSelection` et `Install`,
  donc l'utilisateur le voit avant que quoi que ce soit ne s'installe ;
- l'app à l'intérieur est signée `com.moonlightweb.server` avec **l'exigence
  désignée attendue** — `certificate root = H"d051d7d8…"`, la constante de §20.13 —
  et `codesign --verify --deep --strict` passe : les octrois TCC survivront donc
  bien aux mises à jour, ce qui n'avait jamais été vérifié sur un artefact de CI ;
- le `.pkg` lui-même est **sans signature**, comme décidé (Developer ID Installer
  seulement) ;
- le `postinstall` ne contient aucun secret : les trois occurrences de
  « sunshine / password / creds » sont les commentaires qui expliquent leur
  disparition.

⚠️ **Piège de lecture, à ne pas refaire** : `pkgutil --expand` écrit le répertoire
des plugins comme un **fichier** `PlugIns` — un blob gzip+cpio, même forme que
`Payload`. Vu de loin il ressemble à un dossier vide, et j'ai d'abord conclu que le
volet manquait. `xar -tf` puis `gunzip -dc | cpio -i` montrent le contenu réel.

**Et installé pour de vrai, sur le banc** (08/09, artefact du run `34280361028`,
`sudo installer -pkg … -target /`) : « The upgrade was successful », la charge
utile arrive dans `/Applications` appartenant à root, `codesign --verify --deep
--strict` passe sur l'app posée, et le `postinstall` fait ce qu'il annonce —
`provisioning.json` écrit, LaunchAgent réécrit vers `/Applications`, données
utilisateur rendues à l'utilisateur, **règle de pare-feu ajoutée**. L'app démarre,
sert `/api/health` en 80 et 46152, et découvre les hôtes du LAN.

⚠️ **Ce que l'installation apprend sur TCC, et qu'il faut lire correctement** :
l'app installée répond `available: false — Screen Recording is not granted`. Ce
n'est pas une régression, c'est l'arithmétique des identités : le banc signe avec
« MoonlightWeb Dev » (`certificate leaf = H"d88095f4…"`) et la CI avec l'identité
de release (`certificate root = H"d051d7d8…"`). Deux exigences désignées
différentes, donc deux octrois différents — l'app installée en demande un, **une
fois**, et le garde ensuite d'une mise à jour à l'autre puisque la racine, elle,
ne bouge plus. Le message de la sonde dit exactement quelle case cocher.

#### ✅ Le volet Internet s'affiche — et deux défauts d'apparence (09/09/2026)

**Vu à l'écran par Bruno, sur le paquet livré tel quel** : la barre latérale
d'Installer.app affiche Introduction · License · Destination Select ·
Installation Type · **Internet** · Installation · Summary, et le volet montre sa
phrase et sa case cochée. Le volet fonctionne, sur macOS 15.6.1, sans correctif.

⚠️ **Ce paragraphe remplace une conclusion fausse, et la méthode qui l'a produite
mérite d'être retenue.** Piloter Installer.app par SSH avait donné quatre
« signaux » convergents — parcours trop court, bundle absent de `lsof`, pas de
fichier de transmission, journal muet — tous **artefacts du même blocage** : une
feuille modale de macOS (« this package will run a program… », bouton **Allow**)
arrêtait le parcours au premier écran. Le volet n'était donc jamais atteint : il
n'avait aucune raison d'écrire son fichier, et le processus interrogé n'y était
pas encore arrivé. Quatre observations tirées d'un même montage cassé ne sont pas
quatre preuves indépendantes — c'est une seule erreur, comptée quatre fois. La
seule mesure qui tranchait était l'œil d'un humain devant la machine.

**Sur `NSPrincipalClass`, qui n'était donc pas le problème.** Le `Info.plist`
déclare `NSPrincipalClass = InstallerSection`, la classe **de base** du
framework et non une sous-classe — ce qui s'écarte du contrat d'Apple, et ce que
la mesure ci-dessous confirme. Mais puisque le volet s'affiche, macOS l'accepte :
c'est une entorse sans conséquence, à laisser telle quelle plutôt qu'à
« corriger » sur du code qui marche. Relevé en chargeant les deux bundles dans le
runtime Objective-C (`NSBundle` + `principalClass`, hors de tout Installer.app) :

| | livré par la CI | patché |
|---|---|---|
| `NSPrincipalClass` déclaré | `InstallerSection` | `MWInternetSection` |
| le bundle se charge | oui | oui |
| classe principale résolue | `InstallerSection` | `MWInternetSection` |
| **est la classe de base elle-même** | **OUI** | non |
| **est une sous-classe** | **non** | **OUI** |
| `MWInternetPane` enregistrée | oui | oui |

⚠️ **L'expérience était viciée, et le montage l'a montré tout seul.** Le paquet
patché, ouvert par Bruno, ne montrait plus le volet — mais un second paquet,
assemblé de la même façon et dont le `Info.plist` n'avait **pas** été touché, ne
le montrait pas davantage. Le point commun n'est donc pas la classe principale,
c'est **la manière de réassembler le paquet** : `pkgutil --expand` → remplacer le
blob `PlugIns` → `pkgutil --flatten` perd la section des plugins, alors même que
le blob se relit correctement. Un paquet ne se rebricole pas à la main ; il
s'assemble avec `productbuild --plugins`, ce que fait `build-pkg.sh`, et ce qu'il
faut faire aussi pour tout paquet de banc (la recette sans Xcode : `pkgutil
--flatten` sur le *composant* seul pour le remettre à plat, puis `productbuild`
avec la Distribution, les Resources et le dossier de plugins).

Conclusion sur `NSPrincipalClass` : **non tranchée, et sans intérêt pratique**.
Ce qui est livré marche ; l'entorse au contrat d'Apple est réelle mais sans
conséquence observable, donc on n'y touche pas. Le paquet de test a par ailleurs
échoué à l'installation faute de privilèges — encore un symptôme du même
réassemblage bricolé, pas du paquet de la CI, qui s'installe.

**Deux pièges de banc à retenir**, qui ont coûté plusieurs passes chacun :
`screencapture` lancé depuis une session SSH n'a pas l'enregistrement d'écran, et
macOS lui rend alors le fond d'écran et la barre de menus **en effaçant les
fenêtres des autres applications** — les captures paraissent montrer un bureau
vide alors que la fenêtre est bien là (position et taille lues par l'API
d'accessibilité). Et le premier écran d'Installer est une **feuille modale** de
macOS (« this package will run a program… », bouton **Allow**, pas « Agree ») :
tant qu'elle n'est pas acquittée, aucun clic « Continue » n'avance.


## 26. Le pointeur absolu sous Wayland : la disposition vient du compositeur, pas de KMS (17/09/2026)

Issue #18 : un hôte Gentoo/Hyprland, les boutons marchent, le toucher (deltas)
marche, mais **la souris ne bouge pas**. Le client non verrouillé envoie une
position absolue ; le toucher, un delta relatif. Ce sont deux devices uinput
distincts (« MoonlightWeb Keyboard » pour les deltas, « MoonlightWeb Pointer »
pour ABS_X/ABS_Y), et seul le second échoue chez lui.

### 26.1 La chaîne absolue est saine jusqu'au compositeur

Relu de bout en bout : udev classe un device ABS_X/Y + BTN_LEFT en
`ID_INPUT_MOUSE` (le chemin prévu pour la souris VMware), libinput lui donne la
capacité pointer et émet `POINTER_MOTION_ABSOLUTE` sans exiger `INPUT_PROP_DIRECT`
ni `BTN_TOUCH`, aquamarine en fait un `warp`, et `CPointerManager::warpAbsolute`
étale la fraction 0..1 sur **la boîte englobante de tous les moniteurs, en pixels
logiques**. Mutter, KWin et sway font la même chose.

### 26.2 Le défaut : les CRTC ne disent rien sous Wayland

§23.3 construisait le « bureau » en unissant les sorties KMS actives, avec
`crtc->x / crtc->y` comme position. C'est vrai sous X11, où chaque moniteur
scanne une fenêtre du framebuffer racine. Un compositeur Wayland donne à chaque
sortie **son propre buffer** : tous les CRTC scannent depuis (0, 0), et la
disposition n'existe que dans le compositeur. À deux écrans, bureau =
(0,0,maxW,maxH), display = (0,0,w,h) : la fraction envoyée couvre au mieux la
moitié gauche de la vraie disposition, et le curseur se déplace **sur l'autre
écran**, hors du flux. Les clics tombent où le curseur est réellement, les deltas
partent de là : exactement le rapport. Le §23.3 disait lui-même « never run on a
real two-display Linux host ».

Même défaut sur la route portail : `PortalCapture::desktopRect()` rendait la
photo à l'origine, sans position.

### 26.3 Le correctif : demander la disposition logique

`input/linux/WaylandLayout` (dlopen de `libwayland-client.so.0`, comme
`X11Pointer` ouvre libX11 — aucun paquet `-dev`, un seul binaire pour X11,
Wayland et headless). Les deux interfaces de **xdg-output** sont décrites à la
main (deux tables `wl_interface`, l'ABI stable de libwayland) plutôt que de
tirer wayland-scanner dans le build. Le socket : `WAYLAND_DISPLAY` quand l'hôte
tourne dans la session, sinon balayage de `/run/user/*/wayland-*` (un hôte lancé
en service n'a pas d'environnement ; les chemins absolus sont acceptés par
libwayland ≥ 1.20). Rien n'est demandé au compositeur au-delà de sa liste de
sorties, que tout client peut lire ; la connexion vit le temps de `read()`.

`LinuxSession::readInputRects()` :

- **Route KMS** : la sortie dont le nom canonique égale le connecteur capturé
  donne le display (rectangle logique), l'union de toutes les sorties donne le
  bureau. Nom canonique : minuscules, et `HDMI-A-1` → `hdmi-1`, parce que
  **Mutter nomme `HDMI-1`** ce que le noyau, wlroots et KWin nomment `HDMI-A-1`
  (constaté sur l'UM790Pro, GNOME 42).
- **Route portail** : la réponse `Start` porte `position (ii)` et `size (ii)`,
  **déjà en coordonnées logiques du compositeur** (spécification du portail,
  fournies par Mutter, KWin et xdg-desktop-portal-hyprland). `PortalScreenCast`
  les lit désormais, `PortalCapture::desktopRect()` les rend, et le bureau est
  l'union des sorties Wayland — sans nom à apparier, un rectangle suffit. Sans
  position, la sortie unique de la même taille est prise pour le moniteur.
- **Sinon** (X11, headless, compositeur sans xdg-output, connecteur absent de la
  liste) : KMS garde le dernier mot, avec une ligne qui dit pourquoi.

Les fonctions de choix (`pickWaylandRects`, `waylandDesktopUnion`,
`findWaylandOutputAt`, `canonicalConnectorName`) sont de l'arithmétique pure dans
l'en-tête, testées sur les trois plateformes (`test_wayland_layout.cpp`) ; le
socket lui-même est exercé par le même test là où il y en a un.

### 26.4 Les logs `[PTR]`, pré-release seulement, temporaires

Le rapporteur ne peut pas être joint sur sa machine ; il lui est demandé des
**lignes de log**, pas des commandes. `Edition::extraDiagnostics()` (build DEV,
instance `--dev`, ou build staging `-stg`/`.stg`) arme
`NativeHost::setPointerDiagnostics()` — dans `main()` **et** dans le worker de
flux, même piège que le drapeau clavier. Sous ce drapeau, `UinputInput` et
`readInputRects()` écrivent, taggées `[PTR]` : les devices créés et si X11 est
joignable, les sorties KMS et Wayland vues, les rectangles display/bureau
retenus et leur source, les 5 premiers mouvements absolus (position client →
point bureau → valeur ABS), les 3 premiers deltas et boutons, et une seule fois
une écriture uinput refusée. Une install PROD n'écrit rien de tout ça. **À
retirer à la fermeture de #18** : tout est derrière `pointerDiagnostics()`.

**Retirés le 23/09/2026**, avec leur plomberie (`Edition::extraDiagnostics()`,
`NativeHost::setPointerDiagnostics()`, le drapeau d'`IInputSink.h`) et les
lignes Windows du §27. Les lignes `[native] input: pointer mapped on …` restent :
elles ne dépendaient d'aucun drapeau.

Vérifié : 3775/3775 checks Windows (dont la section Wayland, arithmétique
seule) ; UM790Pro GNOME 42 Wayland, build complet propre, tests verts, la session
loggue « pointer mapped on the Wayland layout » par les deux chemins de socket
(`WAYLAND_DISPLAY` et balayage `/run/user`), `HDMI-A-1` apparié à `HDMI-1`. ⚠️
Toujours pas d'hôte Linux à deux écrans sous la main : le cas du rapport reste à
confirmer par lui, avec ses lignes `[PTR]`.

**Concrètement, pour l'utilisateur** : sur un hôte Wayland à plusieurs écrans,
la souris du navigateur arrive sur l'écran qu'il regarde, à l'endroit visé, y
compris avec une échelle fractionnaire et quel que soit l'écran choisi — par le
scanout comme par le portail. Sur un seul écran rien ne change. Et s'il reste un
cas tordu, la version pré-release le raconte dans son journal sans qu'il ait à
ouvrir un terminal.

## 27. Le pointeur dessiné par le téléphone n'est plus recalé par l'hôte (17/09/2026)

Vu sur l'iPhone de Bruno, hôte DualRTX sous Windows 11, le 16/09 : appui long
sur la barre de titre d'une fenêtre pour l'attraper, puis, au premier mouvement
du doigt, la fenêtre et le pointeur partent d'un coup dans le coin haut-gauche
de l'écran. Tant que le doigt reste immobile, rien ne bouge, quelle que soit la
durée de l'appui ; c'est le mouvement qui déclenche le saut.

### 27.1 Ce que le journal a montré

Rejoué le 17/09 avec les lignes `[PTR]` du §26 étendues à Windows (positions
absolues injectées, boutons, rapports de position renvoyés au client) :

```
12:32:24.189 [PTR] button 1 down, held now: 0
12:32:24.703 [PTR] pointer report VISIBILITY JUMP: hidden at frame 0,0 (...) previous visible at 717,442
12:32:30.868 [PTR] absolute JUMP: client 1,2 of 1920x1080 -> desktop 1,2 (previous 957,590)
12:32:33.745 [PTR] button 1 up, held now: 1
12:32:33.750 [PTR] pointer report VISIBILITY: visible at frame 159,225 (...) previous hidden at 0,0
```

Une demi-seconde après l'appui du bouton gauche sur une barre de titre, Desktop
Duplication déclare le pointeur **caché, en (0, 0)**, et le maintient ainsi
jusqu'au relâchement — alors que Windows l'affiche, à sa place, pendant tout
le glissement. Le rapport de position (`cursorpos`, §20.11 et commit 4baa4b91)
transmettait ce mot tel quel au téléphone.

### 27.2 Deux causes, une par bout

**Côté téléphone.** Depuis le 12/09 le téléphone dessine le pointeur lui-même
et l'hôte lui renvoyait sa position toutes les 50 ms pour « corriger la dérive »
une fois le doigt immobile 150 ms. Le front appliquait les coordonnées jointes
même quand l'hôte disait « caché » : le pointeur dessiné se retrouvait en
(0, 0) sans que rien ne le montre, et le premier delta du doigt, converti en
position absolue depuis ce point, téléportait le pointeur de l'hôte — et la
fenêtre tenue — dans le coin. Le recalage n'existe plus : le premier mot de
l'hôte place le pointeur (s'il est visible), ensuite seul le doigt le déplace.
Comme chaque mouvement part en position absolue, le pointeur de l'hôte est de
toute façon sous le dessin ; un mot de l'hôte ne pouvait qu'être en retard
d'un aller-retour, ou faux. Décision de Bruno : c'était perturbant, et c'est
retiré (`_clientCursorHostSaid`, `StreamView.js`).

**Côté hôte.** `DxgiDuplication::updateCursor` confronte désormais un « caché »
de Desktop Duplication à `GetCursorInfo`, comme `Win32Cursor` le fait déjà pour
WGC : si Windows montre le pointeur sur cet écran, il est visible, à la position
que Windows donne (ramenée de l'espace virtualisé DPI aux pixels capturés).
Tant que cette substitution tient, chaque image relit Windows, y compris quand
Desktop Duplication ne signale aucune nouvelle du pointeur. Une ligne de log
la première fois par session. Cela corrige aussi le mode composité (jeu, ou
`MOBILE_CURSOR_CLIENT_DRAWN = false`), où le pointeur disparaissait de l'image
pendant un glissement de fenêtre. ⚠️ Cette dernière phrase était une déduction,
et elle est fausse : l'image porte le pointeur pendant le glissement, et dire
« visible » à un client qui dessine le sien en montrait deux — voir §29.

Pourquoi Desktop Duplication cache le pointeur pendant la boucle de déplacement
d'une fenêtre n'est pas établi — hypothèse : Windows passe en curseur logiciel
pendant cette boucle, et DXGI ne rapporte que le curseur matériel. Le
contournement ne dépend pas de la réponse.

### 27.3 Vérifié

Windows (DualRTX) : tests natifs 3772/3772, tests front 726/726, instance
`--dev` relancée avec les deux correctifs. Confirmation sur l'iPhone : à faire
par Bruno sur cette instance. ⚠️ Les lignes `[PTR]` Windows (`Win32Input`,
`WindowsSession`) sont temporaires, à retirer avec celles du §26.

**Concrètement, pour l'utilisateur** : sur un téléphone ou une tablette, le
pointeur dessiné ne bouge plus que sous le doigt. Attraper une fenêtre par sa
barre de titre et la déplacer la laisse suivre le doigt, sans saut, et le
pointeur reste visible pendant tout le glissement. Sur un PC client rien ne
change.

## 28. Le downscale : ce que fait la passe de conversion, et le banc pour la remplacer (17/09/2026)

### 28.1 Ce qui est fait aujourd'hui

Quand le client demande moins que l'écran (1440p → 1080p), la réduction est
faite **dans la passe RGB → YUV**, par le viewport plus petit que la source et
un sampler `D3D11_FILTER_MIN_MAG_MIP_LINEAR` sur un SRV à un seul mip
(`ColorConvert.cpp:463`, `:623`, `:678`) ; Linux idem en GLES (`GlConvert.cpp:329`,
`:524`) ; le palier CPU Linux fait un bilinéaire 16.16 (`BgraToI420.h:97`) ;
macOS ne réduit pas lui-même, ScreenCaptureKit livre la taille demandée
(`SckCapture.mm:379`). Donc sur Windows et Linux : **un fetch bilinéaire, noyau
de 2 texels quel que soit le ratio, en espace gamma** (SRV `_UNORM`, pas
`_SRGB`). À 0,75 il manque un tiers du noyau : aliasing et scintillement sur
les traits fins au défilement, battement de netteté de période 4, bords
assombris par la moyenne en gamma ; et la chroma 4:2:0 est rééchantillonnée
depuis la source pleine résolution, ratio 2,67 en un seul tap. Les échelons
75 %/50 % d'`EncodeLoadCap` retombent sur les mêmes ratios.

La capture ne peut pas produire plus petit : DDA donne le mode
(`DxgiDuplication.cpp:184`), WGC la taille de l'item (`WgcCapture.cpp:270`), et
le host ne change jamais le mode de l'écran. Le Selector ramène toujours la
demande à la forme de l'écran (`Selector.cpp:351`), donc le pipeline ne voit
qu'une réduction à aspect identique ; seul un changement de mode en session
avec `followDisplayShape=false` crée un écart, étiré sur Windows/Linux,
letterboxé sur macOS (`FrameFit.h`).

### 28.2 Le banc

Décision de Bruno (17/09) : macOS intouché ; tier CPU Linux et encodeur
logiciel Windows intouchés (machines lentes) ; Linux GPU → priorité perf ;
Windows encodeurs matériels → meilleur rapport qualité/perf ; le choix vient
d'un banc et non d'une théorie. Le banc est `mw-scaler-bench`
(`backend/native-host/tools/scaler-bench/`, `-DMW_BUILD_TOOLS=ON`), décrit en
§8j de `docs/bench-native-host.md` : temps de la passe seule par timestamps
GPU, qualité contre une référence Lanczos-3 linéaire, test de défilement
(gain de mouvement / flicker), crops, rapport HTML interactif ; tous les GPU
de la machine, SDR et HDR (synthétisé), perceptuel et linéaire, fp32 et
`min16float` ; FSR1, NIS, SGSR1 et `ID3D11VideoProcessor` en plus des
filtres classiques.

Ce que le premier passage a déjà établi, sans attendre la campagne : NIS
refuse toute réduction par contrat ; FSR1 fp16 est faux sur NVIDIA (chemin
`min16float` de fxc) ; les timestamps 3D ne voient pas `VideoProcessorBlt`
sur NVIDIA et Intel (chrono mur à la place, non comparable) ; NVIDIA fait du
bilinéaire dans son VideoProcessor, Intel quelque chose de mieux, AMD quelque
chose de plus flou. Sur 1440p → 1080p SDR, le bilinéaire actuel a un flicker
de 0,16 pour un gain de 0,78 ; le même en linéaire 0,087 / 0,91 pour le même
prix (16 µs sur la 5060 Ti) ; un Lanczos-3 dilaté en linéaire fait 0,0002 /
1,00 pour 450 µs. **La décision (quel filtre, sur quel tier) attend la
campagne complète et le choix de Bruno** ; l'intégration (passe
intermédiaire, chroma en vraie moyenne 2×2, letterbox aligné sur macOS,
`MW_SCALER` pour l'A/B) est décrite dans le plan et reste à faire.

### 28.3 Ce que la campagne a dit, et ce qui est intégré (17/09/2026, soir)

Campagne complète : 5 cas × SDR/HDR × RTX 5060 Ti, Arc A380, iGPU Radeon
(2 CU), 300 runs par candidat (`C:\Test\scaler-bench\campaign-1`). Trois
verdicts, tous les cas confondus :

1. **Le bilinéaire en gamma est le pire endroit où être.** 1440p → 1080p sur
   le RTX : 16 µs, flicker 0,159, gain de mouvement 0,78, 28,7 dB. Le même
   fetch en lumière linéaire : même prix, flicker 0,086, gain 0,91, 33,3 dB.
2. **Les filtres à rayon fixe ne sont pas des réponses** (Catmull-Rom /
   Mitchell 9 fetches, Lanczos fixe, FSR1, SGSR1) : nets mais aliasés à 1,33,
   effondrés à ratio 2 (flicker 1,5–1,6 à 1440p → 720p — plus d'énergie
   parasite que de mouvement réel). Des reconstructeurs, pas des passe-bas.
3. **Seuls les noyaux dilatés au ratio tiennent partout.** Lanczos-2 dilaté,
   linéaire, sur le RTX : 1440p → 1080p 261 µs, flicker 0,017, gain 0,91,
   38,5 dB ; 4K → 720p 408 µs, 0,031, 0,87, 39,1 dB (le bilinéaire y fait
   4,56 / 22,9 dB). Arc : 480 µs ; iGPU 2 CU : 3,5 ms (mais son bilinéaire
   fait déjà 300 µs). Lanczos-3 dilaté = la référence à 60 dB, pour 2× le
   prix et rien de visible en plus. Le VideoProcessor NVIDIA est un
   bilinéaire, Intel un peu mieux, AMD plus flou ; aucun ne compose le
   curseur. fp16 : jamais plus rapide, faux sur FSR1 (NVIDIA) et Lanczos-3
   dilaté (RTX, AMD).

**Décision de Bruno** : Windows encodeurs matériels → Lanczos-2 dilaté
linéaire ; Linux GPU → bilinéaire linéaire d'abord, Lanczos-2 si le 780M le
permet ; macOS, tier CPU Linux, encodeur logiciel Windows intouchés.

**Intégré** (`ScaleFilter.h`, `ColorConvert`, `GlConvert`) :

- une **passe de rééchantillonnage séparable** avant la conversion : Lanczos-2
  dilaté au ratio, horizontale dans un intermédiaire FP16 linéaire (largeur
  de sortie × hauteur source), verticale dans une image à la taille de
  sortie que les passes luma/chroma lisent ensuite **en 1:1** — la chroma
  4:2:0 redevient une vraie moyenne 2×2. 2 × 7 taps à 1440p → 1080p là où le
  banc mesurait un noyau carré de 49 ; 2 × 13 à 4K → 720p contre 169 ;
- **en lumière linéaire** : sur D3D11 le 8 bits est décodé par tap à l'aller
  et ré-encodé gratuitement par une RTV `_SRGB` sur une texture TYPELESS
  (lue en `_UNORM` par la conversion) ; sur GLES pas de vue sRGB sur un
  DMA-BUF importé, donc la passe verticale ré-encode elle-même vers un RGBA8
  — c'est aussi pourquoi le « bilinéaire linéaire » n'est **pas** gratuit sur
  Linux (il faudrait décoder 4 taps à la main, autant que la passe H de
  Lanczos-2) : le tier GPU Linux part donc directement en Lanczos-2, et le
  chiffre du 780M tranche ;
- **1:1 → rien** : `scaleFilter()` retombe sur Bilinear, aucune passe, aucune
  texture ;
- **letterbox** sur ce chemin : une source d'une autre forme (changement de
  mode avec `followDisplayShape=false`) est ajustée entre des bandes noires
  via `FrameFit.h`, comme macOS ; le chemin bilinéaire étire toujours ;
- **choix par tier** : `WindowsSession::buildPipeline` donne Lanczos2 à
  NVENC/AMF/VPL/MF et Bilinear à l'encodeur logiciel ; `GpuPipeline::init`
  (Linux) donne Lanczos2, le `CpuPipeline` est intouché ; `MW_SCALER=
  bilinear|lanczos2` force l'un ou l'autre pour l'A/B ; GLES sans
  `GL_EXT_color_buffer_float` retombe en bilinéaire avec une ligne de log ;
- **test** `test_color_convert.cpp` sur WARP : un damier 1 px réduit de
  moitié donne le gris de mi-lumière (code 177) par Lanczos-2 et le gris
  gamma (126) par le bilinéaire — les deux chemins ne peuvent pas être
  confondus — ; 1:1 sans passe ; 64×32 → 32×32 letterboxé, bandes à 16,
  image centrée. 3769/3769 sur DualRTX.

**Le 780M a répondu** (UM790Pro, Ubuntu 22.04, Mesa 23.2, `test_linux_session`
« a 720p stream of a bigger display », 1920×1080 → 1280×720, ratio 1,5) :
conversion **0,57 ms** par image en Lanczos-2 contre 0,45 ms en bilinéaire —
0,12 ms de plus, l'encodeur en prend 1,5 — donc Lanczos-2 **reste le défaut
du tier GPU Linux**. 4000/4000 checks sur la machine.

**Reste** : l'A/B sur un vrai flux par encodeur (`MW_SCALER=bilinear` contre le
défaut, DualRTX NVENC/VPL, UM790Pro VA-API) et le visuel client 1:1 — à
Bruno, sur le banc réel.

**Concrètement, pour l'utilisateur** : sur une machine à encodeur matériel, un écran 1440p ou 4K streamé en 1080p arrive désormais sans le scintillement du texte au défilement et nettement plus fidèle (+6 à +16 dB), pour quelques centaines de microsecondes de GPU par image ; un écran qui change de forme en cours de route montre des bandes noires au lieu d'une image écrasée. À la résolution native, ou sur une machine qui encode au CPU, rien ne change.

---
## 29. Deux souris pendant un glissement de fenêtre, et la souris qui attend un clic en mode jeu (18/09/2026)

Deux signalements de Bruno sur le client Desktop, le 18/09 :

1. **Game mode ON, la session démarre, la souris reste au client.** Le pointeur
   du navigateur est visible par-dessus l'image et peut sortir du cadre tant
   qu'on n'a pas cliqué dans l'image. Attente : « Game mode » veut dire « la
   souris appartient à l'hôte », dès la première image.
2. **Game mode OFF, on attrape une fenêtre par sa barre de titre :** deux
   pointeurs à l'écran pendant tout le glissement, celui du client et celui de
   l'hôte.

### 29.1 Le deuxième pointeur vient de l'image, pas du client

C'est la suite directe du §27. Desktop Duplication déclare le pointeur **caché**
pendant toute la boucle de déplacement d'une fenêtre ; l'hypothèse posée là-bas
— Windows repasse en **curseur logiciel** pendant cette boucle — est justement
ce que « caché » veut dire chez DXGI : *le pointeur n'est plus sur son plan
matériel, il est peint dans l'image du bureau*. Le §27 a bien rétabli la
visibilité et la position à partir de `GetCursorInfo`, mais il l'a dit au
client qui, lui, dessine le sien : deux pointeurs, celui que Windows a peint
dans l'image et celui que le navigateur pose par-dessus. Le rapport de Bruno est
la preuve directe que l'image le contient — et donc que la phrase du §27.2 sur
le mode composité était une déduction, pas une observation : le pointeur n'y
disparaissait pas, il était déjà dans l'image.

Donc `CursorState` gagne `inImage` : *l'image porte déjà le pointeur*. Transitoire,
contrairement au verdict par écran de `PaintedPointer.h` — il dure le temps du
glissement. `DxgiDuplication` le lève exactement quand la substitution du §27
joue (DXGI dit caché, Windows dit montré), et deux conséquences en découlent :

- **ce qu'on dessine** — `WindowsSession::pointerToDraw()` remplace les quatre
  `composite ? cursor() : kNoCursor` du boucle de capture : pas de pointeur si
  le client dessine le sien, et pas de pointeur non plus si l'image le porte
  déjà. Sans quoi le mode composité dessinait par-dessus le vrai une **forme
  périmée** (DXGI n'envoie plus de forme pendant le glissement), et une forme
  inversante — l'I-beam — inversée deux fois devient invisible. Le chemin
  `PointerOnly` sort tôt pour la même raison : un pointeur peint dans le bureau
  bouge avec le bureau, il n'y a rien de nôtre à redessiner ;
- **ce qu'on dit au client** — `reportCursor` et `reportCursorPosition`
  rapportent `visible && !inImage`. « Pas visible » est la vérité utile pour un
  client qui dessine : il n'a rien à poser, l'image s'en charge. Il cache donc le
  sien pendant le glissement et le retrouve au relâchement, quand DXGI reprend
  la main. C'est aussi, mot pour mot, ce que Bruno attendait : le pointeur passe
  à l'hôte seul le temps du glissement.

Rien ne change quand DXGI parle normalement : `inImage` reste faux, et les
chemins macOS et Linux ne le lèvent jamais.

### 29.2 Le mode jeu prend la souris à la première image

`_autoCapturePointer()` (`StreamView.js`) demande le verrou dès la première
image décodée — le mode a été choisi avant que le flux existe, le redemander
d'un clic n'apprend rien à personne. Jamais sur une vue en attente (la promotion
d'une vue standby a sa propre passation), jamais si une autre vue tient le
verrou.

Un navigateur peut vouloir un geste à lui, et celui qui a démarré la session a
plusieurs secondes quand l'image arrive. Sur refus, `_armPointerCapture()` arme
un **coup unique sur la première touche** : le premier W de la partie sert de
geste. Les clics dans l'image capturaient déjà (gestionnaire de clic du mode
jeu), d'où le clavier seul ici. Désarmé dès que le verrou arrive, au changement
de mode et à la fermeture. Et passer le mode jeu en cours de session demande le
verrou tout de suite : la bascule est elle-même le geste.

Sur refus des deux, le comportement est celui d'aujourd'hui : l'indice « cliquer
pour capturer » reste affiché et fonctionne.

### 29.3 La souris de l'hôte qui « se recentre » : c'est une application, pas nous

Troisième signalement du même jour : sur DualRTX, la souris physique de l'hôte
revient toujours au centre. Relevé sur la machine, sans session native ouverte :

- la position est **figée à 853,480**, soit exactement le centre de l'écran
  principal (2560×1440 à 150 %, donc 1706×960 en coordonnées virtualisées) ;
- un `SetCursorPos` vers un autre point est **défait en moins de 100 ms**, et un
  `SendInput` relatif ne déplace rien ;
- `GetCursorInfo` rend `CURSOR_SHOWING` avec un **handle de curseur applicatif**
  (ni `IDC_ARROW` ni aucun curseur système), dont la forme décodée est **vide**
  (ink 0×0) ;
- `hl.exe` (Counter-Strike) et `portal2.exe` tournent.

C'est la signature d'un jeu qui garde la souris : il pose son propre curseur
(vide), la replace au centre à chaque image et lit le mouvement comme un écart
à ce centre. C'est précisément le comportement pour lequel `RecentreDetector`
existe côté injection (§ input) — mais côté souris physique de l'hôte, rien
dans MoonlightWeb ne peut ni ne doit intervenir : le worker n'injecte que sur
un événement du client, et il n'y en avait aucun. **Verdict : hors périmètre.**

⚠️ Conséquence pour les tests : `test_win32_cursor` lit le pointeur **réel** du
bureau. Tant qu'un jeu tient la souris avec une forme vide, trois checks
tombent (`inkWidth > 0`, `inkHeight > 0`, `drawn + inverting > 0`) — un fait sur
la machine, pas sur le code. À rejouer jeux fermés.

### 29.4 La forme du curseur ne suit plus la souris physique de l'hôte

Quatrième signalement : bouger la souris **physique** de l'hôte ne déplace pas
le pointeur du client (attendu — il est piloté en absolu depuis le navigateur),
mais **changeait sa forme**. La raison est directe : l'hôte rapporte la forme
que son pointeur survole, et son pointeur n'est pas toujours celui du client.
Une main sur la souris de l'hôte qui traverse un champ de texte faisait
apparaître un I-beam sous une main qui n'avait pas bougé.

Le client garde donc la forme reçue mais ne la **peint** que si son propre
pointeur a bougé récemment (`CLIENT_POINTER_FRESH_MS`, 500 ms — la réponse de
l'hôte à notre propre mouvement arrive un aller-retour plus tard, il ne faut pas
la rejeter). Sinon elle attend : chaque `mousemove` relit la forme courante
(`_applyLocalCursor`), donc le prochain mouvement du viewer l'applique. La toute
première forme d'une session est toujours peinte — il n'y en a pas d'autre, et
rien n'a encore bougé par définition.

### 29.5 Le pointeur parti sur l'autre écran revient au milieu — là où on ne le voit pas

Demandé d'abord pour le mode bureau (un signe au centre de l'image quand le
pointeur de l'hôte n'est pas sur l'écran stream), livré comme tel, puis **repris
par Bruno le jour même** : en mode bureau le problème n'existe pas — le pointeur
de l'hôte suit le curseur du navigateur, le viewer voit toujours le sien, et un
signe de plus au milieu ne fait que gêner. La flèche centrale et le drapeau
`elsewhere` qui la portait (message `cursor`, `CursorState`, `CursorUpdate`,
signal `cursorShapeChanged`) sont **retirés en entier**.

Ce qui manquait est ailleurs : **quand le pointeur est dessiné DANS l'image** —
mode jeu, ou le trackpad d'un téléphone — l'écran stream est tout ce que le
viewer voit. Un pointeur parti sur l'autre moniteur de l'hôte est alors un
pointeur piloté à l'aveugle : le mouvement relatif le déplace toujours, les
clics arrivent toujours, et rien ne se montre. C'est donc **la souris de l'hôte**
qu'il faut ramener, pas un dessin qu'il faut ajouter.

`WindowsSession::recentrePointerIfAway()` demande à Windows (`GetCursorInfo`,
pas à la capture : « pas visible » de Desktop Duplication couvre aussi le
pointeur qu'une application cache, et celui-là ne nous regarde pas) si le
pointeur est hors du rectangle de l'écran capturé, toutes les 200 ms, et
seulement tant que `m_CompositeCursor` est vrai. Si oui, une position absolue au
centre part par le puits d'entrée ordinaire — donc rectangle d'écran,
virtualisation DPI et détecteur de recentrage s'appliquent une fois, là où ils
vivent déjà. En mode bureau, rien : le client a son pointeur et le place lui-même.

Borné à **5 tentatives** : une application peut tenir le pointeur sur l'autre
écran et le reprendre à chaque image (Counter-Strike laissé tourner sur l'écran
principal fait exactement ça, cf. §29.6). On perdrait ce bras de fer cinq fois
par seconde pour toute la session, avec un pointeur qui clignote entre les deux
écrans. Le compteur repart à zéro dès que le pointeur est revu sur cet écran.

**Au démarrage seulement (05/10).** Ramené toute la session, le pointeur ne
pouvait plus partir sur l'autre écran de l'hôte : la personne devant l'hôte le
voyait revenir au milieu de l'écran streamé à chaque essai (test manuel du flux
commun, S9). Décision de Bruno : « seulement au démarrage partout ». Dès que le
pointeur est vu sur l'écran capturé, quel que soit le mode, le démarrage est
fini et plus rien ne le ramène. Les 5 tentatives épuisées finissent aussi le
démarrage. L'exception des jeux qui détachent le pointeur (Roblox bouton droit
tenu sur le Mac, `CgInput::postRelative`) est une autre affaire et ne change pas.

### 29.6 Mode bureau + jeu qui recentre le pointeur : la visée tournait

Cinquième signalement, le même jour, et le plus grave : Counter-Strike (GoldSrc)
et Portal 2 (Source) streamés en mode bureau (Game mode **OFF**, le défaut), la
vue « part au ciel et au sol » au moindre mouvement ; Ravenfield (Unity), lui,
se joue très bien dans le même mode, et tout va bien en Game mode ON.

**Le mécanisme.** En mode bureau le navigateur envoie *où est* son pointeur,
et l'hôte y place le pointeur système (`MOUSEEVENTF_ABSOLUTE`). Un jeu à
l'ancienne lit la souris en **ramenant le curseur au centre de sa fenêtre à
chaque image** et en mesurant de combien il a été poussé depuis. Les deux se
battent : le jeu recentre, la position client suivante remet le pointeur là où
est celui du viewer, et le jeu lit toute cette distance — une demi-fenêtre —
comme un coup de poignet. Un jeu en raw input (Unity) ne recentre jamais, et
le bureau non plus : seul le genre recentreur casse, et il casse complètement.
Le log du matin de Bruno le montrait en clair : `pointer report JUMP` à chaque
image, en alternance entre 960,540 (le centre de la fenêtre) et la position du
viewer.

**Deux correctifs, un par côté.**

1. *Client, mode jeu* (2c13f5ff) : le verrou demande `unadjustedMovement`, donc
   des deltas en counts de souris, sans l'accélération du navigateur qui
   s'ajoutait à celle de l'hôte (×3,3 mesuré sur le banc du §pointer lock).
2. *Hôte, mode bureau* (edd2b58b pour Windows, puis macOS et Linux) :
   `RecentreDetector`, arithmétique pure et neutre, partagée par les trois
   puits. Avant chaque placement l'hôte **regarde où est le pointeur**. Trouvé
   ailleurs qu'où il l'avait mis, **au même point trois fois de suite** (à 2 px
   près, l'axe absolu 16 bits arrondit), c'est qu'une application le recentre :
   à partir de là chaque position client part comme **le delta entre deux
   positions consécutives**, exactement ce que le mode jeu envoie, et le jeu
   mesure la distance que le viewer a réellement parcourue. Quand le pointeur
   cesse de revenir à ce point pendant **300 ms** (menu ouvert, jeu quitté), les
   placements reprennent — 300 ms et pas un seul raté, parce qu'entre un
   placement et l'image suivante du jeu le pointeur est légitimement ailleurs.
   Une main sur la souris physique de l'hôte ne revient jamais au pixel : elle
   ne déclenche rien. **Le point doit être sur l'écran capturé** : un jeu laissé
   en arrière-plan sur l'*autre* écran de l'hôte recentre tout autant (GoldSrc
   le fait même sans le focus — vu le 18/09 à 17 h 08, CS derrière un stream du
   second écran : le détecteur l'a suivi, les clics partaient dans le jeu, le
   stream semblait figé), et le suivre emmènerait la souris hors de l'écran que
   le viewer regarde. Hors écran, rien à apprendre : placements ordinaires.
   Un delta d'un quart d'écran ou plus est un pointeur qui
   **rentre dans l'image** loin de là où il en était sorti, pas un geste : il
   est jeté (`isReentryJump`).

Par hôte, la lecture et le delta :

| Hôte | Lire le pointeur | Envoyer le delta |
|---|---|---|
| Windows | `GetCursorPos` | `MOUSEEVENTF_MOVE` |
| macOS | `CGEventCreate` + `CGEventGetLocation` | événement posé à *ici + delta*, champs `kCGMouseEventDeltaX/Y` renseignés (un événement Quartz porte toujours une position ; c'est celui qu'une souris bougée depuis là produirait) |
| Linux X11 | `XQueryPointer` (déjà là pour ramener le pointeur sur l'écran) | `REL_X/REL_Y` sur le device clavier |
| Linux Wayland | personne ne peut lire le pointeur d'autrui | le détecteur ne décide jamais : comportement inchangé, le mode jeu reste la réponse |

**Mesuré sur DualRTX**, CS 1.6 au premier plan, auto-stream en mode bureau
piloté par CDP (recette dans la mémoire de session) : détection après trois
événements (« keeps putting the pointer back at 1280,720 » dans le log), zéro
`JUMP` ensuite, un balayage de 400 px à droite puis à gauche ramène la vue au
pixel, Échap vers le menu → placements en 1,5 s et le curseur suit à nouveau,
reprise → re-détection. macOS : compilé sur mw-mac, tests unitaires verts ; pas
de jeu recentreur sur le Mac de banc, le chemin Quartz n'est pas mesuré.

**Mesuré sur l'UM790Pro en GNOME Xorg** (session basculée le temps du test),
avec une sonde de banc `recentre-live` : une fenêtre X qui, comme un jeu SDL en
mode *warp*, ramène le pointeur au centre (960,540) toutes les 4 ms et cumule
l'écart lu, et le vrai `UinputInput` nourri de positions client à 125 Hz :

| Phase | Résultat |
|---|---|
| A — bureau, 20 placements | le pointeur suit 20/20 |
| B — jeu, balayage +400 px puis −400 px | détection au 3ᵉ événement (ligne « keeps putting the pointer back at 960,540 »), 80 warps par balayage, le jeu lit ≈ +470 puis −556 |
| C — le jeu lâche | « no longer put back » et placements suivis à nouveau **362 ms** après (300 ms de grâce + cadence) |
| D — le jeu reprend | re-détection, +200 px lus ≈ +236 |

Deux écarts attendus dans ce que le jeu lit : le device relatif uinput passe par
l'**accélération de libinput** (×1,2 à 1,4 à cette vitesse, la même chose que
l'*enhance pointer precision* de Windows sur `MOUSEEVENTF_MOVE`), et le
placement absolu d'**avant** la détection est lu une fois comme un coup depuis
le centre (−360,−40 ici : les trois premiers événements d'une prise en main,
inhérent à la règle des trois retours, Windows pareil).

**Limites assumées.** Les deltas dérivés de positions gardent l'accélération
du navigateur client (pas de pointer lock en mode bureau) en plus de celle de
l'hôte : la visée est un peu moins précise qu'en mode jeu, qui reste la
référence pour un FPS. Sortir de l'image arrête le mouvement, comme avant.

**Concrètement, pour l'utilisateur** : il lance son FPS depuis le stream sans
toucher au réglage souris. Si le jeu est du genre qui recentre le curseur, la
vue suit la main au lieu de partir au ciel ; s'il ouvre le menu, la souris
redevient un pointeur ordinaire. Le mode jeu reste là pour la précision
maximale — et sous Wayland il reste nécessaire pour ces jeux-là.

### 29.7 Vérifié

DualRTX : build complet propre, `mw-native-tests` **4136/4136** — les 3 checks
de `test_win32_cursor` qui tombaient plus tôt sont revenus au vert dès que le jeu
a rendu la souris, ce qui confirme au passage le §29.3 — ESLint et Prettier
propres sur `StreamView.js` et `stream.css`. **À confirmer par Bruno sur le banc
réel** : mode jeu qui prend la souris au démarrage ; glissement de fenêtre avec
un seul pointeur, en mode bureau **et** en mode jeu (c'est le mode jeu qui dirait
si l'image ne portait finalement pas le pointeur : il disparaîtrait pendant le
glissement au lieu d'être doublé) ; forme du curseur inchangée quand la souris
physique de l'hôte bouge ; et le pointeur ramené au centre quand il est parti sur
l'autre écran, **en mode jeu et sur le trackpad d'un téléphone seulement**.

**Concrètement, pour l'utilisateur** : en Game mode, la session s'ouvre avec la
souris déjà dans le jeu — plus de pointeur de navigateur qui traîne sur l'image
ni de sortie accidentelle du cadre — et si le pointeur de la machine était resté
sur son autre écran, il revient au milieu de l'écran joué au lieu d'être piloté à
l'aveugle. En mode bureau, attraper une fenêtre par sa barre de titre ne montre
plus deux souris : pendant le glissement c'est celle de l'hôte qui commande, la
sienne s'efface, et elle revient dès qu'on relâche ; et la forme du curseur ne
change plus toute seule quand quelqu'un touche la souris de la machine.

## 30. Le worker natif élevé : les fenêtres administrateur répondent (22/09/2026)

Le serveur démarre par la tâche de logon `\MoonlightWeb`, au niveau **non élevé**,
et son worker de stream hérite de ce jeton. Windows (UIPI) refuse alors à ce worker
toute fenêtre qui tourne en administrateur — Gestionnaire des tâches, console
admin, jeu lancé en admin — et, tant qu'elle a le focus, **toutes** les entrées :
c'est le bandeau « Une fenêtre administrateur a le focus sur l'hôte »
(`inputGateHostUnelevated`, raison `"uipi"` de `Win32Input`).

**Niveau 1 (fait) : seul le worker est élevé.** L'installeur, déjà élevé,
enregistre une tâche sans déclencheur `MoonlightWeb Stream Worker`
(`HighestAvailable`, `Parallel`, `Priority 4`, cachée) dont l'action est notre exe
sous `{app}` : `--stream-worker --worker-pipe $(Arg0)`. Pour chaque session
native, `StreamWorkerHost` la lance par `IRegisteredTask::RunEx` au lieu d'un
`QProcess` (`ConsoleProcess::startThroughTask`). Le serveur, le tray et le
navigateur qu'il ouvre restent au niveau de l'utilisateur ; la bascule « Start at
login » continue d'écrire une tâche `LeastPrivilege` sans élévation.

- **Transport.** Une tâche ne transmet pas de handles : trois **tubes nommés**
  (`<produit>-worker-<pid>-<128 bits aléatoires>-in/out/err`), une instance chacun,
  `FILE_FLAG_FIRST_PIPE_INSTANCE`, `PIPE_REJECT_REMOTE_CLIENTS`, DACL
  `D:P(A;;GA;;;<SID de l'utilisateur>)` (la DACL par défaut laisserait lire
  Everyone). Le worker s'y connecte (`ConsoleSession::attachWorkerPipes`) et les
  met sous ses fd 0-2 : un exe sans console démarre avec ces fd **libres**, il
  faut donc d'abord rouvrir les trois flux sur `NUL`, sinon `_open_osfhandle`
  rend le fd 0 lui-même et le `_close` qui suit le referme (premier essai :
  « No config on stdin »). Le reste — lignes JSON, lecteurs, attente de sortie —
  est le code de `ConsoleProcess`, les lectures passant en `OVERLAPPED`.
- **Contrôle croisé.** Le worker refuse un serveur de tube dont l'image n'est pas
  le même exe (`GetNamedPipeServerProcessId`) : n'importe quel processus de
  l'utilisateur peut lancer la tâche, seul notre serveur obtient un worker élevé.
  Le serveur vérifie de même l'image du client avant d'écrire la config.
- **Quand.** `elevatedWorkerAvailable()` : Windows, pas un service, processus
  non élevé, tâche présente **avec cet exe pour action** (un build `build\` ne
  lance jamais le worker de l'installation). Un échec (tâche absente, 10 s sans
  connexion) repasse par `QProcess` et désactive le détour pour le reste du run.
- **Mesuré sur DualRTX** (tâche dev `MoonlightWeb-dev Stream Worker`, UAC
  « never notify ») : une fenêtre WinForms élevée au premier plan sur l'écran
  streamé reçoit les clics envoyés par le stream ; même scénario avec la tâche
  en `LeastPrivilege` → aucun clic, `input gate closed … (UIPI)`. Arrêt :
  `exitCode 0`. Sans tâche : `Worker spawned` par `QProcess`, comme avant.
- **Droits observés sur le worker élevé depuis un processus moyen** :
  `PROCESS_TERMINATE` et `QUERY_LIMITED` accordés (le Planificateur crée le
  processus ainsi), `VM_WRITE`, `CREATE_THREAD`, `DUP_HANDLE`, `QUERY_INFORMATION`
  refusés — pas d'injection. `kill()` tente `TerminateProcess`, sinon ferme stdin
  (le worker sort sur EOF). Ré-enregistrer une tâche en `HighestAvailable` sans
  élévation : « Access is denied ».
- **Limites.** Un compte standard obtient son jeton ordinaire (le bandeau reste
  juste). L'invite UAC et l'écran de verrouillage sont sur le **bureau sécurisé**,
  hors de portée d'un processus utilisateur même élevé : c'est le niveau 2.

## 31. Le worker SYSTEM : le bureau sécurisé répond aussi (23/09/2026)

Le niveau 1 achète toutes les fenêtres administrateur, pas le **bureau
sécurisé** : l'invite UAC, l'écran de verrouillage et l'écran Ctrl+Alt+Suppr
vivent sur `Winlogon`, un autre bureau de la même station, et Windows n'y laisse
entrer que SYSTEM. C'est le niveau 2, et c'est ce que fait Parsec.

### 31.1 Le service lanceur

Un service **LocalSystem** minuscule, `MoonlightWeb Worker`
(`WorkerService.{h,cpp}`), dont l'unique travail est de démarrer le worker en
session console sous un jeton SYSTEM. L'exe s'enregistre lui-même
(`--worker-service-install` / `--worker-service-remove`, lancés élevés par
l'installeur) et tourne sous `--worker-service`. Démarrage **automatique** et
non « à la demande » : un service à la demande obligerait à accorder
`SERVICE_START` à l'utilisateur interactif, et le droit de démarrer un processus
LocalSystem est une chose plus grosse à distribuer que les quelques centaines de
kilo-octets que coûte l'attente sur un tube.

Le jeton : celui du service, dupliqué, puis déplacé vers la session console par
`SetTokenInformation(TokenSessionId)` — qui demande `SeTcbPrivilege`, que
LocalSystem a. `lpDesktop` vaut explicitement `winsta0\default` : le défaut d'un
service est une station que personne ne regarde, et un worker démarré là
capturerait le vide.

**Ce qui n'est PAS élevé** : le serveur. Le processus qui écoute sur le réseau,
décode le WebRTC, dessine le tray et ouvre le navigateur garde le niveau de
l'utilisateur, exactement comme avant.

**Le transport** ne bouge pas : les trois tubes nommés du niveau 1, créés par le
serveur, leur nom passé en paramètre. Une seule différence, la DACL, qui gagne
`(A;;GA;;;SY)` — le worker est SYSTEM et n'ouvrirait pas son propre stdin sans.

### 31.2 La frontière de confiance, dite franchement

N'importe quel processus de l'utilisateur console peut demander un worker au
service et obtient un processus SYSTEM branché sur des tubes qu'il a créés.
C'est une vraie frontière, et c'est la même que celle des services de Parsec et
de Sunshine. Elle est réduite à ce que la fonction exige :

- le service lance **une** commande, l'image sous laquelle il a été installé,
  avec des arguments fixes — rien de la requête ne devient un chemin, un drapeau
  ou un mot de shell ;
- la requête ne porte qu'un **nom de tube**, accepté seulement en
  `[A-Za-z0-9._-]{1,128}` : pas de séparateur, pas de `..`, pas d'UNC ;
- l'appelant doit être sur la **session console** (son jeton est impersonné au
  niveau identification et son `TokenSessionId` comparé) et son image doit être
  **ce même exe** ;
- le worker vérifie la même chose dans l'autre sens avant de lire un octet de
  config ;
- le serveur vérifie aussi le worker (son image, avant de croire les tubes).
  Il ne le peut que parce que le service donne au worker une DACL à lui :
  SYSTEM et les administrateurs comme d'habitude, et l'utilisateur qui l'a
  demandé avec `PROCESS_QUERY_LIMITED_INFORMATION`, `SYNCHRONIZE` et
  `PROCESS_TERMINATE` — ce qu'il a déjà sur son worker élevé, rien qui touche
  la mémoire, les threads ou le jeton (28/09, voir §31.7) ;
- la DACL du tube de contrôle est `D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x12019f;;;IU)`
  — la DACL par défaut d'un tube laisserait entrer toutes les sessions
  d'ouverture de la machine.

Ce qui n'est **pas** prétendu : un utilisateur capable de lancer l'exe installé
peut obtenir un worker SYSTEM et lui passer une session à lui. Le propriétaire
de la machine installe ça exprès, comme il installe Parsec ; la fonction ne peut
pas exister sans cette étape, et le dire vaut mieux que faire croire que le
contrôle la referme.

### 31.3 Suivre la bascule de bureau

Les deux moitiés du stream sont **par thread** :

- **Desktop Duplication** duplique le bureau du thread **appelant**. Sur un
  thread resté sur `Default` pendant que l'UAC est affichée, `DuplicateOutput`
  répond « only a SecureUI is displayed ».
- **SendInput** injecte dans le bureau du thread **appelant**. Depuis `Default`,
  rien n'atteint une invite UAC.

D'où `platform::attachThread()` (`InputDesktop.{h,cpp}`) : `OpenInputDesktop` +
`SetThreadDesktop`, l'ancien handle fermé **après** la bascule seulement (fermer
le bureau sur lequel un thread se tient est indéfini). Tout y est un no-op qui
répond faux hors SYSTEM, donc le moteur se comporte exactement comme avant.

- **Capture** : l'appel est en tête de `openCapture()`, ce qui couvre le seul
  site qui compte — `restartCapture()`, qui tourne **sur** le thread de capture,
  et où l'attente « l'écran est absent » du §30 devient une reconnexion.
- **Entrée** : `SetThreadDesktop` refuse de déplacer un thread qui possède une
  fenêtre ou un hook, et le thread qui livre l'entrée fait tourner une boucle
  d'événements Qt — laquelle possède une fenêtre interne sous Windows. Il ne
  suivra **jamais**. L'entrée passe donc par un thread dédié, créé par
  `Win32Input::start()` **et seulement quand le worker est SYSTEM** : file
  `deque` + condition variable, bornée à 4096 événements. C'est l'exception
  assumée au « pas de file, pas de saut de thread » d'`InputEvent.h` — elle
  coûte un réveil de condition variable (quelques dizaines de µs contre 8 ms de
  frame) et un worker ordinaire injecte toujours sur le thread appelant, sur le
  chemin exact qui avait été mesuré.
- À chaque bascule détectée (nom du bureau relu au plus toutes les 100 ms), ce
  qui était enfoncé est **oublié** sans être relâché : l'état appartenait au
  bureau qui vient de partir, et un key-up envoyé au nouveau serait une touche
  que personne n'a pressée.

### 31.4 Ctrl+Alt+Suppr

Ce n'est pas une combinaison de touches : Windows la réserve dans le noyau et
aucune entrée injectée ne la fabrique. Elle passe par `SendSAS` (`sas.dll`),
réservé à SYSTEM. Chaîne complète : Ctrl et Alt verrouillés puis `Del`, sur la
barre de touches tactile → message `secureattention` → les trois relais →
`IMediaEngine::sendSecureAttention()`. Côté natif, un
`InputEvent::Type::SecureAttention` qui finit sur le thread suiveur — donc sur
le bureau qui vient d'apparaître. Côté GameStream, `MoonlightShim` envoie les
trois touches réelles : sur un hôte dont le propre service sait lever le bureau
sécurisé, c'est ce qu'il faut ; ailleurs ce sont trois touches ordinaires, ce
qui reste plus utile qu'un refus.

La barre de touches est le **seul** chemin : le système d'exploitation du
spectateur avale la combinaison physique avant que la page ne la voie, sur
toutes les plateformes. Jusqu'au 29/09/2026, une touche `C+A+Suppr` dédiée la
portait. Bruno l'a fait retirer : le geste sert trop rarement pour une touche à
lui, et Ctrl, Alt puis `Del` est celui du clavier. La barre reconnaît ce `Del`
et envoie le message à la place des trois touches, qu'aucun Windows
n'accepterait.

### 31.5 La dégradation

`StreamWorkerHost::start()` essaie de haut en bas, et chaque cran perd de la
portée sans rien changer au stream :

| Chemin | Jeton | Atteint |
|---|---|---|
| service lanceur | SYSTEM, bureau console | bureau sécurisé compris |
| tâche élevée (§30) | jeton complet de l'utilisateur | fenêtres administrateur |
| enfant simple | jeton ordinaire | ce qui marchait avant |

Un échec coûte ce lancement-là, jamais le stream : on tombe à la ligne suivante,
et les deux détours se coupent d'eux-mêmes pour le reste du run après un échec.

### 31.6 Journal et diagnostics

L'AppData d'un processus SYSTEM est celui de `systemprofile`, où personne ne
regarde. Le service passe donc au worker `--worker-data-dir <AppData de
l'utilisateur console>` (obtenu par `SHGetKnownFolderPath` avec le jeton de
l'utilisateur), et le journal comme les minidumps du worker SYSTEM atterrissent
à côté de ceux du serveur. Le service lui-même écrit dans
`moonlightweb-worker-service.log`, sous l'AppData de SYSTEM — le seul endroit
où un processus LocalSystem est certain de pouvoir écrire.

Le worker SYSTEM lit aussi les réglages de l'utilisateur par ce chemin (vu le
28/09 : `keyboard_debug`, posé à la main, y est actif). Toute la configuration
de session arrive de toute façon par stdin ; il ne reste que le bouton de
débogage clavier, qui se tait sur le bureau sécurisé (§31.7).

### 31.7 Vérifié

Compilation complète propre (MSVC, warnings as errors), `mw-native-tests`
**4129/4129**, ESLint + Prettier propres sur `StreamViewKeyboard.js`,
clang-format 19.1.7 propre sur `backend/src`. **À confirmer par Bruno sur une
vraie installation**, en trois temps, depuis l'iPhone : (1) l'invite UAC
apparaît dans le stream et le bouton « Oui » répond au clic ; (2) Win+L puis
déverrouillage au mot de passe tapé depuis le client ; (3) le bouton
`C+A+Suppr` ouvre l'écran de sécurité. Puis les mêmes trois avec le service
arrêté (`sc stop "MoonlightWeb Worker"`), qui doivent retomber sur le niveau 1 —
stream normal, fenêtres admin toujours pilotables, bureau sécurisé noir.

**Le 28/09, première vraie installation (C8.4 du plan D3D12) : le niveau 2
n'avait jamais servi.** Le service était installé et démarré, mais chaque
session le refusait (« pipe client is "" (pid N), not this executable ») et
tombait sur la tâche élevée : REALTIME, mais ni duplication (`0x80070005`) ni
`SendInput` (erreur 5) sur `Winlogon`. Verrouillé depuis le stream, l'écran du
PIN ne prenait ni souris ni clavier. La cause : le serveur vérifie l'image du
worker, et un processus créé avec un jeton SYSTEM hérite de la DACL par défaut
de ce jeton, qui n'admet que SYSTEM et les administrateurs ; le serveur, non
élevé, lisait une image vide. Toute la v0.3.1 en est là (journaux de la prod
depuis le 26/09). Correctif `f1e8e8e3` : la DACL du worker du §31.2. **Vérifié
par Bruno** depuis son Mac, par Internet : verrouillage, PIN tapé dans le
stream, déverrouillage. Le journal dit « SYSTEM (launcher service) »,
« GPU scheduling class REALTIME (token SYSTEM) », « now on the "Winlogon"
desktop », une duplication rouverte sur `Winlogon` et la chaîne D3D12 de l'Arc
reconstruite des deux côtés de la bascule, sans passer par WGC ni D3D11.

Trouvé au même test : `keyboard_debug`, posé à la main sur ce poste, a écrit le
PIN touche par touche dans le journal du worker. Les diagnostics clavier se
taisent désormais sur le bureau sécurisé, quoi que dise le réglage
(`1887b2ea` : `NativeHost::secureDesktopHasInput()`, demandé à chaque touche
par `Win32Input` et par une sonde du codec d'entrée) ; les lignes déjà écrites
ont été effacées et le réglage coupé sur ce poste.

**Le 29/09, `C+A+Suppr` essayé de bout en bout : il n'ouvre rien.** Un Chrome
de l'UM790Pro, en mode tactile émulé (la barre n'existe que sur un appareil
tactile), streame l'écran principal de DualRTX par l'édition dev installée
(`0.3.1-f1e8e8e3-dev`, worker SYSTEM).
- Toute la chaîne répond : le journal du worker dit « input: Ctrl+Alt+Suppr
  sent », `SendSAS(FALSE)` est appelé par le processus SYSTEM, sans erreur.
- Mais l'écran de sécurité ne vient pas : la duplication n'est pas perdue, et
  l'image reste le bureau.
- La cause probable est la stratégie « générer une séquence d'attention
  sécurisée par logiciel » (`SoftwareSASGeneration`, dans
  `HKLM\…\Policies\System`), absente sur ce poste. Sans elle, Windows ignore
  `SendSAS`, même venant d'un service.

Le produit ne touche pas à cette stratégie (§31.4). La poser à « services »
(1), dans l'installeur ou au moment de l'appel, est une décision de Bruno. Tant
qu'elle n'est pas posée, le bouton est sans effet. Le verrouillage (Win+L) et le
déverrouillage, eux, marchent (vérifié le 28/09).

**Le même jour, la vraie cause, et le correctif (`456acb1e`).** Bruno a choisi
l'installeur. Mais la stratégie posée à la main n'a rien changé : même journal,
et toujours aucun écran.
- Un service jetable, LocalSystem en session 0, a fait le même appel. L'écran
  de sécurité est apparu en moins d'une seconde : `LogonUI` lancé dans la
  session de l'utilisateur.
- **Windows n'honore `SendSAS` que venant d'un service en session 0.** Il
  l'ignore, sans un mot, venant d'un processus SYSTEM de la session console, ce
  qu'est le worker.

Le correctif :
- **Le worker demande l'appui au service lanceur**, sur son tube de contrôle,
  par la requête `sas!`. Aucun nom de tube ne peut prendre cette forme.
- Le service vérifie l'appelant comme pour un worker : même exécutable, session
  console. Il lit la stratégie, pour qu'un refus s'écrive au journal avec sa
  raison, puis il appuie (`WorkerService::requestSecureAttention`,
  `NativeHost::setSecureAttentionSender`).
- Seul un worker SYSTEM demande : lui seul peut suivre l'écran qui s'ouvre, et
  recevoir l'Échap qui le ferme. `sas.dll` est chargée depuis System32
  seulement.
- **L'installeur pose la stratégie** (1, les services) quand personne ne l'a
  posée.
  - Chaque édition qui s'appuie sur une valeur posée par l'un de nos
    installeurs est inscrite sous `HKLM\SOFTWARE\MoonlightWeb\SoftwareSASGeneration`.
  - Une désinstallation ne retire la valeur que si elle vient de nous, qu'aucune
    autre édition ne s'en sert, et qu'elle vaut toujours 1.
  - Une valeur posée par un tiers (un administrateur, une stratégie de groupe,
    un autre bureau à distance) reste telle quelle.

**Trouvé au passage : un stream ne démarrait pas si l'écran de sécurité était
déjà affiché**, et donc pas davantage sur un poste verrouillé (`0x80070005`).
- La duplication s'ouvrait depuis le fil de l'appelant, le fil Qt du worker. Ce
  fil possède une fenêtre, et `SetThreadDesktop` refuse de le déplacer
  (erreur 170) : il duplique `Default`, que Windows refuse pendant que
  `Winlogon` a la main.
- Elle s'ouvre désormais depuis un fil à elle, qui ne possède rien et suit le
  bureau d'entrée. Le fil de capture rejoint ce bureau avant de lire.

**Vérifié le 29/09 sur DualRTX**, avec l'édition dev `0.3.1-456acb1e-dev`
installée par son installeur, qui a posé la stratégie et s'est inscrit comme
propriétaire. Le Chrome de l'UM790Pro, en tactile émulé, pilote le stream :
- **`C+A+Suppr`** : l'écran de sécurité s'affiche dans le stream (« now on the
  "Winlogon" desktop », duplication rouverte), et **Échap** le referme.
- **Un stream lancé sur l'écran de sécurité** démarre dessus (« the capture
  opens from a thread on the "Winlogon" desktop »), prend l'Échap, puis revient
  au bureau.

Captures : `bench-out\d3d12v2\sas`.

**Les gestes de Bruno, faits le 29/09 vers 09:15** depuis son iPhone en 5G, sur
l'édition dev `0.3.1-81c55309-dev`, qui n'a plus de touche dédiée (§31.4) :
- **Ctrl, Alt puis `Del`** sur la barre de touches : « Ctrl+Alt+Suppr sent »,
  l'écran de sécurité s'ouvre dans le stream. Bruno y choisit Verrouiller.
- **Deux streams relancés sur le poste verrouillé**, l'un sur l'Arc, l'autre
  sur la RTX, démarrent sur l'écran de verrouillage (« the capture opens from a
  thread on the "Winlogon" desktop »).
- Son code, tapé au clavier de l'iPhone, rend le bureau (« now on the "Default"
  desktop », 09:15:30).

**L'invite UAC, vérifiée le même jour sur DualRTX**, avec la même édition et le
même client, qui streame cette fois l'écran principal (la RTX). Aucun poste du
banc n'affiche l'invite : Bruno a remis lui-même l'UAC à son réglage par défaut
le temps du test, puis l'a redescendu.
- Win+R, `cmd` puis Ctrl+Shift+Entrée, tapés dans le stream : l'invite
  « Windows Command Processor » s'affiche dans le stream (« now on the
  "Winlogon" desktop », duplication rouverte).
- **Le clic sur « Yes »**, à la souris depuis le client, ouvre « Administrator:
  C:\WINDOWS\system32\cmd.exe », et le worker revient sur `Default`. L'invite
  élevée a été refermée par un `exit` tapé dans le stream.
- C'est la première preuve de la **souris** sur le bureau sécurisé. Les essais
  précédents n'y envoyaient que le clavier (le PIN, Échap).

Captures : `bench-out\d3d12v2\uac`. Un piège du banc, pas du produit : l'hôte
tape la touche physique selon **sa** disposition. Un client qui simule des
touches doit donc envoyer la position AZERTY (le « m » est `Semicolon`) : avec
`KeyM`, « cmd » est arrivé en « c,d ».

**Concrètement, pour l'utilisateur** : le stream ne s'arrête plus devant une
porte. Quand Windows demande une autorisation administrateur, l'invite apparaît
à l'écran distant et le bouton « Oui » se clique comme n'importe quel autre ;
si la machine se verrouille, on tape son mot de passe depuis le client au lieu
de regarder un écran noir en attendant ; et Ctrl, Alt puis `Del` sur la barre de
touches ouvrent l'écran de sécurité, ce qu'aucune combinaison au clavier n'a
jamais pu faire depuis un navigateur. Si le service n'est pas installé ou a été
arrêté, rien ne casse : on retrouve exactement le comportement précédent.

## 32. Pipeline vidéo D3D12, deuxième essai (ouvert le 26/09/2026)

> Ébauche, complétée à chaque porte du chantier (branche `feat/d3d12-pipeline`).
> D3D11 reste le défaut partout où le banc et Bruno n'en ont pas décidé
> autrement : Intel passe en D3D12 le 28/09 (§32.9).

### 32.1 Ce que la première tentative a appris (21/09)

La v1 n'a fait tourner dans le produit qu'un **hybride** : la conversion passait
en D3D12, sur une file COMPUTE de priorité HIGH, et l'image revenait à D3D11
pour les encodeurs d'aujourd'hui. Aucun encodeur D3D12 n'a tourné dans le
produit. L'hybride a perdu partout, et `84524e7f` l'a retiré :

| GPU | Condition | D3D11 (total hôte moy. / p99) | Hybride |
|---|---|---|---|
| Arc A380 | RE9, stream réel | 19,8 / 61,4 ms | 30,5 / 90,1 ms : la file COMPUTE est préemptée par le jeu |
| Arc A380 | banc 1080p120 | 5,4 ms | 11-13 ms : 3,5 ms de travail + 2,5 ms d'attente de file |
| iGPU AMD | charge synthétique | 16,8 / 41 ms | 117 / 249 ms, 8 i/s : la file COMPUTE est affamée |
| RTX 5060 Ti | RE9 | 5,2 / 13,3 ms | 6,6 / 14,3 ms, 9 % d'images en moins |

Toutes ces mesures datent d'avant la classe GPU REALTIME (`ee7de92b`) et le
worker élevé. Deux sondes, en revanche, ont gagné : sur l'Arc sous RE9, D3D12
Video Encode encode en 2,3 ms (p99 3,6) là où oneVPL prend 7,2 ms (p99 15,4) ;
et la lecture par la file COPY est devenue `CrossGpuBridge` (`acdcda49`).

Les pièges relevés, gardés pour la v2 :
- un timestamp pris sur une file préemptée compte la préemption ;
- le `convert_us` de D3D11 ne mesure que la soumission CPU : le travail GPU de
  la conversion est facturé à `encode_us` ;
- le pilote Arc plante sur un `CopyTextureRegion` d'une texture planaire
  **partagée** dans une liste COMPUTE ;
- binding tier 1 : il faut des descripteurs nuls en bouche-trous ; pas d'UAV
  sRGB ;
- les BOOL rendus par le pilote Intel ne valent pas toujours 1 (tester `!= 0`) ;
- D3D12 Video Encode n'écrit que les slices : un PPS faux se décode en bouillie
  **sans erreur** ;
- le 4:4:4 (AYUV) est refusé partout en D3D12 Video Encode ;
- les surfaces de Desktop Duplication s'ouvrent en D3D12 par handle NT, pas
  celles de Windows.Graphics.Capture.

### 32.2 Pourquoi recommencer peut marcher

1. **Plus de renvoi à D3D11** : l'encodeur est lui aussi en D3D12. Ce qui a tué
   l'hybride, rendre l'image à D3D11 pour qu'un encodeur se resynchronise sur la
   file 3D, disparaît.
2. **La conversion sur la file DIRECT**, avec les mêmes pixel shaders que D3D11
   (sortie identique à l'octet), au lieu d'une file COMPUTE que l'Arc préempte et
   que l'iGPU AMD affame.
3. **Des priorités de file mesurées** : sous HAGS, les files DIRECT/COMPUTE d'un
   même créateur sont groupées et leur priorité de création est ignorée ; la file
   HIGH de la v1 ne valait sans doute rien sur la RTX. Un `CreatorID` propre et la
   priorité `GLOBAL_REALTIME` (le worker élevé ou SYSTEM des §30-31 tient le
   privilège) n'ont jamais été essayés.
4. **Sur Intel, l'encodeur est la vraie cible** : oneVPL est lent, sans
   invalidation de référence effective, et ne peut pas monter au-dessus de son
   débit de départ. L'Arc a le delta QP : un contrôle de débit maison devient
   possible sans reconfigurer l'encodeur.
5. **Le vrai jeu dès la première mesure** (RE9), jamais la charge synthétique
   seule.

### 32.3 La chaîne visée

Une image, un thread, aucune file ni tampon ajouté :

1. `AcquireNextFrame` sur le device D3D11 de capture ;
2. le contexte de capture signale une fence partagée (A) ;
3. la file de conversion D3D12 l'attend, convertit (mise à l'échelle, curseur,
   tone map) dans l'entrée de l'encodeur, puis signale une fence (B) ;
4. le contexte de capture attend B **sur le GPU**, puis `ReleaseFrame` : Desktop
   Duplication ne réécrit pas la surface trop tôt, et la CPU n'attend pas ;
5. la file d'encodage attend B, encode, puis signale C ;
6. la CPU attend C : c'est la seule attente CPU de l'image ;
7. le bitstream part, en-têtes compris sur une IDR.

Les étapes horodatées gardent leur sens : le total hôte se compare tel quel à
D3D11.

### 32.4 Ce qui protège D3D11

- Un réglage `native_video_pipeline` (`auto`, `d3d11`, `d3d12`) et un choix
  « Avancé » dans l'admin ; `auto` suit une ligne par vendeur, qui ne bouge que
  sur décision de Bruno : D3D11 partout au départ, Intel en D3D12 depuis le
  28/09 (§32.9).
- Un refus à la construction (Windows.Graphics.Capture, pont inter-GPU, 4:4:4,
  étage logiciel, codec pas encore fait…) repasse en D3D11 pour cette
  construction, avec la raison au journal. Un échec en cours de stream repasse en
  D3D11 pour la session, en une image clé, sans couper le stream.
- Le chemin D3D11 passe d'abord derrière une interface (`WindowsVideoPipeline`)
  **sans changement de comportement**, prouvé au banc avant toute ligne D3D12
  (porte G0).

### 32.5 Les portes

G0 (refactor sans régression), G1 (sondes : files, poignée de main DDA,
encodeurs), G2 (bout en bout avec D3D12 Video Encode), G3 (contrôle de débit
maison), G4 (NVENC et AMF en entrée D3D12). Chaque porte donne un rapport
chiffré et une recommandation ; Bruno tranche. Les résultats seront consignés
ici, porte par porte.

**Concrètement, pour l'utilisateur** : rien ne change tant qu'un type de GPU n'a
pas été basculé. Ensuite, sur un PC Intel, le plus répandu, l'image doit partir
plus vite de l'hôte quand un jeu charge la carte, le débit pouvoir remonter
au-dessus de celui du départ, et une perte réseau se réparer sans l'à-coup d'une
image clé, ce qui se sent surtout par Internet. Sur NVIDIA, l'objectif est de ne
rien perdre face à un chemin déjà très bon ; sur AMD, la mesure dira. Si quelque
chose échoue, le stream repasse seul en D3D11 sans se couper, et l'overlay comme
le journal disent quel chemin tourne et pourquoi.

### 32.6 Linux : la route scindée, et la règle qui encadre Vulkan (28/09/2026)

Phase 13 du plan : la même forme de chaîne sous Linux, en Vulkan. Les sondes
(banc §8o) ont réordonné le travail. Sous un jeu qui sature le 780M, la
conversion GL d'aujourd'hui attend deux images du jeu (46 ms) ; une file compute
Vulkan tourne à côté du jeu (10,6 ms sans privilège, 8,0 en HIGH). Le gain tient
à la file, pas à l'encodeur. D'où la **route scindée**, décidée le 28/09 (plan
§9-17) : la conversion passe en Vulkan compute, l'encodeur reste le VA-API
d'aujourd'hui.

**Les deux bouts, prouvés avant d'écrire le moteur.**
- L'entrée : le tampon KMS (DCC d'AMD en trois plans) s'importe dans Vulkan avec
  son modificateur, sa barrière implicite attendue en `sync_file`, et se lit au
  pixel près comme EGL le lit (§8o.2).
- La sortie : la surface d'entrée de VA-API est linéaire sur AMD ; Vulkan écrit
  ses deux plans en images de stockage R8 et RG8, et VA-API relit exactement ce
  qui a été écrit (§8o.4). Ni copie, ni changement de propriétaire de la surface.

**Le code.**
- `platform/linux/vulkan/VulkanDevice` : le chargeur ouvert par `dlopen`, le
  périphérique du GPU trouvé par son nœud de rendu (`VK_EXT_physical_device_drm`),
  une file compute, en HIGH seulement si le processus tient `CAP_SYS_NICE` et
  qu'une soumission le prouve — une file d'encodage en HIGH se crée sur le 780M
  et le noyau refuse sa première soumission (§8o.3).
- `convert/linux/VulkanConvert` : le jumeau de `GlConvert`, même interface,
  shaders transcrits ligne à ligne (`shaders/vk_scale.comp`, `vk_nv12.comp`,
  compilés en SPIR-V par `glslangValidator` au build). Sur le 780M, les deux
  écrivent la même image à une valeur près au plus, en 1:1, en Lanczos-2 et en
  bilinéaire (`test_vulkan_convert`).
- `core/LinuxRouteChoice.h` : le choix de la chaîne, pur et testé, à chaque
  construction. La clé de banc, puis le réglage, puis une table par vendeur ; la
  table dit GL partout tant qu'un banc et Bruno n'ont pas bougé une ligne (AMD
  l'a été le 28/09, §32.8). La clé `convert=vulkan` prend la route scindée.

**La règle (Bruno, 28/09) : jamais Vulkan forcé.** Ce qui ne peut pas tourner
est refusé, nommé, et la chaîne descend d'un cran — Vulkan Video → VA-API → CPU,
Vulkan compute → GL. Une conversion Vulkan qui ne démarre pas (pas de chargeur,
pas de Vulkan 1.3, un modificateur qu'elle n'importe pas) laisse GL convertir dès
le départ ; une qui lâche en plein stream (périphérique perdu, tampon refusé) est
remplacée par GL sur l'image même, avec une image clé, pour le reste de la
session. La route, sa raison et le refus éventuel vont au journal et dans
`SessionInfo` (overlay, banc). Pour l'encodeur Vulkan à venir, le banc a montré
que ni le numéro du micrologiciel ni la version de Mesa ne disent « fiable »
(§8o.3) : il ouvrira sur une preuve au pixel et, s'il l'échoue, laissera VA-API.

**Concrètement, pour l'utilisateur** : rien ne change par défaut sous Linux tant
que le banc n'a pas mesuré la route et que Bruno n'a pas basculé AMD. Ensuite, sur
un PC Linux AMD, l'image n'attendra plus derrière le jeu pour être convertie. Et
si la carte ou son pilote ne s'y prêtent pas, le stream reste sur le chemin
d'aujourd'hui, sans coupure.

### 32.7 Linux : la chaîne Vulkan Video, prise sur la parole du pixel (28/09/2026)

C13.5 du plan : la chaîne entière en Vulkan, sur un seul périphérique. La capture
KMS est convertie en compute directement dans l'image d'entrée de l'encodeur,
puis encodée en HEVC par Vulkan Video. Une copie par image, le flux.

**Le code.**
- `encode/linux/VulkanHevcEncoder` : le jumeau Linux de `VideoEncode12`.
  - Il possède son entrée : une image NV12 aux dimensions codées (des CTB
    entiers, la leçon de C5.3) et deux vues de plan, R8 et RG8, que la
    conversion écrit en stockage. Entre deux images, elle reste en
    `VIDEO_ENCODE_SRC`. Les rangées et colonnes hors de l'image sont noires,
    écrites une fois.
  - L'encodage attend la conversion sur le GPU, par le sémaphore de la
    dernière soumission (`VulkanDevice::afterLast`) : c'est lui qui rend
    visibles les écritures de la file compute. La CPU n'attend que le flux.
  - Nos en-têtes en structures StdVideo, les octets ceux du pilote
    (`vkGetEncodedVideoSessionParametersKHR`), posés juste devant les
    tranches : une IDR sort d'un seul tenant. Les premières tranches sont
    relues avec ces en-têtes (la garde de `VideoEncode12`).
  - **La profondeur de transformée complète** (CtbLog2SizeY − MinTbLog2SizeY) :
    le micrologiciel d'AMD découpe jusque-là quoi que dise le SPS, et à 2 toutes
    les P qui bougeaient étaient fausses sur le 780M (banc §8o.3).
  - `HevcDpb` : quatre images gardées, une perte guérie par un delta. Le CBR du
    pilote avec le VBV commun (`RateControl.h`), changé en vol par une commande
    de contrôle, sans remise à zéro.
  - La file d'encodage reste à la priorité par défaut : en HIGH, le noyau 6.8
    refuse sa première soumission (§8o.3).
  - Depuis le 29/09, les balayages d'intra-refresh remplacent les images clés
    pour le client qui traverse les pertes (C13.9, §32.24), et le bourrage du
    CBR de RADV est retiré avant le lien (§32.23).
- `encode/linux/VulkanHevcDecoder` : les yeux de la preuve. Un décodeur HEVC
  Vulkan Video juste assez large pour les flux du moteur (I et P, ensembles de
  références propres à la tranche, ni tuiles ni listes de quantification). Tout
  le reste est refusé par son nom. Sur VCN 4, la sortie est distincte du DPB,
  et RADV lit l'unité d'accès entière (lu dans Mesa, sous licence MIT).
- `encode/linux/VulkanHevcProof` : la preuve au pixel.
  - Une courte séquence qui bouge (les images du labo, celles qui avaient
    attrapé la faute) passe par l'encodeur du produit, à la taille du stream.
    En route, les images 4 et 5 sont perdues et guéries depuis la 3, et une
    image clé est demandée.
  - Le décodeur Vulkan du même GPU relit le tout, comparé image par image et
    rangée de CTB par rangée de CTB.
  - Juste : au moins 22 dB par image et 18 dB par rangée. Un flux juste en
    donne 37 à 40, les P fausses du §8o.3 en donnaient 5.
- `core/LinuxRouteChoice.h` : la preuve ne tourne que là où la chaîne serait
  prise (`linuxRouteWantsVulkanVideo`). La table des vendeurs dit VA-API
  partout, donc personne n'est obligé de prouver quoi que ce soit.

**Un import par conversion.** RADV 26 envoie toute la mémoire du périphérique
avec chaque soumission : sa liste de BO est globale. Un tampon d'affichage gardé
importé (le cache de `VulkanConvert` en gardait jusqu'à quatre) faisait attendre
chaque soumission, encodage compris, que le compositeur finisse l'image suivante
du jeu : 15 ms d'encodage sous charge au lieu de 2. Le tampon n'est donc plus
gardé d'une image à l'autre. Il est importé pour la conversion et relâché dès
la fin de l'attente, pour 0,02 à 0,04 ms (banc §8o.6).

**Le verdict, gardé.** Une preuve coûte 70 à 260 ms sur le 780M. Le verdict va
dans le cache de l'utilisateur (`$XDG_CACHE_HOME/MoonlightWeb/vulkan-video-proofs.txt`).
Il est rangé sous ce qui pourrait le changer : le GPU (UUID), le pilote et sa
version, le noyau, le micrologiciel VCN (lisible par tous sous amdgpu), la
taille, et la révision de l'encodeur du moteur. Seule une comparaison est
gardée ; une preuve qui n'a pas pu tourner est reposée la fois suivante.

**La règle de Bruno, appliquée au bout.**
- Un pilote qui ne montre pas d'encodeur (le Mesa 23.2 d'Ubuntu 22.04) est
  refusé par son nom, sans même ouvrir de périphérique.
- Un pilote qui code autre chose que son SPS échoue la preuve.
  `MW_VK_ENCODE_DEPTH=2` rejoue la faute du §8o.3 pour les tests.
- Une chaîne qui ne démarre pas laisse VA-API encoder dès le départ.
- Une chaîne qui lâche en plein stream, à la conversion comme à l'encodage,
  cède la place à VA-API sur l'image même, avec une image clé, pour le reste de
  la session.

La raison est chaque fois dans le journal et dans `SessionInfo`. Testé sur le
780M avec les deux pilotes (`test_vulkan_hevc`, `test_linux_session`).

**Concrètement, pour l'utilisateur** : rien ne change par défaut. La chaîne
Vulkan n'est prise que si on la demande (réglage, clé de banc), en attendant
que le banc tranche par vendeur. Même demandée, elle ne sert que sur une carte
dont le pilote a prouvé, image à l'appui, qu'il encode juste. Sinon le stream
part sur VA-API comme aujourd'hui, et la raison est écrite dans le journal.

### 32.8 Linux : la route scindée par défaut sur AMD (décision de Bruno, 28/09/2026)

Plan §9-20, sur la foi du banc §8o.5. Sous un jeu qui sature le 780M, la
conversion GL attendait le jeu : 16 images captées par seconde sur 45, et 38 ms
de la présentation à l'image encodée. La file compute de Vulkan tournait à côté
du jeu : 44 images, 8 ms. Au repos, elle gagnait encore 0,3 ms.

**Ce qui change.** La ligne AMD de la table des vendeurs
(`autoLinuxConversion`) passe à Vulkan compute devant VA-API. L'encodeur reste
VA-API : la chaîne Vulkan Video reste derrière le réglage jusqu'à G5 (§9-21),
et `auto` ne la prend jamais.

**Ce qui ne change pas.**
- Intel et NVIDIA gardent GL.
- Le portail garde GL, même sur AMD. Seul l'import du tampon KMS a été mesuré
  au pixel (§8o.2) ; les tampons de PipeWire attendent leur banc (C13.3).
  **Levé le 29/09 (§32.21)** : le portail en DMA-BUF a eu son banc (§8o.11),
  et la route scindée y gagne aussi.
- `vaapi` choisi dans l'admin, ou `pipeline=vaapi` au banc, reprend la chaîne
  d'avant, GL devant VA-API. C'est le retour arrière, sans clé de banc.
- La clé `convert=gl|vulkan` passe devant tout, pour les bancs.

**Le repli, en deux marches au besoin.** Une conversion Vulkan qui ne démarre
pas (pas de chargeur, pas de Vulkan 1.3) laisse GL convertir dès le départ.
Une qui lâche en plein stream (périphérique perdu, tampon refusé) cède la place
à GL sur l'image même. Nouveau depuis cette décision : sur AMD, la chaîne
Vulkan Video qui lâche descend d'abord sur la route scindée, qui peut refuser
le même tampon. La session redescend alors jusqu'à GL, toujours sur la même
image, au lieu de couper le stream (`convertPicture`). La raison est au
journal ; `refused` dit qu'une conversion demandée n'a pas pu tourner.

**Concrètement, pour l'utilisateur** : sur un PC Linux à carte AMD, rien à
régler. Sous un jeu qui occupe tout le GPU, le stream capte toutes les images
du jeu au lieu d'une sur trois, avec 8 ms de retard au lieu de 38. Si la carte
ou son pilote ne s'y prêtent pas, le stream reprend l'ancien chemin sans
coupure, et « VA-API » dans l'admin y revient à la main.

### 32.9 Windows : Intel en D3D12 par défaut (décision de Bruno, 28/09/2026)

Plan §9-23, sur la foi de G3 (§8n.6 à §8n.10 du banc) et du test de Bruno
(C5.7). Sur l'Arc, par Internet depuis un Mac, le bureau passe de 12,0 à 6,0 ms
sur l'hôte, et de 63,6 à 47,7 ms du clic au photon (médianes). Sous RE9, le jeu
tient 30 à 35 i/s dans le stream en D3D12, contre 25 à 30 en D3D11, qui saute en
plus des images. Sur le N95, D3D12 est plus rapide au repos (9,5 ms contre
13,2) et égal sous une charge qui sature l'iGPU.

**Ce qui change.** La ligne Intel (oneVPL) de la table des vendeurs
(`autoVideoPipeline`) passe à D3D12 : conversion sur la file DIRECT, D3D12
Video Encode HEVC, contrôle de débit maison. L'overlay lit « D3D12 VE
(Intel) ».

**Ce qui ne change pas.**
- NVIDIA et AMD gardent D3D11. Sur la RTX, VE met 21 ms là où NVENC en met
  environ 2 ; l'iGPU AMD perd 6,7 à 8,1 ms au repos (G2). Leur voie D3D12
  passe par leurs SDK en entrée D3D12 (phase 7, G4).
- `d3d11` choisi dans l'admin, ou `pipeline=d3d11` au banc, reprend la chaîne
  d'avant. C'est le retour arrière, sans clé de banc.
- Ce que la route D3D12 ne porte pas encore reste en D3D11, pour la
  construction concernée : H.264 (un navigateur sans HEVC), AV1, 4:4:4,
  Windows.Graphics.Capture, pont inter-GPU. Le journal dit pourquoi, et
  l'overlay lit « oneVPL (D3D11) » : `refused` vaut aussi quand c'est la table
  qui a demandé D3D12.

**Windows 10.** Sans `ID3D12VideoDevice3`, il n'y a pas de D3D12 Video Encode.
La première construction le découvre avant de faire la conversion, et le
processus s'en souvient, un worker étant une session. Les constructions
suivantes choisissent D3D11 d'emblée, au lieu de faire puis défaire une chaîne
D3D12 à chaque reconstruction.

**Concrètement, pour l'utilisateur** : sur un PC à carte ou à puce graphique
Intel, rien à régler. L'image part plus vite de l'hôte (deux fois plus vite
sur une Arc), et un jeu garde sa cadence dans le stream ; par Internet, le clic
arrive à l'écran environ 16 ms plus tôt. Si le PC ne s'y prête pas (Windows 10, un navigateur
sans HEVC), le stream prend l'ancien chemin sans coupure, et « D3D11 » dans
l'admin y revient à la main.

### 32.10 Windows : la robustesse, mise à l'épreuve (phase 8, 28/09/2026)

Depuis §32.9, D3D12 porte les streams d'Intel par défaut. La phase 8 vérifie
sa promesse avant la fusion de la branche, sur l'Arc et le N95.

**Les pannes injectées (C8.1).** `MW_D3D12_FAULT=<panne>@N`, dans
l'environnement du banc ou d'un worker, provoque une panne à la N-ième
conversion de la chaîne :
- `encode` : l'image est jetée comme une erreur du pilote ;
- `convert` : la liste est refusée ;
- `timeout` : la file attend une fence que personne ne signale, jusqu'à ce que
  la chaîne abandonne ;
- `removed` : `ID3D12Device5::RemoveDevice`, comme un TDR ;
- `open` : la N-ième chaîne du processus ne s'ouvre pas.

Même raison d'être que `MW_CAPTURE=wgc` (§17) : un chemin de secours jamais
parcouru pourrit. Au banc de l'Arc (§8n.11 du banc), chaque panne finit en
D3D11 sur une keyframe, la seule hors de la première, avec la panne et la
chaîne choisie au journal. Le flux ne compte aucune erreur de décodage. Le
trou sans image dure 0,6 s : la duplication rouverte, puis l'encodeur D3D11
ouvert. Il dure 3,6 s pour `timeout`, dont les 3 s qui séparent un GPU perdu
d'un GPU occupé. `open` donne D3D11 dès le départ, raison à l'appui. Le banc
écrit la chaîne de chaque image dans son CSV, et résume la bascule.

**Le retour à la duplication (C8.3 bis).** Un worker qui n'est pas SYSTEM
passe sur WGC, donc en D3D11, quand la duplication est refusée derrière un
écran verrouillé. Il y restait pour la fin de la session (G2). Il revient
maintenant à DDA dès que le bureau de l'utilisateur est de retour, et à D3D12
avec (§17.1). Au banc de l'Arc, avec `MW_DDA_REFUSE=3+4` : la duplication est
perdue à 3 s, WGC prend le relais en D3D11 (0,7 s sans image), puis D3D12
revient 0,4 s après la fin du refus (0,5 s sans image). Chaque bascule se fait
sur une keyframe, sans erreur de décodage (§8n.12 du banc).

**Le pilote, et les pilotes exclus (C8.3).** La sonde lit la version du
pilote de chaque GPU. Elle utilise `IDXGIAdapter::CheckInterfaceSupport` sur
`IDXGIDevice` : depuis WDDM 2.3, les parts D3D9, D3D11 et D3D12 d'un paquet
de pilote partagent un même numéro. La version s'affiche à trois endroits :
- la ligne de session du journal (« on Intel(R) Arc(TM) A380 Graphics
  (driver 32.0.101.7088) ») ;
- `/api/native/status` (`gpu_driver`) ;
- le JSON de la sonde en session console.

Une liste par vendeur et par plage de versions (`d3d12DriverExclusions`, dans
`VideoPipeline.h`) tient la route D3D12 à l'écart d'un pilote fautif. Le choix
de chaîne le refuse alors comme les autres cas, raison à l'appui, et
`video_pipeline_auto` lit `d3d11` avec `video_pipeline_excluded`. La liste est
vide : une ligne n'y entre qu'avec une panne que les gardes de la route
n'attrapent pas déjà (garde d'en-têtes, délai des fences, retour en D3D11), et
avec sa preuve.

**Les scénarios (C8.2).** Le banc capture un écran virtuel rendu par l'Arc
pendant que son mode et son HDR changent (§8n.13 du banc) :
- quatre modes, dont un en 4:3 ;
- le HDR activé puis coupé sous une session SDR (le bureau passe en FP16, la
  conversion D3D12 fait le tone mapping) ;
- le HDR coupé puis réactivé sous une session HDR (elle se reconstruit en SDR) ;
- deux sessions à la fois sur deux écrans de l'Arc, en deux processus comme
  deux workers.

Tout reste en D3D12 : chaque redémarrage de capture reconstruit la chaîne sur
une keyframe, et aucun flux n'a d'erreur de décodage. Le passage de la
duplication à WGC et retour est celui de C8.3 bis. Pendant 30 min sous RE9,
sur l'Arc : 42 053 images en D3D12, sans perte ni repli, et une mémoire plate
(221 Mo privés, 50 Mo de VRAM, du début à la fin).

**Ce que la phase 8 a trouvé : le HDR de l'UHD d'un N95.** Une session HDR en
D3D12 y perdait le périphérique dès sa première image (`DEVICE_HUNG`). Le
repli l'a rattrapée : D3D11, HDR compris, sans erreur. Mais le GPU se
réinitialisait à chaque session. La cause est `ClearRenderTargetView` sur un
plan de P010 : le pilote 32.0.101.7088 le prend mal, alors que le dessin dans
ce même plan passe. Le convertisseur D3D12 dessine donc son noir, par un
`PsFill` qui rend les mêmes octets partout (§8n.14 du banc). Le test
`color_convert12_gpu` rejoue ces étapes sur chaque vrai GPU, et plus seulement
sur WARP.

**Le test de Bruno (C8.4), sur l'édition dev installée.** Écran verrouillé
depuis le stream, puis déverrouillé en tapant le PIN dans le stream, et un
stream HDR sur l'écran virtuel : l'overlay reste sur « D3D12 VE (Intel) ». Le
premier essai a d'abord montré que le worker SYSTEM n'avait jamais servi, en
v0.3.1 non plus (§31.7, `f1e8e8e3`). Une fois corrigé, la duplication se
rouvre sur le bureau `Winlogon` et la chaîne D3D12 se reconstruit des deux
côtés de la bascule, sans passer par WGC ni D3D11 (§8n.15 du banc).

**Les deux dernières passes, après la fusion dans `main` (§8n.17 du banc).**
- Dans un vrai Chrome, la bascule D3D12 → D3D11 ne coupe pas le stream. Le
  décodeur du navigateur repart sur la keyframe de l'encodeur D3D11, 0,6 s
  après la panne, sans erreur ni demande de keyframe.
- 30 min sous RE9 dans la classe du worker installé (REALTIME) : 40 131 images
  en D3D12, sans perte ni repli, et une mémoire plate. De la présentation à
  l'image encodée, 6,7 ms en moyenne, contre 24,3 en classe HIGH.

**Concrètement, pour l'utilisateur** : si la carte graphique décroche en
plein stream (pilote qui plante ou se met à jour, GPU bloqué), l'image se fige
une demi-seconde, trois secondes et demie au pire, puis repart d'elle-même
par l'ancien chemin, sans rien à relancer : le navigateur suit sans erreur.
Une longue partie tient aussi : 30 minutes sous un vrai jeu, sans une image
perdue ni mémoire qui grimpe. Un PC verrouillé se déverrouille depuis le
stream, comme avec Parsec, ce que la v0.3.1 promettait sans le tenir. Et là où
le worker ne tourne pas en SYSTEM (banc, `--dev`), un écran verrouillé ne fait
plus perdre le chemin rapide jusqu'à la fin du stream.

### 32.11 Le scaler matériel d'Intel, mesuré et écarté (§9-15, 28/09/2026)

Sur un iGPU Intel saturé par un jeu, la conversion attend le jeu, l'encodeur
non : il tourne sur le moteur vidéo (§8n.8 du banc). D3D12 Video Process met
le scaler de ce moteur (SFC) à portée, et la sonde `queues` du labo l'a mesuré
sur le N95 (§8n.16 du banc).

Il ne tient pas sa promesse. Sa file attend le jeu comme la nôtre, environ
10 ms sous charge, même en `GLOBAL_REALTIME`, et sa latence n'est pas meilleure
que celle de nos shaders en bilinéaire (11,2 contre 10,9 ms). Au repos, il est
plus lent (4,0 contre 2,9 ms). Il rend quelques images au jeu, mais plus dès
que le pointeur est composé : la composition à deux flux repasse par le moteur
3D. Il ne sait pas le HDR, ni le pointeur en inversion. Sa réduction, en
gamma, tient entre notre bilinéaire et notre Lanczos-2.

Pas de route SFC, donc : la condition posée par Bruno (« seulement si la sonde
ne voit plus le jeu ») n'est pas remplie. La sonde reste au labo (`vp`,
`vp-pointer`, `--picture`, `-Set vp` dans la campagne) pour un autre GPU ou un
autre pilote.

**Concrètement, pour l'utilisateur** : rien ne change. Sur un PC portable à
puce Intel où un jeu occupe toute la carte graphique, l'image du stream reste
préparée par le même chemin, parce que le circuit spécialisé d'Intel, mesuré,
fait la queue derrière le jeu tout autant et n'irait pas plus vite.

### 32.12 G4 : NVENC et AMF en D3D12 ne battent pas leur chemin D3D11 (28/09/2026)

La phase 7 a donné aux SDK des fabricants une entrée D3D12. NVENC prend une
image D3D12 depuis `NvencEncoder12`, AMF depuis `AmfEncoder12`. Leur
configuration est partagée avec le chemin D3D11 (`NvencConfig`,
`AmfConfig`), et une empreinte prouve qu'elle est identique d'un chemin à
l'autre. G4 les a mesurés face à D3D11, sous RE9 et au repos (§8n.18 du banc).

Aucune ne passe : NVENC en D3D12 coûte 0,2 à 0,3 ms et 16 à 24 % au p99, AMF
0,7 à 1,2 ms et 4 ms au p99. D3D11 ne perdait déjà rien sur ces GPU (G2).
Leur interface D3D12 ajoute des fences, une attente CPU (NVENC), une file pont
et une remise d'état (AMF).

Les lignes NVIDIA et AMD de la table restent donc D3D11. Les routes restent
joignables par le réglage et par `enc12=` au banc. Sur l'iGPU AMD, AMF en
D3D12 bat VE de 5 à 7 ms : c'est lui qu'il faudrait si la route D3D12 devait
un jour servir sur AMD (C12.1, à Bruno).

**Concrètement, pour l'utilisateur** : rien ne change sur un PC à carte
NVIDIA ou AMD. Le stream reste sur le chemin qui s'est montré le plus rapide
au banc, sous un jeu comme sur le bureau.

### 32.13 Le H.264 par D3D12 Video Encode (C9.1, 28/09/2026)

VE codait le HEVC seul. Il code maintenant le H.264, dans la forme que VA-API
envoie depuis toujours : profil High, une image gardée, fenêtre glissante,
`frame_num` sur 16 bits, POC de type 2. Le pilote écrit les tranches ; nous
écrivons SPS et PPS, placés devant l'IDR comme les en-têtes HEVC.

- **Négociation** (`H264EncodeNegotiation`). On prend CABAC et la
  transformée 8×8 là où le pilote les code : l'iGPU AMD n'a pas la 8×8, et le
  PPS le dit. Un pilote qui ne filtre pas tous les bords n'a pas d'encodeur :
  le PPS le promet. Pour le reste, l'échelle de régulation et la règle
  d'intra-refresh sont celles du HEVC, et le niveau vient du pilote, 5.1 à
  défaut.
- **Planification.** `HevcDpb`, à une image de capacité : la précédente,
  toujours utilisée. Une perte coûte donc une keyframe, comme en VA-API.
  Invalider une référence demanderait des opérations de gestion mémoire dans
  chaque tranche, que D3D12 laisse à l'appelant. C'est un pas pour plus tard,
  si une porte le demande.
- **Garde** (`H264SliceParser`). Les tranches des six premières images sont
  relues avec nos SPS et PPS. La garde vérifie le type d'image, `frame_num`,
  une référence gardée par la fenêtre glissante et une seule référence
  prédite. Un pilote qui écrit autre chose renvoie la session en D3D11 avant
  qu'un client voie du bruit.
- **Table.** La ligne D3D12 d'Intel a été mesurée en HEVC seulement
  (`autoD3d12Codec`). En Auto, le H.264 d'Intel reste donc en D3D11 ; le
  réglage et la clé de banc atteignent la nouvelle route.

Sur les trois GPU de DualRTX, les flux se décodent sans erreur, et chaque
image est bien la sienne (§8n.19 du banc).

**Concrètement, pour l'utilisateur** : rien ne change par défaut. Un
utilisateur qui choisit « D3D12 » dans l'admin et dont le navigateur ne lit
que le H.264 garde maintenant la route D3D12. Avant, il retombait en D3D11.

### 32.14 L'AV1 par D3D12 Video Encode (C9.2, 28/09/2026)

En AV1, D3D12 laisse au pilote la tuile et à l'application tout le reste.
Après l'image, les métadonnées résolues disent où est la tuile et quelles
valeurs d'en-tête le pilote a choisies : quantificateur, filtre de boucle,
CDEF, segmentation. `Av1Obu.h` écrit alors, d'après la spécification, le
délimiteur temporel, l'en-tête de séquence (devant chaque image clé) et
l'en-tête de `OBU_FRAME`. Le reste :
- **Négociation** (`Av1EncodeNegotiation`). On prend les fonctions que le
  pilote exige, plus CDEF et les order hints quand il les a. Un outil exigé
  que nos en-têtes ne savent pas dire (outils d'écran, super-résolution,
  copie intra, carte de segments fournie) veut dire pas d'encodeur. Une
  seule tuile : jusqu'à 4096 de large et 4096×2304 de surface. Le niveau
  vient de la taille et de la cadence, pas du pilote : l'Arc propose 6.0
  pour du 1080p60, et un décodeur peut refuser un niveau qu'il n'atteint
  pas.
- **Références.** Chaque image rafraîchit les huit emplacements, qui
  gardent donc tous l'image précédente. C'est `HevcDpb` à une image, comme
  en H.264 : une perte coûte une image clé.
- **Régulation.** Celle du pilote seulement, sur l'échelle qindex de l'AV1
  (0 à 255). Notre régulation parle le QP du HEVC et du H.264.
- **Ce que les pilotes ne disent pas.** Une fonction exigée doit aussi être
  allumée sur chaque image (la segmentation automatique de la RTX). L'Arc
  écrit son AV1 au début du tampon, quel que soit le décalage de début de
  trame. La tuile est donc prise là, puis déplacée derrière les en-têtes, en
  quelques microsecondes.
- **Table.** Le réglage et la clé de banc atteignent la route ; en Auto,
  rien ne bouge.

Sur la RTX et l'Arc, dav1d décode sans erreur, et chaque image est bien la
sienne (§8n.20 du banc). L'iGPU AMD n'a pas d'AV1 en D3D12.

**Concrètement, pour l'utilisateur** : rien ne change par défaut. Un
utilisateur qui choisit « D3D12 » dans l'admin et dont le navigateur demande
l'AV1 garde maintenant la route D3D12 sur une carte NVIDIA ou Intel Arc.
Avant, il retombait en D3D11.

### 32.15 G3 : la RTX en témoin du contrôle de débit maison (28/09/2026)

Le contrôle de débit maison (`QpRateController`) a été réglé sur l'Arc, qui ne
change pas de débit en cours de séquence. La RTX le sait, et fait donc tourner
ce contrôle en témoin, sans rien y changer (`rc12=qp`, §8n.21 du banc). Il
tient G3 sur ce second fabricant :
- 7 fenêtres de 2 s sur 9 dans ±10 % de la cible, et les ratés sont sous la
  cible ;
- un p95 de taille d'image de 1,23 à 1,96 budget ;
- des marches de débit suivies en 3 images ;
- un écran fixe amené à QP 18 ;
- des pertes réparées sans image clé.

Il fait mieux que le débit du pilote NVIDIA par la même voie : le pilote
reste sous la cible sur le clip et met jusqu'à 27 images à suivre une marche
montante. Le pilote code le QP demandé sur chaque image, comme celui de l'Arc.

Le témoin ne change pas la table. Sur la RTX au repos, VE encode une image en
environ 8 ms, NVENC par D3D11 en 2 ms, et NVENC tient mieux le budget que les
deux.
Le résultat vaut pour plus tard : si VE devait porter un GPU qui ne
reconfigure pas son débit, ou la chaîne Vulkan de Linux (§32.7), le même
contrôle s'y branche tel quel.

**Concrètement, pour l'utilisateur** : rien ne change sur un PC NVIDIA. Ce
banc confirme que la régulation qui fait tenir son débit aux PC Intel n'est
pas un réglage propre à une carte.

### 32.16 G3 : le profil « Internet » sur un vrai stream (28/09/2026)

Un Chrome sous Linux a reçu l'écran de l'Arc à travers un lien dégradé :
40 ms d'aller-retour, 0,3 puis 2 % de pertes, une marche de 30 à 8 Mb/s et
retour (§8n.22 du banc). Le gouverneur, le relais et les réparations
jouaient tous.

**La chaîne D3D12 tient.** Notre contrôle de débit suit le gouverneur, de
20 à 4 Mb/s en 4 s, puis vers le haut quand les pertes cessent. L'hôte reste
à 4 ms dans toutes les phases, et les pertes se réparent par un delta. Sur le
même profil, la chaîne D3D11 d'avant ne tient pas 60 i/s sur l'Arc, avec
23 ms d'hôte, et gèle deux fois plus.

**Le transport, lui, plafonne.** Le canal de données lit chaque perte comme
une congestion. À 0,3 % de pertes et 40 ms d'aller-retour, il ne porte
qu'environ 5 Mb/s, quelle que soit la capacité du lien. Sa file monte à
0,5-0,8 s pendant les pertes, et leur arrivée coûte un gel de 2,4 s. C'est
vrai pour les deux chaînes : aucun réglage de l'encodeur n'y touche. C'est la
mesure qui manquait au chantier du transport sous pertes (plafond du canal de
données, correction d'erreurs), prévu après celui-ci.

**Concrètement, pour l'utilisateur** : sur un PC Intel, par Internet, le
stream D3D12 reste fluide et réactif là où l'ancien chemin perdait des images.
Sur une connexion qui perd des paquets, l'image reste plus sobre (vers
5 Mb/s) et peut geler un instant quand les pertes commencent. C'est le
prochain chantier, côté transport.

### 32.17 Deux images en vol : mesuré, gardé en clé de banc (phase 10, 29/09/2026)

La chaîne D3D12 encode une image à la fois : le fil de capture convertit une
image, attend son bitstream, puis revient acquérir la présentation suivante.
Là où la conversion et l'encodage dépassent ensemble l'intervalle d'image,
des présentations sont perdues.

`pipelined=1` (clé de banc, `ee2c326e`) change cette forme :
- L'encodage et la remise à l'envoi passent sur un fil à eux, à la classe
  MMCSS du fil de capture.
- Le convertisseur écrit deux sorties à tour de rôle. La capture convertit
  l'image suivante dans celle que l'encodeur ne lit pas.
- Une image convertie qui attend encore quand une plus récente est prête est
  jetée : au plus deux images en vol, jamais de file.
- Ce qui suit une remise à l'envoi (modèle du lien, gardes d'encodage et de
  rééchantillonnage, compte de la cadence) reste sur le fil de capture, à
  partir d'un relevé laissé par le fil d'encodage.
- Un débit posé pendant un encodage part avec l'image suivante. Tout autre
  usage de l'encodeur (renvoi, reconstruction, redémarrage) attend d'abord
  qu'il ne tienne plus rien.
- Un échec sur ce fil prend le même chemin de retour vers D3D11 qu'un échec
  sur le fil de capture.
- D3D12 Video Encode seulement : les SDK des fabricants enregistrent une seule
  entrée.

**Mesuré** (banc §8n.23, classe REALTIME, A/B alternés) :
- Là où l'encodage dépasse l'intervalle et où la conversion tourne sur un
  autre moteur, le parallélisme paie. C'est l'iGPU AMD en VE à 120 i/s :
  94,5 images/s au lieu de 79,4, et l'hôte plus court (−0,6 ms en moyenne,
  −2,6 ms au p99).
- Là où l'encodage tient dans l'intervalle, rien ne change : RTX et Arc au
  repos, Arc sous RE9.
- Sur un iGPU saturé, le N95 à 120 Hz, les conversions faites pendant
  l'encodage disputent le GPU à l'encodeur : même cadence, p99 de l'hôte
  +52 ms. À 60 Hz, le N95 gagne un peu (p99 38,9 → 25,6 ms).

**Décision** : le défaut ne change pas (§9-26 du plan). Le seul cas qui gagne
nettement, l'iGPU AMD en VE, n'est pas la route d'AMD (AMF en D3D11, §32.12),
et Intel, dont c'est la chaîne par défaut, n'y gagne rien de sûr. La clé
reste pour le plan « ultra-low latency », qui la reprendra si l'encodage d'une
image y redevient plus long que l'intervalle.

**Concrètement, pour l'utilisateur** : rien ne change. La chaîne sait
désormais encoder une image pendant qu'elle convertit la suivante. Le banc
dit que ce n'est utile que sur un couple GPU × cadence que le produit ne
prend pas aujourd'hui.

### 32.18 Phase 11 : la télémétrie, l'encodeur gardé, et trois études (29/09/2026)

**Les shaders SM 6 en 16 bits (C11.1), mesurés avant d'être écrits.** Le but
était un Lanczos-2 abordable sur le N95, sous le garde de rééchantillonnage
(1,5 ms). La sonde `mw-d3d12-lab scale` (banc §8n.25) a d'abord cherché où
passe le temps.
- Sur Intel, la passe est liée à la mémoire. Précalculer les poids ou décoder
  le sRGB une seule fois ne gagne rien, voire perd.
- Le simple fetch bilinéaire prend déjà 1,46 ms sur le N95, le budget entier.
- Sur le petit iGPU AMD, lié au calcul, les mêmes réécritures font −57 %, mais
  2,5 ms restent au-dessus du budget.
- `min16float` ne gagne rien, nulle part.

**Pas de SM 6 ni de DXC dans le build** : aucune variante ne ferait changer
un GPU de côté du garde.

**Le temps GPU de l'encodeur (C11.2, `de0184b2`).** Avec `gputiming=1`, deux
horodatages encadrent chaque soumission sur la file d'encodage. Leur somme
remplit `gpu_encode_us`, re-encodages compris. Sur la RTX, VE passe 7,6 ms
sur les 8,1 de l'étape d'encodage, et l'iGPU AMD 8,8 sur 13,1 : le reste est
l'attente de la conversion. L'Arc écrit ses horodatages avant que l'image
soit codée (20 µs pour 4 ms). Le moteur le voit sur les 30 premières images
et ne rapporte rien plutôt qu'un chiffre faux. Les cartes de QP et de SATD
demandent un Agility SDK plus récent que celui du build : hors périmètre.

**L'encodeur gardé à travers un redémarrage de capture (C11.4, `df5187ef`).**
Un changement de mode, un écran qui passe en HDR, un écran verrouillé :
chacun reconstruisait toute la chaîne D3D12, encodeur compris. Pourtant, le
flux codé reste le même, car la session garde la taille et la cadence
négociées quoi que fasse le bureau. `keep12=1` met l'encodeur de côté au
démontage, et le reprend si le flux (codec, taille, cadence, HDR,
intra-refresh) n'a pas changé. Sur l'écran virtuel rendu par l'Arc, le trou
avant l'image clé d'un redémarrage passe de 717 à 546 ms en médiane, sur
12 redémarrages par bras, sans aucune erreur de décodage (banc §8n.24).
**Par défaut depuis le 29/09** (§9-27 du plan, accepté par Bruno ; `keep12=0`
le refait). Le flux ne change pas, il repart toujours sur une image clé, et il
gagne 0,2 s par redémarrage sur la chaîne par défaut d'Intel. Un encodeur
qu'une panne laisse derrière elle n'est jamais gardé.

**ROI par carte de QP (C11.3, étude).** La sonde du 29/09 refait le constat
du 26/09 sur les pilotes du jour :
- la RTX applique les cartes delta et absolue (cases de 32 px) ;
- l'Arc annonce la carte delta, mais `EncodeFrame` la refuse ;
- l'iGPU AMD annonce la carte absolue (cases de 64 px), et `EncodeFrame` la
  refuse aussi.

Un ROI (pointeur net, texte net) n'a donc de GPU où servir dans la chaîne
D3D12 que la RTX, dont le stream passe par NVENC en D3D11, qui a sa propre
carte. **Pas de ROI dans la chaîne D3D12** tant qu'un pilote Intel ne
l'applique pas. Un ROI sur NVENC (le pointeur composé, par exemple) serait
une ligne à part de la liste du plan natif, à mesurer pour lui-même.

**L'envoi par tranches (C11.5, étude).** Envoyer une image par tranches
(slices), au fur et à mesure qu'elles sont codées, fait partir la première
plus tôt :
- NVENC le permet par sa lecture partielle (`enableSubFrameWrite` et
  `reportSliceOffsets`, sur la route du fabricant).
- D3D12 Video Encode ne le permet qu'avec les notifications de sous-régions
  d'un Agility SDK en préversion, hors périmètre du plan.

Le gain est borné par le plus petit de deux temps : le temps d'encodage qui
reste après la première tranche, et le temps d'envoi de l'image.
- Côté navigateur, WebCodecs décode une unité d'accès entière : le décodeur
  attend le dernier octet de toute façon.
- Sur un réseau local à 1 Gbit/s, une image de 40 Ko part en 0,3 ms : c'est
  le gain maximal.
- Par Internet à 20 Mbit/s, elle part en 16 ms : l'encodage de NVENC (2 ms)
  s'y recouvre presque entier, soit 1,5 ms au mieux.

Il faut payer ces 1,5 ms de 2 à 5 % de débit (en-têtes de tranche, prédiction
coupée aux frontières), et d'une réparation par invalidation plus complexe.
**Pas fait.** À rouvrir dans le plan « ultra-low latency », si son codec
décode par blocs indépendants.

**Concrètement, pour l'utilisateur** : un changement de résolution ou le
retour d'un écran verrouillé rend l'image environ 0,2 s plus vite sur un PC
Intel (par défaut depuis le 29/09). Le reste de la phase outille les bancs, et dit
pourquoi deux idées (ROI, envoi par tranches) attendront.

### 32.19 Clôture du plan : la chaîne par GPU (29/09/2026)

Le plan `pipeline-video-d3d12-v2` s'achève sur une chaîne par type de GPU,
chacune choisie au banc puis acceptée par Bruno. `Auto` prend la chaîne de la
table ; le réglage `native_video_pipeline` (Avancé, dans l'admin) en force une
autre. Un refus ou une panne ramène toujours à la chaîne d'avant, sans couper
le stream.

| GPU | `Auto` aujourd'hui | Pourquoi | Ce que le réglage offre en plus |
|---|---|---|---|
| Intel (Arc, iGPU Xe) | **D3D12** : conversion D3D12, D3D12 Video Encode en HEVC, contrôle de débit maison | Sous un jeu, l'hôte deux fois plus court sur l'Arc (G2) ; le débit tenu (G3) ; le clic → photon de Bruno (C5.7) | H.264 et AV1 par D3D12 Video Encode (en `Auto`, ils restent sur oneVPL) |
| NVIDIA | D3D11 (NVENC) | NVENC y encode en 2 ms. VE en prend 5 à 8, et NVENC en entrée D3D12 ne fait pas mieux (G2, G4) | NVENC en entrée D3D12 |
| AMD | D3D11 (AMF) | VE ajoute 7 à 8 ms, et AMF en entrée D3D12 ne fait pas mieux (G2, G4) | D3D12 Video Encode |
| AMD sous Linux | **Vulkan compute → VA-API** (route scindée), par le scanout comme par le portail (§32.21) | Sous un jeu, 8 ms au lieu de 15 à 38 par GL (§8o.5, §8o.8) ; par le portail, 16 ms au lieu de 31 (§8o.11) | La chaîne Vulkan Video, prise sur la preuve au pixel (`vulkan`), ou GL (`vaapi`) |
| Intel et NVIDIA sous Linux | GL → VA-API ; OpenH264 sur NVIDIA | Pas encore de carte au banc | La chaîne Vulkan Video, écrite d'après la spécification et les capacités lues |

**Restaient à Bruno** (plan, §9), et ses réponses du 29/09 (§32.20) :
- l'image jetée au calage du lien, nommée à l'encodeur (§9-25) : oui, derrière
  un interrupteur d'abord (`namedrops`), puis par défaut pour NVENC et AMF en
  D3D11 après les bancs, et pour NVENC en entrée D3D12 (§32.20) ;
- `pipelined=1` (§9-26, §32.17) : par défaut sur un GPU Intel à mémoire propre
  (§32.20) ;
- `keep12=1` par défaut (§9-27, §32.18) : fait ;
- G5 sous Counter-Strike 2 ; la file d'encodage HIGH sous le noyau 7.0 reste
  refusée (banc §8o.10) ;
- les tests manuels : le pompage de G3, **fait** (Bruno, 29/09, en 5G :
  aucun pompage sur du texte ni sous RE9, à 10 et 5 Mb/s ; banc §8n.22) ;
  l'invite UAC, **faite** (29/09, cliquée depuis le client, Bruno ayant remis
  l'UAC par défaut le temps du test ; §31.7) ; Ctrl+Alt+Suppr (§9-28) : fait,
  par le service lanceur et la stratégie de l'installeur (§31.7), et les
  gestes de Bruno depuis son iPhone, **faits** (29/09 : Ctrl, Alt puis `Del`,
  puis un stream relancé sur le poste verrouillé et son code) ;
- la séance du 780M sous Windows (C10.2) : sans objet, la décision sur
  `pipelined` ne touche pas AMD.

**Concrètement, pour l'utilisateur** : sur un PC Intel, le stream part plus
vite et reste net sous un jeu, avec un débit qui suit la connexion. Sur
NVIDIA et AMD sous Windows, rien ne change : leur chemin était déjà le
meilleur, et le banc l'a vérifié. Sous Linux avec un GPU AMD, l'image reste
fluide quand un jeu sature le GPU. Partout, l'admin montre la chaîne qui
tourne, et permet d'en choisir une autre.

### 32.20 Les réponses du 29/09 : keep12, l'image jetée nommée, deux images en vol à 244 Hz

**`keep12` par défaut** (§9-27, accepté). Le démontage d'une capture met
D3D12 Video Encode de côté, sauf avec `keep12=0`, ou quand une panne l'a
laissé derrière elle. Vérifié sans clé sur l'écran virtuel de l'Arc (banc
§8n.26) : l'encodeur est gardé à 6 reconstructions sur 7, avec 471 ms de trou
médian avant l'image clé, et 0 erreur de décodage.

**L'image jetée au calage du lien, nommée à l'encodeur** (§9-25, accepté
derrière un interrupteur). La clé `namedrops`, lue par le relais, est décrite
au §9.10.2. Après les bancs sur lien bridé (banc §8n.27 et §8n.28), Bruno l'a
mise par défaut pour NVENC et AMF en D3D11 : les images abîmées y fondent, et
les gels ne bougent pas. Jamais pour oneVPL, dont le HEVC se bloquait sous les
réparations (il ne répare plus sous intra-refresh, §21.6b) ; D3D12 Video Encode,
neutre, reste sans. Un dernier banc (§8n.29) l'a étendue à NVENC en entrée
D3D12, qui gagne autant ; AMF en entrée D3D12, neutre, reste sans.

**Deux images en vol à haute fréquence** (§9-26, pas tranché). Bruno a demandé
deux vérifications : un gros GPU à très haute fréquence y gagne-t-il, et
l'encodeur d'Intel partage-t-il l'unité de calcul de la conversion ? Banc
§8n.26 :
- **L'encodeur d'Intel ne touche pas aux shaders** : moteur 3D à 0 % en
  encodage seul sur le N95. Aucune priorité de file ne peut donc départager la
  conversion et l'encodage : ils tournent sur deux moteurs.
- Ce qui ralentit le N95 à 120 Hz, c'est la mémoire. Elle est à un canal,
  commune avec le CPU, et un encodage y dure ~40 % de plus quand la conversion
  tourne en même temps.
- **À 244 Hz, l'Arc y gagne** 2 à 4,5 % d'images, avec un hôte presque
  inchangé : il a sa propre mémoire.
- **La RTX n'en a pas besoin** : NVENC en D3D11, sa chaîne par défaut, suit
  240 i/s jusqu'en 1440p avec 1,6 à 2,8 ms d'hôte. Seul le 4K à 244 Hz le met
  en limite (226 i/s, 4,24 ms par image pour 4,1 d'intervalle). Il faudrait
  alors écrire les deux images en vol pour NVENC, qui n'enregistre qu'une
  entrée.

**Décision de Bruno (29/09), sur le tableau avant/après : par défaut sur un
GPU Intel à mémoire propre** (`9814c606`). Seules les lignes de l'Arc changent :
gain à haute fréquence, rien de mesurable ailleurs.
- La règle `pipelinedByDefault` (vendeur Intel, pas de mémoire unifiée) entre
  dans la table des vendeurs, avec ses tests. `D3d12Device` lit le vendeur et
  le drapeau `UMA` de `D3D12_FEATURE_ARCHITECTURE` ; WARP se déclare en mémoire
  unifiée, vérifié.
- La ligne « D3D12 chain on … » du journal dit « memory of its own » ou « the
  CPU's memory ».
- Les iGPU (N95, Xe, AMD) restent à une image à la fois.
- `pipelined=0` force une image à la fois sur l'Arc, `pipelined=1` deux images
  partout où D3D12 Video Encode tourne.

**Concrètement, pour l'utilisateur** : sur un PC Intel, un changement de
résolution rend l'image ~0,2 s plus vite. Sur une carte Intel Arc à très haute
fréquence (240 Hz), le stream tient quelques pour cent d'images de plus.
L'image jetée nommée attend son banc.

### 32.21 Linux : le portail en DMA-BUF, et la route scindée dessus (C13.3 bis, 29/09/2026)

Plan C13.3 bis, « Go » de Bruno le 29/09 ; banc §8o.11.

**Ce que le portail donnait.** De la mémoire partagée, toujours. Un compositeur
ne donne un DMA-BUF qu'à un client qui annonce, format par format, les
modificateurs qu'il sait importer : c'est la négociation DMA-BUF de PipeWire.
`PortalCapture` n'annonçait rien, et GNOME 42 puis 46 choisissaient la mémoire
partagée. La session encodait donc sur le CPU (§19.15).

**Ce qui change.**
- **L'offre.** La session demande à EGL, sur le render node de la carte, les
  modificateurs qu'il importe pour les quatre formats d'un écran
  (`GlConvert::importableModifiers`, sans ceux qu'EGL ne lit qu'en texture
  externe). GL prend la relève de toutes les autres conversions : sa liste est
  celle qu'un tampon doit tenir. `PortalCapture::offerDmabuf` les annonce avant
  la mémoire partagée, marqués « à ne pas fixer » : le compositeur choisit
  celui qu'il sait allouer. GNOME 46 fixe `0x200000010401b04` sur le 780M.
- **Le GPU nommé.** La paire GPU recevait un render node vide sur cette route.
  Le portail le reçoit de la session (`setRenderNode`).
- **`dmabuf()` juste.** Il se lisait sur le premier tampon, arrivé après la
  fin de `start()` : la session choisissait sa paire sur une réponse encore
  fausse. `start()` attend maintenant ce premier tampon (une seconde au plus ;
  un écran fixe en envoie un au démarrage).
- **Le repli.** Si GL lui-même refuse un DMA-BUF du portail, aucune marche
  Vulkan ne restant à descendre, la session rouvre le portail sans offre, sur
  l'accord déjà donné (`leaveDmabuf`) : pas de dialogue, et le stream continue
  en mémoire partagée.
- **La table.** La ligne AMD (`autoLinuxConversion`) vaut pour le portail :
  Vulkan compute devant VA-API. Au banc, la conversion y passe de 0,84 à 0,65 ms
  au repos, et de 30,7 à 15,8 ms sous un jeu qui sature le 780M, avec 40 i/s
  captées au lieu de 28.

**Ce qui ne change pas.** Une capture par le scanout (le paquet, avec son
lanceur) ne passe pas par le portail. `vaapi` dans l'admin reprend GL devant
VA-API, portail compris. Le banc garde `convert=gl`, et `portaldmabuf=0`
demande la mémoire partagée seule.

**Concrètement, pour l'utilisateur** : avec l'AppImage sous GNOME, le stream
n'encode plus sur le processeur. Il passe par le GPU, en HEVC si le navigateur
le prend, et reste fluide sous un jeu (40 images par seconde captées au lieu de
28 sur un 780M saturé). Le premier stream demande toujours une fois
l'autorisation de partager l'écran ; les suivants, non.

### 32.22 Linux : la mémoire partagée du portail, par la conversion Vulkan (C13.10, 29/09/2026)

Plan C13.10, « Go » de Bruno le 29/09 ; banc §8o.12.

**Pourquoi.** Un compositeur qui ne donne pas de DMA-BUF donne de la mémoire
partagée. GL ne sait pas la lire : la session encodait sur le CPU, en H.264
seulement, et un cœur y passait.

**Ce que la conversion Vulkan fait d'une image en mémoire partagée**
(`KmsFrame::mapped`).
- Elle tente d'importer la mémoire là où elle est mappée
  (`VK_EXT_external_memory_host`, activé où le pilote l'a) : le GPU la
  copierait sans que le CPU touche un pixel.
- **amdgpu refuse** : le noyau n'importe que de la mémoire anonyme, et celle
  de PipeWire est un memfd. Le refus est retenu pour la session, et dit une
  fois au journal.
- Le repli : le CPU copie l'image dans un tampon visible du GPU, gardé d'une
  image à l'autre. Le GPU la copie ensuite dans une image, puis la convertit
  comme un DMA-BUF. Pas de barrière implicite ici : le compositeur a fini
  d'écrire avant de rendre le tampon.

**Qui la prend** (`LinuxRouteChoice.h`).
- La mémoire partagée passe par la conversion Vulkan là où la table convertit
  déjà en Vulkan (AMD) ou quand la clé `convert=vulkan` la demande. Elle mène
  à VA-API, ou à la chaîne Vulkan Video si celle-ci est prise.
- GL ne la lisant pas, tout ce qui mènerait à GL mène au CPU : la table
  d'Intel et de NVIDIA, VA-API choisi par son nom dans l'admin, une conversion
  Vulkan refusée. Le CPU reste la dernière marche, jamais GL.

**Au banc** (780M, §8o.12) : 5 % d'un cœur au lieu de 71, la même latence, du
HEVC au lieu du seul H.264, pour 8 % d'images en moins (le GPU est partagé
avec la copie du compositeur).

⚠️ **GNOME 46 n'enregistre aucune image d'une fenêtre en plein écran en
mémoire partagée** : 0 image en 10 s, avec l'ancienne paire CPU comme avec la
nouvelle (vu avec Chrome en kiosque). En DMA-BUF, le plein écran passe. Avant
C13.3 bis, une AppImage sous GNOME 46 ne streamait donc rien d'une fenêtre en
plein écran, un jeu y compris selon toute vraisemblance : l'offre DMA-BUF
(§32.21) le corrige aussi.

**Concrètement, pour l'utilisateur** : sur un bureau Linux dont le
compositeur ne donne que de la mémoire partagée, l'AppImage n'occupe plus un
cœur du processeur à encoder. Sur une carte AMD, l'image passe par le GPU, en
HEVC si le navigateur le prend.

### 32.23 Linux : le bourrage de RADV, retiré de la chaîne Vulkan Video (29/09/2026)

Trouvé en relisant, unité par unité, les flux du banc d'intra-refresh ; banc
§8o.13.

**Ce que RADV écrit.** En CBR, le pilote complète chaque image jusqu'à son
budget avec des unités de bourrage (*filler data*, type 38 en HEVC), placées
après les tranches. Sunshine en reçoit autant sur ce pilote, et le shim les
retire depuis `9ecc3abf`. Le chemin natif, lui, les envoyait :
- une page qui défile, à 16 Mbit/s : 23 à 37 % des octets ;
- un écran fixe : 122 Ko par image renvoyée, pour quelques centaines
  d'octets d'image. Le raffinement de l'image fixe « coûtait » 610 Ko en
  5 passes, et le lien en retenait 8 images (banc de C13.11).

**Le correctif.** L'encodeur retire le bourrage sur place, dans son tampon de
flux, avant que l'image parte (`stripHevcFiller`). La fonction vit à côté du
découpage Annex-B de `HevcSliceParser.h` : le module natif ne peut pas inclure
l'`AnnexBFiller.h` du shim. Le journal le dit à la première image, et donne le
total à l'arrêt.

**Ce qui ne change pas.**
- Les images décodées sont les mêmes. La preuve au pixel garde son verdict,
  sans nouvelle révision de l'encodeur.
- Le bourrage n'était jamais devant la tranche d'une IDR : nos en-têtes sont
  posés devant les tranches, lui vient après. Aucune image clé n'était donc
  refusée, comme celles de Sunshine sur le Mac ; il ne coûtait que du débit.
- Aucun des 75 flux Windows gardés des bancs D3D12 n'en contient (D3D12 Video
  Encode, oneVPL, NVENC et AMF). VA-API le coupe (`disable_bit_stuffing`).

**Concrètement, pour l'utilisateur** : avec la chaîne Vulkan Video, un bureau
immobile ne coûte presque plus rien sur le réseau, et une image qui bouge
passe avec un quart à un tiers d'octets en moins, pour la même image. Sur une
connexion lente, chaque image met d'autant moins de temps à passer.

### 32.24 Linux : l'intra-refresh de la chaîne Vulkan Video (C13.9, 29/09/2026)

Plan C13.9, « Go » de Bruno le 29/09 ; banc §8o.14.

**Pourquoi.** Le client qui traverse les pertes (`rideOutLoss`) demande un
intra-refresh : l'image se répare par balayages, sans attendre d'image clé. La
chaîne Vulkan Video n'en faisait pas. `/start` disait `intra_refresh: false`,
et le client retombait sur les images clés.

**Le code.**
- `VulkanDevice` active `VK_KHR_video_encode_intra_refresh` et sa
  fonctionnalité, là où le pilote les a, sur un périphérique ouvert pour
  encoder (`encodesIntraRefresh`).
- `VulkanHevcEncoder` crée la session avec son mode
  (`VkVideoEncodeSessionIntraRefreshCreateInfoKHR`) : les colonnes d'abord,
  parce qu'un défilement vertical reste dans sa bande, puis les rangées, puis
  les blocs.
- Chaque image d'un balayage porte le drapeau d'intra-refresh, la durée du
  balayage et son rang (`VkVideoEncodeIntraRefreshInfoKHR`). Sa référence dit
  combien de régions restent à rafraîchir (`VkVideoReferenceIntraRefreshInfoKHR`,
  la durée moins le rang).
- `encode/IntraRefreshSweep.h` tient le compte, sur la règle du moteur
  (`RateControl.h`) : deux secondes d'images par balayage (120 à 60 i/s),
  toutes les quatre périodes (480). La clé de banc `irdist=`, jusqu'ici à
  oneVPL seul, vaut pour les deux (`EncoderTuning::intraRefreshDist`).
- **Une réparation relance le balayage.** Pendant un balayage, seule l'image
  précédente peut être en partie propre. Une réparation qui prédit depuis une
  image plus ancienne (l'invalidation de référence de `HevcDpb`) repart donc
  au rang 0, sa référence comptée sale en entier. Les réparations restent
  permises : contrairement à oneVPL en HEVC (§21.6b), la preuve et le banc
  passent sans blocage.
- L'horizon annoncé au client (`intra_refresh_frames`) : l'écart plus un
  balayage, 600 images à 60 i/s. C'est ce que le chien de garde du ride-out
  attend avant de demander une image clé.

**Les refus**, dits au journal ; le stream continue alors en images clés à la
demande :
- un pilote sans l'extension ;
- aucun des trois modes ;
- aucune référence permise pendant un balayage ;
- des balayages de moins de 2 images ;
- une session refusée avec l'intra-refresh : elle est recréée sans.

**La preuve.** La révision 2 de l'encodeur rejoue les verdicts gardés. Son
témoin balaie par 4 images dos à dos, à travers les pertes et l'image clé de la
séquence : sur le 780M, 39,6 dB au pire par image (39,8 sans balayage).

**Au banc** (780M, §8o.14), le bourrage retiré (§32.23) :
- 0 erreur au décodage, perte pendant un balayage comprise ;
- l'encodage ne bouge pas (1,62 ms) ;
- sur une page qui défile, les balayages espacés ne coûtent rien de mesurable
  (le même débit, 0,2 de QP) ; dos à dos, 5 % d'octets ;
- sur une page fixe, un balayage coûte ~10 Ko par image. Espacés de quatre
  périodes, les balayages y coûtent 0,7 Mbit/s ; dos à dos, 4,1 Mbit/s ;
- réparer une perte par une image clé coûte moins d'octets mais adoucit
  l'image : QP 27,5 sur la demi-seconde qui suit, contre 19 à 20.

**VA-API, vu en passant.** Sous Mesa 26.2.3, VA-API balaie aussi sur le 780M
(`VAConfigAttribEncIntraRefresh`), en H.264 comme en HEVC ; sous Mesa 23.2, il
n'en avait pas (§19). Sa vague est continue, sans l'écart de quatre périodes.

**Concrètement, pour l'utilisateur** : sous Linux avec une carte AMD et la
chaîne Vulkan Video, le mode qui traverse les pertes marche comme sous
Windows. Une image abîmée se répare d'elle-même, sans le gel de l'image clé
attendue.

### 32.25 Linux : l'écran fixe — la carte de QP écartée (C13.11), et VA-API sous Mesa 26 (29/09/2026)

Plan C13.11, « Go » de Bruno le 29/09 ; banc §8o.15.

**La carte de QP, mesurée et écartée.** L'idée : une carte de deltas de QP
(`VK_KHR_video_encode_quantization_map`, cases de 64 px sur RADV) pour affiner
plus vite l'écran fixe de la chaîne Vulkan Video. Le bourrage retiré (§32.23),
le banc montre qu'il n'y a rien à affiner : la chaîne est déjà à QP 18 quand
l'image s'arrête, et ses rafales de raffinement ne coûtent plus rien. Le
« raffinement » de 610 Ko vu avant n'était que du bourrage. Pas de code.

**VA-API ne se pose plus sur un bureau presque immobile.** Sur une page de
texte où seul un carré de 48 px tourne, VA-API prend 32 Ko par image après 4 s,
jusqu'au bout. Le débit est de 14,3 Mbit/s sur 20 par la route par défaut
(Vulkan compute → VA-API) comme par GL, et de 20 avec sa vague d'intra-refresh.
La chaîne Vulkan Video reste à 0,8 Mbit/s sur la même page. Sous Mesa 23.2, le
22/09, VA-API y retombait sous 1 Ko par image. Ce sont de vraies données, pas
du bourrage.

**Ouvert, à Bruno** : la route par défaut sur AMD est touchée. En chercher la
cause (le contrôle de débit du pilote, ou nos références), un plancher de QP
pour VA-API, ou la chaîne Vulkan Video par défaut sur AMD. Rien n'est changé
d'ici là. **Tranché le 05/10** : la chaîne Vulkan Video par défaut sur AMD, la
cause cherchée à côté (§32.28).

**Concrètement, pour l'utilisateur** : rien ne change pour l'instant. Sous
Linux avec une carte AMD et un Mesa récent, un bureau presque immobile peut
consommer presque tout le débit réglé. Ce n'est gênant que sur une connexion
limitée, et c'est à régler.

### 32.26 Linux : un bureau X11 sur deux GPU, et le portail du paquet (30/09/2026)

Six défauts vus le 30/09 sur l'UM790Pro, quand le pilote NVIDIA de la GTX 1050
y a fait passer GDM en X11 (deux écrans sur deux GPU). « Go » de Bruno pour
chacun ; G5 au banc §8o.16.

**Une fenêtre de la racine (5, `6465fbff`).** Sous X11, tous les écrans d'une
carte lisent un seul tampon, la racine de X : l'écran de 1920×1080 est une
fenêtre d'un tampon de 4480×1440. `KmsCapture` y voyait un changement de mode
sans fin : 0 image. Il lit désormais le rectangle `SRC_X/Y/W/H` du plan
primaire (`ScanoutWindow.h`), importe le tampon entier et ne convertit que ce
rectangle (GL, Vulkan, CPU).

**Une image recopiée (6, `f758982c`).** Un jeu en synchro coupée est recopié
par X dans le même tampon : le tampon ne change pas, et la capture, réveillée
par un nouveau tampon, livrait 1 image en 5 s. `X11Damage` écoute les
« damage » de la racine (80 par seconde sous le jeu, aucun sur un bureau
immobile) ; `KmsCapture` relit le même tampon au vblank quand il y en a eu.

**Un écran que KMS ne montre pas (3, `1750eab9`).** Le pilote X de NVIDIA règle
ses modes sans KMS : le M27Q de la GTX y est « 0×0 · 0 Hz (off) », était pourtant
proposé, et son lancement échouait. Une sortie sans CRTC n'est plus proposée ;
la sonde dit pourquoi, et la session compte les écrans par la même règle.

**Le pointeur sur la disposition de X (2, `4ae134c7`).** X étale un pointeur
absolu sur toute sa racine, écrans de tous les GPU compris ; la session ne
connaissait que la carte capturée, et un puits PRIME lit son tampon depuis 0,0.
Le centre de l'image de l'AMD visait donc 2241,720 au lieu de 3520,540.
`X11Layout` lit RandR par `dlopen` : le rectangle de chaque sortie, son EDID, le
connecteur KMS que publie le pilote, et la taille de la racine. L'écran capturé
y est reconnu par son EDID, puis son connecteur, puis sa place, jamais par son
nom : chaque pilote X nomme à sa façon (HDMI-A-1 du noyau = HDMI-A-0
d'amdgpu). Mesuré de bout en bout avec le paquet de la CI : sur sept points
visés depuis un client, le pointeur tombe à un pixel près (banc §8o.17).

**Le portail du paquet (1, `470341ef`).** xdg-desktop-portal 1.18 identifie
l'appelant par `/proc/<pid>/root`, que le noyau ne lui ouvre que si ses
capacités couvrent celles de l'appelant. Le worker du paquet en porte
(`CAP_SYS_ADMIN`, `CAP_SYS_NICE`), le portail aucune : toute route portail du
`.deb` et du `.rpm` était refusée. Un processus qui porte des capacités demande
désormais par un auxiliaire : le même binaire, relancé sans aucune capacité
(`no_new_privs`), qui tient la session du portail et rend le descripteur
PipeWire par une paire de sockets. La capture garde ses files en priorité haute.
Sous X11, l'écran virtuel n'est plus proposé : seul le compositeur Wayland de
GNOME sait en créer un (01/10/2026 : celui de KDE Plasma 6 aussi, §35).
- Vérifié par la suite avec capacités, sous X11. Pas encore par le paquet sous
  Wayland : l'UM790Pro n'y revient pas tant que le pilote NVIDIA y est (vu le
  01/10/2026, l'UM790Pro repassé en Wayland : banc §8s.11).
- Le thème GNOME de Qt lit ses réglages d'apparence par le même portail, au
  démarrage de l'app et du worker. Il est refusé de la même façon, ce qu'écrit
  une ligne « dbus reply error ». Sans effet sur le stream.

**Les tests sur la mémoire partagée (4, `9e30b407`).** Sous X11, le portail de
GNOME donne de la mémoire partagée, que GL ne lit pas : quand la conversion
Vulkan lâche, c'est le CPU qui prend la suite, et un client HEVC seul est
refusé. La suite supposait le DMA-BUF de Wayland (12 échecs) ; elle suit
maintenant la raison de la route. Sans capacités, 6191/6191 ; avec, 6376/6376.

**Concrètement, pour l'utilisateur** : sous Linux avec une carte NVIDIA à côté
d'une autre, le bureau passe en X11, et il devient streamable : chaque écran de
la carte qui le montre se capte, jeux en synchro coupée compris. La souris va
là où on vise sur un bureau à plusieurs écrans. Un écran que le pilote NVIDIA
tient hors de portée n'est plus proposé pour rien. Et le paquet installé n'est
plus refusé par le portail, dont dépend l'écran virtuel sous Wayland.

### 32.27 Linux : l'AV1 par Vulkan Video (C13.12, 04-05/10/2026)

Plan C13.12 (`max`), « Go » de Bruno le 29/09, repris le 04/10 au soir ; banc
§8o.18. Commits `6045301d` (l'encodeur, la preuve) et `72471fbf` (l'offre, la
route, la session).

**Pourquoi.** Sous Linux, l'hôte natif ne proposait jamais l'AV1 : le VA-API
de Mesa liste le profil et ne décrit aucun encodeur (§19.13). RADV 26 en
expose un par Vulkan Video, sur le même périphérique que la conversion.

**L'encodeur** (`encode/linux/VulkanAv1Encoder`), le jumeau de
`VulkanHevcEncoder` :
- l'entrée NV12 écrite en place par la conversion, une soumission qui attend
  la conversion sur le GPU, le CPU qui n'attend que l'encodage ;
- l'en-tête de séquence est le nôtre en structure StdVideo, ses octets ceux du
  pilote (`vkGetEncodedVideoSessionParametersKHR`), relus par
  `av1::parseSequenceHeader` ;
- devant chaque image un délimiteur temporel, et l'en-tête de séquence devant
  une image clé, posés juste avant les octets du pilote : une image part d'un
  seul tenant ;
- les références : la tenue de `HevcDpb`, une image du pool pour un des huit
  emplacements de l'AV1. Une image rafraîchit l'emplacement de sa texture,
  jamais celui d'une image gardée, et ses sept noms de référence pointent
  l'image dont elle prédit. Une perte laisse périmés, chez le récepteur, les
  emplacements des images perdues ; rien ne les nomme plus avant qu'ils soient
  rafraîchis, donc la réparation prédit d'une image que les deux côtés ont ;
- les premiers en-têtes de trame sont relus (`av1::parseFrameHeader`, jusqu'à
  `delta_q_present`) : type, numéro d'ordre, taille affichée, emplacement
  rafraîchi, référence ;
- l'intra-refresh comme en HEVC (§32.24).

**⚠️ VBR, jamais CBR.** Le CBR de RADV demande au micrologiciel de bourrer
chaque image, et en AV1 le VCN 4.0.2 ne revient pas de la première : anneau
bloqué 22 s, réinitialisé par le noyau (8 fois sur 8). Le VBR, son pic au
débit visé, tient le même budget sans bourrage. Plancher q-index 90 (le QP 18
du moteur, la règle de D3D12 VE). Le quantificateur n'est pas rapporté : RADV
écrit `base_q_idx` 127 et règle le débit par des deltas par superbloc.

**⚠️ La taille : sur la grille du pilote, la même forme.** Le 780M code l'AV1
par blocs de 64×16. Un cadre allongé jusqu'à la grille dit l'image dans la
taille d'affichage de l'AV1, que Chrome ignore (WebCodecs, logiciel et
matériel : 1920×1088 affichés pour 1080p). L'encodeur ramène donc l'image sur
la grille en gardant sa forme exacte (`alignedToGrid`, à côté de
`alignedToBlocks` qu'utilise déjà le HEVC de VA-API) : 1080p devient
1792×1008, la conversion y met l'image à l'échelle (Lanczos-2), la session
annonce cette taille. 720p, 1440p et 4K sont déjà sur la grille.

**La preuve au pixel** (`proveVulkanAv1`, `vulkanAv1Verdict`) : la séquence
de la preuve HEVC, relue par dav1d, le décodeur qu'utilise Chrome quand son
GPU ne décode pas l'AV1. Ouvert à l'exécution (`libdav1d.so.7`, `.6` ou `.5`),
jamais lié ; ses en-têtes au build seulement (`libdav1d-dev`, ajouté à la CI ;
les paquets le recommandent). Les structures de la bibliothèque reçoivent une
place plus large que toute version : seuls leurs premiers champs sont lus,
stables depuis la 0.9 de la CI (Ubuntu 22.04). Verdict gardé dans le cache,
sous une clé à part (`av1|…`, la version de dav1d comprise).

**L'offre** (`platform::offerSessionCodecs`) : l'AV1 n'est ajouté aux codecs
d'un GPU que pour une session dont la clé de banc ou le réglage demande la
chaîne Vulkan Video, où le pilote Vulkan montre l'encodeur AV1 et où le build
porte dav1d. La table des vendeurs n'en demandait aucune : **le défaut ne
changeait pas** (jusqu'au 05/10 : AMD y prend la chaîne, §32.28). Windows et
macOS n'ajoutent rien.

**La route et la session** :
- `LinuxRouteChoice` prend l'AV1 comme le HEVC, derrière sa preuve ;
- refusé avant le stream (preuve, chaîne qui ne démarre pas), la session
  prend le codec suivant du client que le GPU encode sans lui
  (`ResolvedTarget::codecsWithoutOffer`), et refait le choix de route ;
- refusé en cours de stream, la session s'arrête en le disant : rien d'autre
  n'encode l'AV1 sous Linux, et un stream ne change pas de codec.

**Le client qui recadre (05/10, `2bdda3a8`, accord de Bruno).** Un navigateur
qui sait couper une image décodée le dit au `/start` (`crops_to_frame`,
essayé une fois par `stream/FrameCrop.js`) ; la session reçoit
`SessionConfig::clientCropsToFrame`, l'encodeur remplit alors le cadre
jusqu'à la grille (1920×1088), l'image entière dans la taille d'affichage, et
la session annonce 1920×1080. La vue coupe à la sortie du décodeur
(`StreamView`, `VideoDecodeWorker`), en AV1 seulement, avant tout renderer :
une `VideoFrame` sur la même image, `visibleRect` réduit, sans copie, et
seulement si l'image dépasse la taille annoncée de moins d'un superbloc. Sans
le drapeau (frontend ancien, invité), l'image reste sur la grille (1792×1008).
La preuve au pixel code désormais le cadre rempli (révision 2, 48,6 dB).
Vérifié : `linux_session` (1920×1080 annoncés sur un cadre 1920×1088), et
Chrome headless, décodeurs logiciel et matériel — l'image coupée fait
1920×1080, sa dernière rangée est la rangée 1079 de l'image.

**Pas fait** : un stream AV1 vers un vrai client par WebRTC (la chaîne d'envoi
est celle de l'AV1 de Windows) ; la conversion RVB du VCN
(`VALVE_video_encode_rgb_conversion`), en sonde de labo seulement d'après le
plan, à revoir sur RDNA4.

**Concrètement, pour l'utilisateur** : sous Linux avec une carte AMD récente,
choisir la chaîne Vulkan Video dans l'administration rend l'AV1 possible pour
les navigateurs qui le préfèrent, le 1080p à pleine taille (1792×1008 agrandi
seulement pour un frontend ancien). Avec le réglage par défaut, rien ne change.

### 32.28 Linux : la chaîne Vulkan Video par défaut sur AMD (décision de Bruno, 05/10/2026)

**La décision.** Bruno, le 05/10, sur le constat du §32.25 (VA-API à
14,3 Mbit/s sur 20 sur un bureau presque immobile sous Mesa 26, la chaîne
Vulkan Video à 0,8) :
- (c) la chaîne Vulkan Video devient la route par défaut sur AMD sous Linux, là
  où la sonde et la preuve au pixel la valident ; VA-API reste le repli
  automatique, puis OpenH264 ;
- (a) la cause est cherchée à côté (le contrôle de débit de radeonsi 26, ou nos
  références), sans retenir (c).

**Le code** (`LinuxRouteChoice.h`, `4e998380`) :
- la ligne AMD de la table des vendeurs (`autoLinuxPipeline`) passe de VA-API à
  Vulkan Video ; Intel et NVIDIA ne bougent pas ;
- la table ne demande que ce que la chaîne peut porter. En H.264, que
  l'encodeur Vulkan Video ne fait pas encore, et dans un build sans la chaîne,
  VA-API tourne par la ligne de la table elle-même, sans refus : la route
  scindée (Vulkan compute → VA-API) comme avant ;
- ce que la preuve ou le stream ont appris reste un refus, dit comme tel
  (« the vendor table for AMD asks for Vulkan Video, which cannot run: … ;
  VA-API runs »), comme la table de Windows (`VideoPipelineChoice.h`) ;
- la preuve au pixel ne tourne que là où la chaîne serait prise
  (`linuxRouteWantsVulkanVideo`) : désormais toute session HEVC ou AV1 sur AMD,
  une fois par taille, gardée dans le cache de l'utilisateur ;
- l'AV1 est offert par défaut sur AMD (`offerSessionCodecs`), là où le pilote
  montre l'encodeur ; un client ne le reçoit que s'il l'a choisi (le codec
  « Auto » du client demande HEVC puis H.264) ;
- le réglage « VA-API » de l'administration reste le chemin de retour : GL
  devant VA-API, la chaîne telle qu'elle a toujours tourné.

**Les tests** : `linux_route_choice` (table, H.264, build sans la chaîne,
preuve refusée, mémoire partagée du portail, VA-API sans jeux de paramètres,
retour par le réglage) ; `linux_session` dit la route de la session HEVC sans
réglage et vérifie que, sur AMD, ce qui n'est pas la chaîne Vulkan Video est un
refus ou un build sans elle.

**(a) La cause, lue dans les sources** (Mesa 23.2.1 contre 26.2.3, sans banc) :
- **La vague d'intra-refresh de VA-API, la part de 14 à 20 Mbit/s.**
  - Mesa 23.2 forçait `RENCODE_INTRA_REFRESH_MODE_NONE`. Le rafraîchissement
    par colonnes arrive avec Mesa 24.1 (MR 27101).
  - `VaapiEncoder` le prend dès que le pilote l'offre et le balaie sans écart :
    une colonne par image, 120 images, et ça recommence.
  - Mesa élargit la bande d'une unité quand le filtre de boucle est actif : en
    HEVC 1080p, 128 px de large ; en H.264, 32.
  - Mesa n'applique pas `qp_delta_for_inserted_intra`, et le QP n'a pas de plancher.
  - La chaîne Vulkan Video balaie, elle, une période sur quatre, avec un QP
    plancher de 18 : la comparaison n'était pas à réglages égaux.
  - Les autres clients VA-API (ffmpeg, OBS, GStreamer) n'envoient pas de
    rafraîchissement par défaut.
- **Le fond de 14,3 Mbit/s sans intra-refresh n'est pas expliqué par là**
  (§8o.15, colonne « sans »). Écartés dans le code de Mesa : `min_qp`/`max_qp`
  (même sens), VBAQ, pré-encodage et niveau de qualité (éteints sans tampon
  `QualityLevel`), bourrage (coupé), saut d'images (codé à 0), remise à zéro du
  contrôle de débit (seulement si le débit ou la cadence change), HRD.
- **Reste en lice** :
  - en HEVC sur VCN 2 à 4, le preset « speed » devient « balance » tant que le
    SAO n'est pas coupé (Mesa 26, MR 40766) ;
  - le micrologiciel VCN (1.24) et le noyau (7.0), qui ont changé avec Mesa.
- **Bancs proposés**, sur la page de §8o.15 :
  - H.264 contre HEVC par VA-API, sans intra-refresh (pas de changement de
    preset en H.264) ;
  - le SAO coupé en HEVC ;
  - un plancher `min_qp` de 18 ;
  - la vague espacée de quatre périodes, comme Vulkan Video.
- **Touche le défaut restant** : le H.264 sur AMD passe toujours par VA-API.
  Sa vague continue y prend tout le budget sur un écran fixe, pour un client qui
  traverse les pertes. **Fait le 05/10, « ok » de Bruno** (`644f57e1`) : un
  balayage toutes les quatre périodes, comme Vulkan Video, en H.264 comme en
  HEVC, l'horizon annoncé au client étant l'écart plus un balayage ; `irdist=-1`
  les remet dos à dos, et la clé `vaminqp=` mesure un plancher de QP.
- **Le libellé** de l'administration devient « Vulkan », sans « (experimental) »
  (décision de Bruno, `059a970e`) ; `/api/native/status` dit `vulkan` pour ce
  qu'Auto prend sur AMD (`f7683e83`).

**Au banc** (05/10 au soir, §8o.19 du banc) :
- **Le défaut est validé.** Sans réglage, le HEVC sur le 780M prend Vulkan
  Video, à 0,59 Mbit/s sur la page fixe, et à 1,0 avec l'intra-refresh espacé.
- **La cause de VA-API est le QP, pas la vague.** Sans intra-refresh, VA-API
  reste à 13,7 Mbit/s en HEVC et à 17,3 en H.264 : le preset HEVC est hors de
  cause. Avec `vaminqp=18`, il tombe à 0,51 en HEVC et à 0,25 en H.264. Sous
  Mesa 26, le contrôle de débit descend sous QP 18 et affine la page fixe
  jusqu'à remplir le budget.
- La vague espacée est juste mais ne change rien tant que ce fond remplit le
  budget (17,5 contre 17,7 Mbit/s).

**Le plancher de QP de VA-API** (décision de Bruno le 05/10 au soir,
`d287dbb5`) : 18 par défaut, comme NVENC et AMF (`encode::kVaapiMinQp`).
`vaminqp=` reste pour le banc (-1 : aucun). Au banc, ce plancher ramène le
H.264 de 17,3 à 0,25 Mbit/s sur la page fixe. Le défaut compilé est vérifié
sous Linux le 06/10 (§8o.20 du banc) : les ready de VA-API disent
« QP >= 18 », et le H.264 sans réglage tombe de 17,2 à 0,51 Mbit/s sur la page
fixe en 1440p.

**Concrètement, pour l'utilisateur** : sous Linux avec une carte AMD récente,
le HEVC et l'AV1 passent par Vulkan Video sans rien régler. Un bureau presque
immobile ne prend plus presque tout le débit. Si le pilote ne prouve pas qu'il
encode juste, le stream passe par VA-API de lui-même. Le réglage « VA-API »
de l'administration ramène l'ancien chemin.

## 33. Framerate « Hôte » : le stream à la cadence de l'écran de l'hôte (ouvert le 29/09/2026)

Plan `framerate-hote.md` : l'essai « cadence de l'hôte » du POC Ultra, sorti en
petit plan à part. Tout vit derrière des clés de banc ; aucun défaut ne change
avant la décision de Bruno.

### 33.1 Ce que coûte la cadence du client

Le stream va aujourd'hui à la fréquence du **client**. « Auto » vaut celle de
son écran, plafonnée à 120 (`util/RefreshRate.js`), et le navigateur l'envoie
comme plafond (`stream_fps_max`). L'hôte y pose sa porte (§9.6, `FrameCadence`) :
il n'encode que la première présentation de chaque intervalle du stream. Un
changement tombé dans une présentation écartée attend la suivante admise.
L'écran virtuel du produit tourne lui aussi à la fréquence du stream.

Le modèle de l'attente entre un changement à l'écran et sa capture :
- hôte à 165 Hz, client à 60 : ~8,3 ms en moyenne (au pire ~17) aujourd'hui,
  ~3 ms (au pire ~6) si chaque présentation part ;
- écran virtuel à 60 Hz : 8,3 ms en moyenne ; à 240 Hz, 2,1 ms, même pour un
  jeu à 60 i/s, dont chaque image attend la composition suivante ;
- à 500 Hz (le maximum du pilote), ~1 ms (au pire 2).

Desktop Duplication ne livre que les présentations qui changent : un écran
virtuel à 500 Hz sous un jeu à 60 i/s coûte 60 encodages par seconde, pas 500.
C'est l'équivalent d'un VRR côté hôte.

L'E2E par image ne voit pas ce gain : il part de l'instant de capture. Le
critère est l'**âge du contenu** montré au client : l'instant où la page a
dessiné ce que le client affiche, lu sur l'image elle-même (§33.3).

### 33.2 Les clés

- `MW_NATIVE_TUNING=cadence=host` : encodeur dimensionné pour la fréquence de
  l'écran capturé, **aucune porte**. Chaque présentation que DDA livre est
  convertie, encodée et remise à l'envoi. Ni alignement sur le client, ni
  `stream_fps_max`, ni `clientfpscap` : reçus et journalisés, pas appliqués.
- `cadence=host-ceiling` : la porte d'aujourd'hui posée sur la fréquence de
  l'écran de l'hôte (`FrameCadence::ceiling`) : une source qui présente plus
  vite que l'écran ne rafraîchit est tenue à sa fréquence.
- `cadence=host-guarded` : `host` plus un **crédit de décodage**. Le client
  dit quand sa file de décodage dépasse une image, et l'hôte saute des
  présentations tant qu'elle n'est pas revenue. Sauter une présentation ne
  coûte rien à l'hôte ; le client, lui, ne peut jeter une image P qu'en payant
  une image clé. Le gouverneur de débit de décodage (`DecodeRateGovernor.js`)
  réagit en trois secondes ; le crédit, en un aller-retour.
- `MW_VDD_REFRESH=<Hz>` : la fréquence de l'écran virtuel du produit, jusqu'à
  500 Hz (le maximum du pilote), pour la clé seulement. `kRateMax` (240) ne
  change pas.

Sur un encodeur débordé, la règle reste « la plus fraîche, jamais de file ».

### 33.3 La mesure : l'âge du contenu

La bande `?band=1` de `scripts/bench/content/scroll.html` porte le numéro de
chaque image de la page. La page garde la table « numéro → instant du rAF »,
calée sur l'horloge du backend par CDP. Le client relit le numéro sur l'image
qu'il vient de dessiner (`probePixels`) et en déduit l'âge de ce qu'il montre,
avec son horloge estimée contre celle de l'hôte par le ping/pong.

### 33.4 La porte

Le verdict se donne par couple hôte × client et par mode de peinture du
client (tearing ou vsync). Une cadence est recommandée pour un couple si :
- l'âge du contenu médian baisse d'au moins 2 ms (ou 20 %), avec un p99 pas pire ;
- le clic → drapeau n'est pas pire ;
- les images répétées et sautées par minute ne montent pas ;
- les i/s du jeu restent à −3 % près ;
- le client suit sans demander de plafond ni descendre l'échelle de qualité.

Pour `host-guarded`, les sauts du crédit sont permis (c'est son rôle) et
rapportés ; une file de décodage qui tient, ou une descente de l'échelle,
invalide la passe. La recommandation au produit vise `host-guarded`, à la
fréquence d'écran virtuel la plus haute qui garde les i/s du jeu à −3 % près.
Bruno tranche.

### 33.5 Ce qui est construit (29/09/2026)

**Les clés** (Windows seulement ; Linux et macOS gardent leur copie du choix et
ignorent la clé) :
- Le choix de la cadence sort de `WindowsSession` pour `core/CadenceChoice.h`,
  une fonction pure. Sans clé, c'est le même choix et la même ligne de journal,
  mot pour mot (tests natifs). Avec `cadence=host…`, la ligne dit « the host's
  rate » et nomme les plafonds reçus et non appliqués ; un plafond qui arrive
  en cours de session est journalisé de même.
- Le crédit (`core/DecodeCredit.h`) : le client dit la profondeur de sa file
  (`decodequeue`, `DecodeQueueSignal.js`) dès qu'une deuxième image attend, la
  redit toutes les 50 ms tant qu'elle reste pleine, et dit quand elle est
  revenue à une. L'hôte retient la présentation qu'il allait encoder tant que
  le dernier mot est « deux ou plus » et date de moins de 250 ms. L'image
  retenue est toujours la plus fraîche ; elle part dès le retour du crédit,
  sans attendre la présentation suivante (la boucle regarde alors toutes les
  millisecondes).
- Écart au plan : il voulait un signal « frais d'un aller-retour ». Or le
  client ne redit sa file qu'à ses sorties de décodeur, à quelques
  millisecondes d'écart au mieux : un crédit qui expire après un aller-retour
  de LAN laisserait l'hôte envoyer entre deux sorties. D'où le « clear »
  explicite, et le délai de 250 ms comme seul garde-fou (un « clear » perdu, une
  page partie).
- Modèle (tests natifs) : hôte à 500 présentations/s, décodeur à 100 i/s. Sans
  crédit, 800 images en file après deux secondes. Avec, deux au plus sur un
  LAN. À 10 ms dans chaque sens, neuf au plus, et 183 images décodées sur 200.
  Les seuils 2/1 gardent le débit du décodeur au prix d'environ une image en
  file ; des seuils 1/0 videraient la file pour ~9 % d'images en moins. À
  trancher par la mesure si la file tient sur un décodeur lent.
- `MW_VDD_REFRESH` : la garde de chaque requête porte désormais le plafond du
  pilote (500) ; le plafond du produit (240) s'applique là où le produit
  choisit la fréquence (`refreshForStream`). Vérifié sur l'Arc : l'écran
  virtuel du produit en 2560×1440 à 240 puis à 500 Hz. Sur un XML qui n'est pas
  le nôtre (le VDD de Bruno sur DualRTX), le produit y ajoute le mode demandé ;
  le banc sauve le fichier avant et le remet après. Ajouter 240 et 500 à la
  liste **globale** de ce fichier a empêché l'écran d'apparaître : ne pas le
  refaire.

**L'instrument** (`ContentAgeProbe.js`, `scripts/bench/content-age`) :
- La bande de `scroll.html?band=time` porte l'horloge `steady_clock` de l'hôte
  (QPC sous Windows). Le pilote la cale par CDP : `time.perf_counter_ns()` de
  CPython lit le même compteur (vérifié, 0,002 ms d'écart), et le meilleur de
  40 échanges laisse moins de 0,3 ms d'erreur.
- Le client lit la bande dans l'image décodée (`VideoFrame.copyTo` du seul
  rectangle, asynchrone), date le dessin de la même image (`afterDraw`), et
  la met sur l'horloge de l'hôte par l'estimateur du ping/pong (le pong porte
  désormais l'heure de l'hôte). Lire le canevas coûtait 13 à 14 ms de fil
  principal par lecture sur l'iGPU AMD, à toute cadence : écarté.
- Trois âges par image : **contenu** (dessin − heure de la page), **capture**
  (dessin − présentation de l'image sur l'hôte, `backendTs`) et **avant la
  capture** (leur différence : ne dépend que des horloges de l'hôte).
- Et l'âge de ce qui est **affiché**, qui seul compare deux cadences : l'image à
  l'écran vieillit jusqu'à la suivante (16,7 ms de plus à 60 i/s, 2 à 500).
  `shown` l'échantillonne toutes les 0,5 ms, `atRefresh` à chaque
  rafraîchissement du client.
- Contrôle sur DualRTX (client sur la même machine, même compteur) :
  l'estimateur tombe à 0,01-0,08 ms de l'horloge exacte ; 1 800 bandes lues sur
  1 800 images, aucune invalide.

### 33.6 Premiers résultats (29/09/2026 au soir, provisoires)

Hôte DualRTX, écran virtuel du produit rendu par l'Arc (D3D12 Video Encode,
HEVC), `scroll.html` à la fréquence de l'écran, 30 s par passe. Âge médian de
ce qui est **affiché** (`shown`), en ms.

**Client sur DualRTX** (iGPU AMD, écran à 60 Hz ; mise au point seulement : son
rAF suit l'écran virtuel), une passe :

| Écran virtuel | Auto (60 i/s) | host | host-ceiling | host-guarded |
|---|---|---|---|---|
| 60 Hz | 50,5 | 51,2 | 50,0 | 33,0 ¹ |
| 240 Hz | 36,3 | 557 | 561 | 81,9 (51,1 avec `pending`) |
| 500 Hz | 31,7 | 697 | 676 | 81,0 (42,3 avec `pending`) |

¹ Le crédit n'a rien retenu ; la page était plus rapide à ce lancement
(5 ms avant la capture, contre 22).

**Client mw-mac** (M1, Chrome, 120 Hz, tearing, **Wi-Fi**), moyenne de deux
passes alternées :

| Écran virtuel | Auto (120 i/s) | host | host-ceiling | host-guarded |
|---|---|---|---|---|
| 120 Hz | 52,1 | 49,0 | 49,0 | 50,0 |
| 240 Hz | 42,5 | **31,1** | 35,6 | 38,4 ² |
| 500 Hz | 38,6 | 116 | 185 | 53,6 |

² Le crédit n'a presque rien retenu (12 à 14 présentations par passe) : l'écart
avec `host` est le Wi-Fi d'une passe à l'autre, après la capture.

Lecture provisoire :
- **L'écran virtuel rapide est le gros levier, même à la cadence
  d'aujourd'hui.** Sa part se lit avant la capture : 22 → 8 → 4 ms (60 → 240 →
  500 Hz) sur DualRTX, 17 → 8 → 4 ms (120 → 240 → 500 Hz) avec le Mac. Le flux
  reste à la fréquence du client : ni débit ni décodage en plus.
- **La cadence de l'hôte ne paie que si le client décode ce rythme.** Le Mac
  suit 240 i/s (230 dessinées) : `host` y gagne ~11 ms sur Auto au même écran
  virtuel, et c'est le meilleur couple mesuré (−21 ms contre l'écran virtuel à
  la fréquence du client). À 500 Hz, le Mac comme l'iGPU AMD décrochent : 0,1
  à 0,7 s de file.
- **Le crédit évite le pire, sans rendre utile un flux plus rapide que le
  décodeur.** Compter `decodeQueueSize` laisse une file dans le décodeur ;
  compter les images soumises et pas sorties (`pending`) la divise par deux.
- **Le Wi-Fi du Mac pèse ±10 ms d'une passe à l'autre**, après la capture : une
  passe en Ethernet, et plus de répétitions, sont nécessaires avant la porte.

**Ce que le crédit doit compter** (`host-guarded`, âge affiché médian, ms) :

| Client, écran virtuel | `decodeQueueSize` | `pending` | `delay` | `host` |
|---|---|---|---|---|
| iGPU AMD local, 240 Hz | 81,9 | 51,1 | — | 557 |
| iGPU AMD local, 500 Hz | 81,0 | 42,3 | — | 697 |
| Mac M1, 240 Hz | 38,4 (2 passes) | 53,6 (2) | 37,8 (1) | 31,1 (2) |
| Mac M1, 500 Hz | 53,6 (2) | 52,0 (2) | 42,5 (1, partielle) | 116 (2) |

- `decodeQueueSize` (le plan) ne voit que ce qui attend devant le décodeur :
  l'iGPU AMD en tenait huit de plus dedans.
- `pending` (soumises, pas encore sorties) les voit, mais le M1 en garde plus
  d'une en vol quand il suit : sous 240 Hz, le crédit retenait 48 à 90
  présentations par seconde pour rien.
- `delay` (`DecodeDelay`) compte le retard de la plus ancienne image dans le
  décodeur au-delà du décodage habituel, en intervalles du flux : indifférent
  à la profondeur propre du décodeur.

Série du 29/09 à 22:30-23:15 (deux passes par case sur le Mac, les trois
cadences intercalées ; une ou deux sur l'iGPU AMD) :

| Client, écran virtuel | Auto | host | guarded `delay` | guarded `pending` | guarded `delay30` |
|---|---|---|---|---|---|
| Mac M1, 240 Hz | 46,5 | 40,3 | **38,6** | — | — |
| Mac M1, 500 Hz | 39,0 | 147 | 45,9 | — | — |
| iGPU AMD local, 240 Hz | — | — | 89,2 | **59,6** | 68,2 |
| iGPU AMD local, 500 Hz | — | — | 93,5 | **49,4** | 95,7 |

- Sur le Mac, `delay` ne coûte rien quand il suit (20 présentations retenues
  par seconde à 240 Hz) et tient la file quand il décroche.
- Sur l'iGPU AMD, un signal en temps s'y trompe, même sur 30 s
  (`delay30`) : ce décodeur (D3D11 de Chrome) semble ne rendre une image qu'à
  l'arrivée de la suivante, si bien que retenir allonge le décodage qu'on
  mesure. Un compte (`pending`) ne s'y trompe pas.
- Aucun signal ne gagne partout. Prochain essai : « `pending` ≥ 3 ou
  `delay` ≥ 2 ».

**Un défaut vu en passant, antérieur à ce plan** : quand l'activation de
l'écran virtuel échoue (« the virtual display did not appear »), le nœud reste
activé sans écran ; seul le démarrage suivant de l'instance l'éteint
(`resetAtStartup`). Vu deux fois le 29/09 (un XML modifié à la main, puis
1784×1160 à 500 Hz, refusé une fois après avoir été accepté).

**Et une précaution de banc** : l'écran de la RTX de DualRTX a quitté Windows
pendant une série de bascules de l'écran virtuel (21:41), et n'est pas revenu
au rebranchement. Les séries sur l'écran virtuel se font quand Bruno n'est pas
devant ses écrans.

### 33.7 La nuit du 29 au 30/09 : Arc et RTX, client mw-mac

Client mw-mac (M1, Chrome, 120 Hz, tearing, Wi-Fi), écran virtuel du produit à
la taille du client (~1790×1160), `scroll.html`, signal du crédit `delay`.
Arc : 4 passes par case (D3D12 Video Encode). RTX : 2 passes par case (NVENC,
D3D11 ; l'écran virtuel rendu par la RTX). Âge affiché médian, en ms, moyenne
des passes (entre parenthèses : avant la capture) :

| Hôte, écran virtuel | Auto (120 i/s) | host | host-guarded |
|---|---|---|---|
| Arc, 120 Hz | 52,2 (16,3) | 51,1 | 53,3 |
| Arc, 240 Hz | 41,2 (5,9) | 35,4 | **35,1** |
| Arc, 500 Hz | 43,6 (2,5) | 188 | 51,0 |
| RTX, 120 Hz | 36,1 (7,6) | 42,9 ³ | 34,7 |
| RTX, 240 Hz | 34,9 (7,8) | 33,8 | **32,2** |
| RTX, 500 Hz | 34,8 (3,8) | 182 | 115 ⁴ |

³ Une passe où la page était lente (11,9 ms avant la capture) : bruit de la
page, pas de la cadence (à 120 Hz, host et Auto encodent la même chose).
⁴ Le crédit n'a pas tenu la file du Mac sous 450 présentations/s de la RTX
(129 images dessinées par seconde, p99 d'une seconde).

Lecture :
- **240 Hz est le bon point**, pas 500. À 500 Hz l'hôte convertit chaque
  présentation (450 par seconde) même quand il n'en encode que 120 : la
  capture vieillit (Arc 30,8 → 36,5 ms, RTX 22,9 → 26,6 ms), ce qui mange le
  gain d'attente avant la capture.
- **Par rapport à aujourd'hui** (écran virtuel à la fréquence du client,
  120 Hz, Auto) : l'Arc passe de 52,2 à 35,1 ms (−17 ms), la RTX de 36,1 à
  32,2 ms (−4 ms). Le gros du gain sur l'Arc vient de l'attente avant la
  capture (16,3 → 5,9 ms, écran virtuel à 240 Hz) : la page s'y rend plus
  lentement que sur la RTX.
- **La cadence de l'hôte ajoute 1 à 6 ms** à 240 Hz sur ce client, qui décode
  240 i/s ; `host-guarded` fait aussi bien que `host` en ne retenant presque
  rien (7 à 18 présentations par seconde).
- **À 500 Hz, ni host ni host-guarded ne tiennent** sur le Mac. Un écran
  virtuel à 500 Hz sous une cadence de l'hôte n'est pas un candidat.
- Mesure : 54 passes sur 56 le premier coup ; deux échecs de l'écran virtuel à
  500 Hz sur la RTX, puis toutes les fréquences, une fois que le XML
  étranger avait accumulé un second mode à 500 Hz. Le banc repart désormais
  d'un XML propre à chaque passe (`local_matrix.py`).

**Le N95 (Intel UHD, Wi-Fi, écran à 60 Hz), même nuit, Arc** (banc §8p.4 bis) :
Auto 66,4 ms à 60 Hz, **53,0 ms** à 240 Hz ; `host` à 240 Hz, plus d'une
seconde ; `host-guarded` 169 (`delay`) et 144 ms (`pending`). Le crédit y
ramène l'hôte à ce que le client dessine (~75 i/s) mais laisse 150 ms de file
hors du décodeur, dans le transport ou le fil principal : aucun des deux
signaux n'y regarde. Le délai aller du récepteur (`linkstats`, déjà envoyé à
l'hôte) serait le signal à essayer pour cette file-là.

**Ce que ça dit du produit, à ce stade** : l'écran virtuel à 240 Hz sous la
cadence d'aujourd'hui gagne sur les trois clients (Mac −11 à −17 ms, N95
−13 ms), sans rien demander au réseau ni au client. La cadence de l'hôte ne
gagne que sur un client qui suit (Mac : 1 à 6 ms de plus), et son garde-fou ne
voit pas encore toutes les files : elle ne peut pas être le défaut en l'état.

**Le client DualRTX, sans le poids de la sonde** (banc §8p.4 ter) : à 240 Hz,
Auto 31,1 ms ; `host` 55,6 ms alors que l'iGPU décode ses 233 images par
seconde — à ce rythme chaque image passe plus longtemps dans le décodeur (57 ms
de capture contre 20). Sur ce client, la cadence de l'hôte coûte de la latence
même quand il suit. Elle n'a gagné que sur le Mac.

**Un dernier signal, `e2e`** (30/09, ~02:30) : l'excès du retard depuis la
capture sur l'hôte (`backendTs`) jusqu'à la sortie du décodeur, sur 10 s, pour
voir aussi une file dans le transport ou devant le fil principal. Sur le N95 à
240 Hz : 141 ms affiché (Auto : 53) ; le crédit retient 180 présentations par
seconde, l'hôte n'envoie plus que ~40 images par seconde — moins qu'Auto — et
l'âge reste haut : les retenues rendent la livraison saccadée, chaque image
reste plus longtemps à l'écran. Sur l'iGPU AMD : 60,2 ms (Auto : 31,1).

**Ce que ça tranche** : sur un client qui ne décode pas la cadence de l'hôte,
aucun crédit, quel que soit ce qu'il compte, ne rend cette cadence meilleure
qu'Auto. La cadence de l'hôte ne peut valoir que pour un client dont on sait
d'avance qu'il suit ; c'est un choix à faire en amont, pas une file à
rattraper.

**Le premier client en Ethernet : l'UM790Pro sous Windows** (Radeon 780M,
Chrome sur un écran à 120 Hz ; banc §8p.4 quinquies). Face à aujourd'hui
(écran virtuel à 120 Hz, Auto : 35,6 ms en tearing, 39,0 en vsync), l'écran
virtuel à 240 Hz donne −9,2 ms sous Auto, −11,8 sous `host`, **−13,8 ms sous
`host-guarded` (21,8 ms, p99 31)** ; en vsync −5,6 (Auto) et −8,3 ms (`host`).
Sur un client qui suit, en Ethernet, la cadence de l'hôte ajoute 2 à 5 ms à ce
que l'écran virtuel rapide gagne seul.

**Trois hôtes, le client en Ethernet** (banc §8p.4 sexies). Le gain de
l'écran virtuel à 240 Hz suit le temps que l'hôte met à composer ce qui
s'affiche : **−17 à −20 ms sur l'iGPU AMD** (45,5 → 25,9 ms), −9 à −14 ms sur
l'Arc, rien de sûr sur la RTX (23,7 → 22,9 ms, dans la variation de la page
d'un lancement à l'autre). C'est là que l'écran virtuel à la fréquence du
client coûte le plus : un GPU faible compose la page en une à deux images de
l'écran, et chacune dure 8 ms à 120 Hz contre 4 à 240.

### 33.8 L'émission calée sur l'affichage du client (`cadence=deadline`, 30/09/2026)

L'idée est de Bruno (décision H4, 30/09) ; elle remplace la cadence de l'hôte
comme candidat au produit. L'hôte capture vite (écran virtuel à 240 ou
500 Hz), mais n'envoie **qu'une image par rafraîchissement du client** : la
plus fraîche qui peut encore y arriver.

**Pourquoi c'est mieux que les deux précédentes.**
- La cadence d'aujourd'hui décode une image par rafraîchissement, mais sa
  grille tombe n'importe où dans ce rafraîchissement : l'image attend en
  moyenne une demi-période de trop.
- La cadence de l'hôte rattrape cette demi-période (le client prend la plus
  fraîche) au prix de tout décoder : sur un client qui ne suit pas, aucun
  crédit ne la sauve (§33.7).
- L'émission calée vise la latence de la seconde avec les décodages de la
  première.

**Ce qui est construit.**
- **Côté client** (`frontend/js/stream/VsyncGrid.js`, `77dc627d`) : période et
  dernier rafraîchissement ajustés sur les horodatages de
  `requestAnimationFrame`, mis sur l'horloge de l'hôte par l'estimateur
  ping/pong (sorti de la sonde d'âge vers `util/ClockEstimator.js`). Toutes les
  500 ms, un message `vsyncgrid` : période, un rafraîchissement, et l'**avance**
  = médiane de « prise → prête » sur 2 s + une marge. Prête = dessinée quand le
  canvas déchire, décodée en vsync.
- **La marge** se règle sur les ratées (une image prête après le
  rafraîchissement visé) : +1 ms par ratée ; après 5 s sans ratée, −moitié de
  ce que les images les plus serrées avaient de trop (1ᵉʳ centile), au moins
  0,25 ms, plancher 0,5 ms. Cible : ≤ 0,5 % de ratées. L'erreur de l'horloge
  s'annule : le rafraîchissement et l'instant « prête » passent par la même
  estimation.
- **Côté hôte** (`core/DeadlineCadence.h`, boucle Windows, `6f9a6bbc`) : pour
  le rafraîchissement R, l'image est prise à R − avance. La boucle ne prend
  rien entre deux rafraîchissements du client : elle dort jusqu'à cet instant
  (minuterie haute résolution, puis attente active de 250 µs), prend ce que
  l'écran a présenté en dernier et l'encode aussitôt. Desktop Duplication
  replie toutes les présentations intermédiaires dans cette prise, sans les
  convertir : une conversion par rafraîchissement du client, même à 500 Hz.
  Rien de neuf, rien d'envoyé. Une grille muette depuis 2 s, ou absurde, rend
  la main à la porte d'aujourd'hui.
- **L'horodatage** d'une image visée est l'instant de sa prise, pas sa
  présentation. Un jeu à 60 i/s sur un écran à 240 Hz laisse sa dernière
  présentation jusqu'à 16 ms avant l'instant ; une avance qui la compterait
  avancerait chaque prise pour rien.
- **Chaque pong** dit si l'hôte veut une grille (`grid`) et s'il la suit
  (`deadline`) : le client ne compte ses ratées et ne lâche sa réserve de rendu
  (`useReserve`, Chromium sans tearing) que dans ce cas.

**L'option C de Bruno** (convertir la dernière présentation convertible à
temps, sans attendre l'échéance) choisit **la même image** que la prise à
R − avance : la dernière présentation qui peut être prise, convertie, encodée
et livrée à temps. Sa seule différence est de traiter plus tôt, ce qui laisse
du temps en réserve sans rajeunir l'image ; la prise à l'échéance ne demande
ni la grille de l'écran de l'hôte ni son délai de composition. Le « ~1 ms » de
conversion annoncé le 30/09 pour la capture à l'échéance était une erreur : il
est dans l'avance mesurée, pour les deux façons de faire.

**Tearing** (décision de Bruno, 30/09) : la méthode vaut pour tous les
clients, « autoriser le tearing » reste activé. Une image dessinée avant le
début du balayage est montrée entière ; une image en retard déchire sur
quelques lignes du haut, puis elle est entière au rafraîchissement suivant.

**Les cas de Bruno : un jeu plus lent, puis plus rapide que le client** (30/09,
17:00 ; délais calculés, pas mesurés, client à 60 Hz, entre l'image du jeu et
son apparition à mi-hauteur de l'écran).
- Un jeu à 49-53 i/s : aucune image n'est jamais en concurrence pour un
  rafraîchissement. En tearing, attendre l'échéance coûtait ~9 ms (~18 contre
  ~8 ms pour l'envoi immédiat), puisque le canvas montre l'image dès qu'elle
  est dessinée sur les lignes que le balayage n'a pas atteintes. En vsync,
  l'échéance et l'envoi immédiat tombent sur le même rafraîchissement.
- Un jeu à 75-83 i/s : en vsync, la visée prend la plus fraîche, pour un gain
  faible (~1 ms : les images du jeu sont à 12,5 ms les unes des autres) ; en
  tearing, envoyer les 80 images (~6 ms) bat une par rafraîchissement (~8) et
  la visée (~16).
- Un écran VRR côté client attend l'image : en tearing, l'envoi immédiat est
  idéal (~3 ms, le temps du balayage à 170 Hz), si Chrome active le VRR pour
  une page — à vérifier au compteur de l'écran.

D'où la règle **ne jamais attendre pour rien** (`f2954fa4`) : la grille dit si
le canvas déchire, avec un budget (son rafraîchissement × `mw_vsyncgrid_budget`,
1 par défaut). Un canvas qui déchire reçoit chaque nouvelle image aussitôt, par
une porte au budget qui saute et ne retient jamais ; seul un client en vsync
est visé. La page de banc prend le rythme d'un jeu (`scroll.html?fps=49-53`,
`--game-fps`, `672d84f6`).

Côté hôte, l'écran virtuel n'a pas de balayage : à 500 Hz, une image du jeu
est composée entière au rafraîchissement suivant (≤ 2 ms, 1 en moyenne) et la
capture la prend entière. Un jeu qui déchire sur un écran physique de l'hôte
ne déchire que sur ce panneau : Desktop Duplication livre des images entières.

**Le banc (P3, 30/09 au soir, banc §8p.7)** : client N95 en Wi-Fi à 60 Hz,
une passe par case.
- En vsync, l'âge affiché baisse de ~11 ms, mais surtout par la réserve
  retirée, et le client ne dessine plus que ~35 images par seconde au lieu de
  43 à 50. L'étalement capture → prête du Wi-Fi (40 à 60 ms) dépasse la
  période (17 ms) : aucune marge ne tient, 11 à 22 % de ratées.
- En tearing, le même envoi qu'Auto, par construction.
- Côté hôte, la méthode tient : réveil à ~0,1 ms près, 55 à 65 conversions par
  seconde au lieu de 220 à 420.

**Le garde-fou** (`26e6134a`) : le client ne demande la visée que sur un lien
qui peut la tenir. Si capture → prête s'étale de plus d'une demi-période (p95
moins la médiane, sur 2 s) ou si la marge a atteint une période, la grille dit
`steady: false` : l'hôte envoie chaque image aussitôt, comme à un canvas qui
déchire (« the client's frames arrive too unevenly to aim »), et le client
garde sa réserve. La visée revient après 5 s sous un tiers de période, la
marge repartie de zéro. Sur le N95 : 50,8 images dessinées par seconde, la
cadence d'aujourd'hui retrouvée.

**VRR** : sur les M27Q de DualRTX (iGPU AMD en FreeSync, RTX en G-SYNC
Compatible fenêtré), un flux à 51 i/s en plein écran laisse l'écran à sa
fréquence (60 et 144 au compteur), même avec le réglage Windows du VRR des
jeux fenêtrés. Le cas 3 reste théorique ; l'écran virtuel principal à 500 Hz
pendant le stream est une cause possible, à écarter par un contrôle sans lui.

**Reste** : un client en Ethernet (UM790Pro sous Windows, Mac au calme), là où
la visée peut tenir — âge au rafraîchissement en vsync contre Auto, avec les
mêmes décodages et ≤ 0,5 % de ratées ; le contrôle VRR sans écran virtuel.

### 33.9 Décision A en produit : l'écran virtuel à 240 Hz (30/09/2026)

Sous Windows, l'écran virtuel du produit tourne à 240 Hz (`kRateMax`) quelle
que soit la fréquence du client (`refreshForStream(…, faster)`, `03c189ea`).
Le flux garde la sienne — celle de l'écran du client sous Auto — et encode la
première présentation de chaque intervalle : rien de plus n'est encodé ni
envoyé, le débit automatique ne bouge pas. Une image que l'hôte dessine attend
la composition 4,2 ms au plus, au lieu d'une période du flux.
- Mesuré : −6 à −17 ms d'âge affiché derrière un Arc ou un iGPU, neutre
  derrière la RTX (§33.7, banc §8p.4 sexies) ; clic → drapeau sur le N95,
  −5,5 ms en médiane et −15 au p90 (banc §8p.8).
- macOS garde la fréquence du flux ; `MW_VDD_REFRESH` reste la clé de banc ;
  le journal de session dit la fréquence et pourquoi.
- Reste : les i/s d'un vrai jeu à 240 Hz (RE9 n'a pas pu être piloté ce
  soir-là) et le ressenti de Bruno.

### 33.10 « Auto » avec détection (01/10/2026, POC Ultra, Phase UA)

Décision de Bruno (01/10) : la cadence de l'hôte revient comme un
comportement de l'« Auto » existant, sans choix de plus dans la liste, avec
une clé pour le couper. Esquisse : plan `framerate-hote.md` §14 ; étapes et
porte : Phase UA du plan du POC Ultra.

**Pourquoi une détection.** `host-guarded` gagne 3 à 6 ms sur un client qui
suit (UM790Pro en Ethernet, Mac) et perd lourdement ailleurs (N95 : 140 à
170 ms ; iGPU AMD local : 31 → 47 ms), et aucun crédit ne voyait la file du
N95 (§33.7). On ne sait pas d'avance qui suit : le client le mesure, un palier
à la fois, et redescend dès que ça coûte.

**L'échelle.** La fréquence du flux (celle de l'Auto), puis deux fois elle,
puis la fréquence de l'écran capturé (240 Hz sur l'écran virtuel du produit,
§33.9), chacune plafonnée à cette dernière. Un palier n'est essayé que si
l'écran capturé présente nettement plus vite que le palier en cours (× 1,15),
et cela dans cinq relevés de l'hôte d'affilée, soit environ 5 s : un jeu à
49-53 i/s sur un client à 60 Hz n'en déclenche aucun, un jeu à 75-83 i/s monte
à 120 et s'y arrête. Les cinq relevés viennent du banc (UA.3, 01/10). Un kiosque
qui s'ouvrait par-dessus la page à 50 i/s a présenté 169 puis 84 fois par
seconde pendant quatre relevés. Avec un seul relevé comme seuil, il avait
déclenché un essai.

**L'hôte** (`core/CadenceStep.h`, Windows seulement) :
- Le client demande un palier par `fpsstep` (0 : retour à sa fréquence). Le
  relais répond aussitôt : appliqué, plafonné à la fréquence de l'écran, ou
  refusé avec la raison. Les raisons : un encodage dont le p95 sur la dernière
  fenêtre de 2 s dépasse deux images à ce rythme (une seule pour l'encodeur
  logiciel, OpenH264, qui n'en a jamais deux en vol : une VM sans GPU, décision
  de Bruno du 02/10), une clé de banc `cadence=`, un
  plafond du décodeur (`clientfpscap`), un client en vsync, un écran pas plus
  rapide que le flux. Un refus laisse le palier déjà gardé.
- La boucle applique le palier entre deux images, par le chemin d'un écran
  client qui change (`chooseCadence` : porte à ce rythme, plafond à la
  fréquence de l'écran). Le budget par image suit (`EffectiveCadence::retarget`)
  et le fil garde le débit de la fréquence du client : le doubler avait coûté
  ~3 ms (banc §8p.4 septies).
- Les contrôles de charge (Lanczos-2, palier du CPU) gardent la fréquence de
  base : un essai à 240 ne coûte pas sa mise à l'échelle à la session.
- `stats.cadence`, chaque seconde : les présentations par seconde de l'écran
  capturé (acquises et repliées), la fréquence de base, le palier en cours,
  la fréquence de l'écran.
- Linux et macOS refusent tout palier et n'annoncent rien ; Sunshine, Wolf et
  MultiSeat ne connaissent pas le message.

**Le client** (`stream/CadenceStepper.js`) :
- L'âge de ce qui est montré : la médiane de capture → peinte, sur l'horloge
  de l'hôte (estimateur du ping/pong, un ping toutes les 500 ms tant qu'il
  tourne), plus une demi-période entre deux images peintes.
- 2,5 s de base au palier en cours, la demande, 0,4 s après la réponse, puis
  2,5 s d'essai. Le palier est gardé si l'âge baisse d'au moins 1 ms et si au
  moins 90 % des images reçues sont peintes. Sinon retour au palier d'avant et
  recul : 30 s, 1, 2, 4, 8, 16 min, remis à zéro quand le contenu (la bande des
  présentations) ou le lien (écran, retour d'arrière-plan) change. Un palier
  gardé sert de base à l'essai suivant.
- Le contenu commande aussi la descente, sans faute à compter ni recul. Au
  banc du 01/10, avant cette règle, un palier gardé sur une page qui
  ralentissait restait en place ; le filet sautait 20 à 25 s plus tard, et le
  p99 grimpait (jusqu'à 100 ms). Désormais :
  - un essai pendant lequel le contenu ralentit est repris (« non concluant ») ;
  - un palier gardé que le contenu n'utilise plus, deux relevés d'affilée,
    redescend au palier qu'il utilise.
- Le filet est armé dès que le flux dépasse la fréquence du client, essais
  compris. Il déclenche dans deux cas :
  - la médiane de capture → peinte sur les 250 dernières ms reste 500 ms
    au-dessus de sa référence (mesurée à la fréquence du client) d'une
    demi-période du client ;
  - deux images restent en file au décodeur pendant 150 ms.
  Le flux revient alors aussitôt à la fréquence du client, en un aller-retour.
  Deux filets d'affilée sur un même lien (`NET_STRIKES`), et l'essai suivant
  attend le plus long recul (16 min), même si le contenu change ; un palier
  gardé qui tient 30 s, ou un lien qui change, efface le compte. Au banc du
  02/10, le N95 a sauté au filet à chacun de ses 8 essais.
- Le filet s'élargit sur confiance (UA.3 bis, 02/10). À sa propre fréquence,
  un Mac en Wi-Fi passe un tiers du temps au-dessus de la borne, par hausses de
  1 à 2,7 s, et il ne va pas plus mal à 240 ; un N95 en Wi-Fi, lui, s'y noie.
  Rien ne les distingue dans la première seconde. D'où les règles :
  - si le lien, à la fréquence du client, est resté au-dessus de la borne plus
    de 500 ms (dans les images gardées avant l'essai, ou dans la dernière
    minute), l'essai tient pendant sa plus longue hausse × 1,2, entre 3 et
    4 s. Il
    est jugé sur le quartile bas de capture → peinte, qu'une hausse ne déplace
    pas. Sinon, 500 ms comme avant ;
  - un essai élargi qui échoue, rendu ou par le filet, compte comme un filet et
    rend le suivant étroit ;
  - un filet étroit qui saute sur une hausse, ou un filet élargi sous un palier
    déjà gardé, laisse le suivant s'élargir.

  En simulation, le Mac garde ainsi 240 en moins de 10 s, et le N95 paie un
  essai élargi, puis un étroit, avant d'attendre 16 min.

  Le plancher de 3 s vient du banc du Mac (§8t.6 du banc). Les hausses vues
  avant l'essai, 1,2 à 2,9 s de filet, ne sont pas les plus longues : à 240,
  le lien du Mac monte jusqu'à 2,8 s, et le palier gardé a sauté dans 5 passes
  sur 7. Le N95 n'y perd rien : sa file de décodeur le rend en 0,6 à 1,3 s, ou
  son essai est jugé à 2,9 s.

  Rejoué hors ligne sur les passes `host-guarded` de la nuit, le filet de
  500 ms sautait sur les 12 passes du Mac, l'élargi sur 2.
  Les 500 ms de tenue datent du banc du 02/10. Sans elles, le Mac en Wi-Fi
  perdait chaque 240 gardé en 0,1 s, sur des pointes qu'il a aussi à 120 (p99
  de 140 à 240 ms). Pourtant, `host-guarded` lui montrait à 240 des images
  plus jeunes de 15 ms. Un client qui se noie reste au-dessus et redescend
  en moins d'une seconde.
- Ce que l'appareil retient (décision de Bruno, 02/10). Chaque essai qui
  échoue laisse une pointe : sur le N95, le p99 passe de 211 à 350 ms à cause
  des seuls essais (banc §8t.7).
  - Un palier est retenu quand il a coûté deux fois dans un stream sans jamais
    y être gardé. Coûter, c'est un essai repris par le filet, ou rendu sur une
    image plus vieille d'une demi-période du client ou sur moins de 90 %
    d'images peintes.
  - Il est retenu pour cet hôte, à cette résolution, dans le `localStorage` de
    l'appareil (`mw_autostep_failed`).
  - Les streams suivants ne l'essaient pas pendant leurs 16 premières minutes.
  - Un palier gardé est oublié ; l'oubli vient aussi après 7 jours, ou plus
    tôt pour un lien qui change en cours de stream.
  - Un essai rendu sans gain ne coûte rien et n'est pas retenu.
  - Le banc efface la mémoire à chaque passe (`pass.py`).
- Un gouverneur de décodage qui demande moins d'images (`clientfpscap`)
  l'emporte : le palier est lâché, et aucun essai n'a lieu tant qu'il plafonne.
- Il ne tourne que là où il mesure : hôte natif, fréquence laissée à l'Auto,
  canvas qui déchire (Chromium sur un ordinateur, son défaut), décodage et
  dessin sur le fil principal, sans pacer. Un client en vsync garde sa
  fréquence ; l'émission calée (§33.8) en reste la piste.
- Simulé (Vitest) : 120 → 240 demandé à 5 s et gardé vers 8 s ;
  60 → 120 → 240 en ~11 s, le second palier étant demandé dès que le premier
  est gardé.

**La clé.** Allumée par défaut depuis la porte UA (02/10) :
`localStorage.mw_autostep = '0'` la coupe. Une TV la garde coupée, sauf
`'1'` : son « peinte » est la remise de l'image au `<video>`, pas ce que montre
son écran, et une TV qui en montre 30 garderait 60. Le banc pose `'0'` pour ses
modes de référence (`pass.py`). `window.mwCadenceStepper` donne au banc l'état
et les décisions (`events`).

**Le banc** (UA.3, nuit du 01 au 02/10, banc §8t) :
- passé sur l'UM790Pro en Ethernet. 240 y est gardé en 4 à 10 s, au niveau
  de `host-guarded` (−1 à −14 ms contre l'Auto) ;
- passé aussi sur le N95 en Wi-Fi (jamais pire, chaque essai rendu en 2,6 s
  au plus), sur le client local (−8 ms) et sur le jeu à 50 i/s (aucun
  essai) ;
- échoué sur le Mac en Wi-Fi. Son lien a déjà, à 120, des hausses de 1 à 2,7 s,
  un tiers du temps, que le filet de 500 ms prend pour un échec du palier. À
  240, son lien ne va pas plus mal qu'à 120, alors que le N95 s'y noie.

**La porte** (Bruno, 02/10) : l'« Auto » détecté par défaut, avec la sûreté
des deux filets. Reste un filet jugé contre ce que le lien fait à la fréquence
du client (UA.3 bis), à mesurer sur le Mac et le N95 avant le push.

## 34. Le flux commun des invités (plan du 28/09 au 01/10/2026)

Plan « flux commun des invités » (`non-je-veux-que-jazzy-sutherland.md`).
Hôte natif seulement : Sunshine, Wolf et MultiSeat gardent une session et un
encodeur par invité, leurs chemins n'ont pas changé.

### 34.1 Pourquoi

Chaque invité d'un hôte natif avait son worker complet : sa capture, sa
conversion, son encodeur. Les encodages des invités et celui du owner voyaient
la même image au même instant et passaient ensemble, et celui du owner attendait
derrière les autres. Sur la RTX, trois invités doublaient le total hôte du owner
(3,58/4,07 → 6,78/10,69 ms au p50/p99, banc §8q.1). Le owner garde maintenant sa
session telle quelle, et ses invités regardent un seul flux, encodé une fois :
**deux encodages au plus**, quel que soit le nombre d'invités.

### 34.2 Un producteur, des abonnés

```
worker « feed » : capture ─ conversion ─ HEVC (IR) ─────────► FeedPublisher ─┬─► pipe ─► worker invité 2 ─► WebRTC
                                         ▲ idr, link, evict                    ├─► pipe ─► worker invité 3 ─► …
                                         └─────────────────────────────────────┴─► pipe ─► worker invité 4 ─► …
worker owner (slots 0/1) : inchangé, son propre encodeur
```

- **Le producteur** est un `--stream-worker` de rôle `feed` : un
  `NativeMediaEngine` sans session, sans relais ni signalisation, qui publie
  ses images au lieu de les donner à un pair. Son profil est fixe :
  - 60 i/s, SDR, 4:2:0 ;
  - la hauteur choisie par le owner et la forme de l'écran, sans agrandissement ;
  - **l'intra-refresh sur la route que la machine choisit pour ce GPU**, comme
    le stream du owner le demande. La route n'est jamais changée pour lui :
    le flux a d'abord exigé l'intra-refresh (`intraRefreshRequired`, S1), et
    sur l'Arc, cela le mettait sur oneVPL en D3D11 à côté du D3D12 VE du owner.
    Au banc S9, avec un seul invité, le p99 du owner est passé de 7,8 à 27 ms,
    et le flux encodait en 12 ms au lieu de 4. Là où la route n'a pas de vague
    (le D3D12 VE de l'Arc et du N95), le flux se répare par images clés,
    regroupées et rationnées (§34.4). `intraRefreshRequired` reste une clé de
    banc (`intra=2`) ;
  - un plancher du gouverneur à 60 % (`governorFloorPercent`, 20 ailleurs).
- **Chaque invité garde son worker** : ses ports, son chemin WS, son échelon de
  transport. Seule sa source vidéo change : `videoSource = External` fait une
  session sans capture ni encodeur. Elle garde l'entrée, l'audio (sa propre
  boucle WASAPI), `inputPolicy` et les manettes. Le rectangle du bureau
  (`SessionInfo::desktop*`) et le curseur arrivent par le pipe.
- **Pourquoi pas un seul processus à N relais** : `Session` et
  `SignalingServer` ne portent qu'un relais chacun, un échelon qui échoue ne
  relance que son invité, et un crash de libdatachannel emporterait tous les
  invités.

### 34.3 Le pipe

- `QLocalServer` : un pipe nommé sous Windows, nommé `mw-feed-<édition>-<nonce>`
  et réservé à l'utilisateur. Le premier message est `hello`, avec un jeton de
  128 bits comparé en temps constant. Rien ne part avant ; après 3 s de silence,
  la connexion est fermée.
- Les abonnés sont lancés dans le même contexte que le flux
  (`StreamWorkerHost::startAs`) : le SYSTEM du service, une tâche élevée ou un
  simple enfant. C'est ce qui permet au pipe de rester réservé à l'utilisateur.
- **Trame** (`FeedWire.h`) : un en-tête de 64 octets, puis le flux. L'en-tête
  porte le numéro de l'image, le drapeau clé et ses horodatages (`presentUs`,
  `capturedUs`, `submittedUs`, `convertedUs`, `encodedUs`). L'horloge est celle
  de la machine, commune aux deux processus : l'E2E et les étapes de l'overlay
  d'un invité restent justes, et le passage par le pipe compte dans « Queue ».
- **Aucun invité ne fait attendre la capture.** `publishFrame` ne fait que
  copier dans une `FreshestQueue` par abonné, où un delta peut être remplacé et
  une image clé jamais. L'écriture a son propre fil, et elle s'arrête pour un
  abonné dont le pipe retient plus de 4 Mo.
- **Messages de contrôle** :
  - du flux vers les invités : `info` (le `SessionInfo` du flux : codec,
    taille, intra-refresh, encodeur ; `FeedInfo`), `cursor`, `displayFormat`,
    `codec`, `bye` ;
  - des invités vers le flux : `idr`, `link`, `evict`.
- L'abonné ne livre rien à son navigateur avant une image clé. Si le flux
  disparaît, il attend 10 s qu'il revienne sous le même nom.

### 34.4 L'arbitrage (`FeedArbiter`)

- **Images clés** : les demandes des invités (une arrivée, un décodeur perdu)
  sont regroupées sur 250 ms, avec une par seconde au plus pour tous. Avec
  intra-refresh, une image clé ne fait que hâter la réparation ; sans, elle
  est la réparation. Le relais d'un invité ne traverse les pertes
  (`ridingOutLoss`) que si l'`info` du flux annonce l'intra-refresh.
- **Débit** : les rapports de lien sont fusionnés au pire (plus forte montée
  d'OWD, plus fort taux de trous), un `reportLink` toutes les 500 ms au plus.
  Le débit suit donc l'invité le plus lent, sans descendre sous 60 % de la
  cible. En dessous, seul cet invité saute des images, réparées par la vague
  ou, sans elle, par l'image clé rationnée.
- **Cadence fixe** : un abonné ne commande pas le flux. `clientfpscap`,
  `clientrefresh`, `framefloor`, la file de décodage et la grille vsync des
  invités sont ignorés.
- L'invalidation de référence ne s'applique pas à un flux commun
  (`ref_invalidation = false` dans l'`info`).
- **Pointeur** : le flux dessine le pointeur de l'hôte dans l'image commune,
  quel que soit le mode de chaque invité. Une session qui dessine le pointeur
  le ramène au milieu de l'écran streamé s'il est ailleurs au démarrage
  (règle du 18/09, réduite au démarrage le 05/10, §29.5), pour un spectateur
  qui ne le voit que dans l'image. Le flux ne le ramène que tant qu'un invité ne voit le pointeur nulle part ailleurs
  (pointeur verrouillé, trackpad d'un téléphone).
  - La page le dit à son worker (`cursormode`), qui le redit au flux, et encore
    à chaque relance du flux.
  - Un invité en mode bureau a son propre pointeur : celui de l'hôte reste
    libre pour le owner ou pour la personne devant l'hôte.
  - Avant le correctif (test manuel de S9, 01/10), le flux le ramenait
    toujours. Un invité en mode bureau tirait donc la souris de l'hôte hors de
    ses autres écrans.

### 34.5 La vie du flux (`SharedFeed`, côté serveur)

- **Lancement et arrêt** : le flux démarre au premier invité et s'arrête 10 s
  après le départ du dernier. Un invité qui revient plus tôt ne coûte rien.
- **Partage** : deux invités partagent un flux quand il montre le même écran
  (même hôte, même appli) à la même hauteur. Le codec n'est pas une raison d'en
  faire deux.
- **Mort du worker** : il est relancé sous le même pipe après 250 ms, puis
  500 ms, etc. Ses invités l'attendent et demandent une image clé. Au troisième
  échec d'affilée, ce partage revient aux encodeurs par invité : les invités
  quittent et rejoignent. Un flux qui a tenu une minute remet le compte à zéro.
- **Hauteur** : le owner choisit l'image des invités (720, 1080 ou 1440p,
  1080p par défaut), une fois pour toute la fenêtre de partage. Le choix est
  gardé dans `share.json` et exposé par `POST /api/share/feed`.
  - Le flux est reconstruit sous le même pipe. La largeur suit la forme, et le
    débit suit le nombre de pixels.
  - Les invités attendent comme après une mort, et leurs décodeurs prennent la
    nouvelle taille à l'image clé.
  - Un invité qui arrive pendant une relance attend avec les autres.
- **H.264** : la page d'un invité teste le HEVC avant de rejoindre
  (`hevcClientDecodes`). S'il ne le décode pas, tout le flux passe en H.264
  jusqu'à son arrêt, sans jamais faire deux flux.
  - Le worker du flux prévient ses invités (`codec`), puis s'arrête.
  - Leurs pages reviennent en H.264 par le repli de codec existant (avis
    `feedcodec`).
- **Interrupteur** : `MW_SHARED_FEED=0`, ou `shared_feed_enabled: false` dans
  `settings.json`, et chaque invité encode de nouveau seul, comme avant.
  Windows seulement pour l'instant : la session `External` n'existe pas encore
  sous Linux.

### 34.6 Les manettes des invités

Sur un hôte natif, chaque worker a sa propre table de quatre pads ViGEm. Le
décalage par slot de GameStream (`gamepadOffset`) n'y séparait donc rien, et
faisait perdre le deuxième pad de l'invité du slot 4. Le moteur garde
maintenant la numérotation du navigateur (`padNumber`) : la vibration revient
au bon numéro, et le masque des manettes reste cohérent. Sunshine décale
toujours (`MoonlightShim`).

### 34.7 L'écran virtuel allumé par un invité

Un invité qui ouvre à froid une invitation sur « MoonlightWeb Virtual
Display », sans stream du owner, l'allume (`VirtualDisplayJob::activateIfOff`).
L'écran est fait à la taille de l'image des invités, avant que le flux ne le
cherche. S'il est déjà allumé (owner, autre invité, grâce en cours), il reste
tel quel : un nouveau mode l'enlèverait à ceux qui le regardent.

### 34.8 Mesuré (banc §8q.3-§8q.5)

- **RTX, trois invités** : total hôte du owner 6,78/10,69 → **3,84/6,19 ms**
  (p50/p99), sessions NVENC 4 → 2, moteur d'encodage 45 → 29 %. C'est la
  ligne « un invité » de S0.
- **Arc et iGPU AMD** payaient peu en S0 et paient autant ou moins : Arc
  +1,2 ms de p99 à trois invités (S0 +1,6), AMD +1,3 (S0 +2,4), p50 inchangé.
- **Cas durs** : le flux tué revient en 1,3 s ; un changement de mode de
  l'écran fait une reconstruction ; un invité seul qui part et revient ne
  relance rien ; un invité sans HEVC fait une bascule. Un invité bridé à
  3 Mb/s tombe seul à 28 i/s peintes, pendant que les autres gardent 61 et que
  le flux s'arrête à son plancher de 6 Mb/s.
- **N95** (quatre cœurs, Wi-Fi) : un invité le met toujours à genoux, puisque
  le owner et le flux font deux captures et deux encodages comme avant. Le
  deuxième invité rejoint désormais (impossible en S0), le troisième non.

## 35. L'écran virtuel sous Linux, par compositeur (01/10/2026)

L'écran virtuel Linux passait par le portail (source VIRTUAL, GNOME 46 et après,
§32) : à 60 Hz, une fenêtre « Partager l'écran » à valider sur l'hôte au premier
stream, ni KDE ni GNOME avant 46. Le chapitre C du plan « Idées Punktfunk » le
fait demander au compositeur lui-même. Mesures : banc §8s.

### 35.1 Les routes

- **GNOME 42 et après** : l'API D-Bus de Mutter (`org.gnome.Mutter.ScreenCast`,
  version 4), par sd-bus (`MutterScreenCast.cpp`). `RecordVirtual` crée l'écran,
  `RecordMonitor` filme un écran qui existe.
  - Ni fenêtre ni jeton : Mutter ne regarde pas l'appelant.
  - Pas de session RemoteDesktop : les entrées restent par uinput.
  - Mutter ferme la session quand l'écran filmé part ou que le bureau se
    verrouille ; la capture le lit (`Closed`) et le stream repart.
- **Repli, le portail** : quand Mutter refuse net (méthode inconnue, accès
  refusé). La clé de banc `mutter=0` le force.
- **KDE Plasma 6** : son portail n'a pas de source VIRTUAL (6.3). La sortie
  virtuelle de KWin (`stream_virtual_output`, `KwinVirtualOutput.cpp`,
  libwayland chargée au premier usage) est demandée par l'auxiliaire sans
  capacités de §32, dont le binaire porte le droit de KWin (`.desktop` caché,
  `X-KDE-Wayland-Interfaces`).
- **X11** : aucune route, la carte n'est pas proposée.

### 35.2 Taille, fréquence, place

- **Taille** : celle que le stream demande, au pixel ; « Match my screen » fait
  celle du client (un téléphone en 1170×2532 compris).
- **Fréquence** : l'écran prend le `maxFramerate` que la capture négocie (GNOME
  42, 46 et 48 ; `modes` n'y change rien, Mutter 50 le lirait). Il est créé à
  240 Hz comme sous Windows (§33.9), le stream gardant la cadence du client. À
  60 Hz servi à 60 i/s, 40 % des images manquaient ; à 240 Hz, toutes passent.
- **Principal**, sous GNOME : l'écran du owner le devient par `DisplayConfig`,
  en configuration temporaire. Les écrans physiques restent allumés, à sa
  droite : en éteindre un a fait planter gnome-shell 46 (le M27Q de la GTX).
  Mutter remet la disposition quand l'écran part. Pas sous KDE.

### 35.3 Un seul écran pour tous les streams (GNOME)

- Un registre et un verrou dans `XDG_RUNTIME_DIR` (`SharedMonitor.h`). Le owner
  crée l'écran et l'inscrit ; un invité le filme tel quel. Un invité seul en
  crée un, et passe sur celui du owner quand il arrive. L'écran part avec son
  stream ; le premier qui revient le recrée.
- Créations et retraits un à la fois, tenus jusqu'à ce que la disposition ne
  bouge plus : Punktfunk a vu gnome-shell planter sur des reconstructions
  concurrentes.
- Le pointeur est recalé quand Mutter change ses écrans (`MonitorsChanged`). La
  veille des modes KMS ne s'applique pas à un écran sans CRTC.
- Sous KDE, chaque stream garde sa propre sortie.

### 35.4 Le pointeur

- Jusqu'à GNOME 47, Mutter recopie la vue de l'écran virtuel, pointeur compris,
  dans ses images DMA-BUF, quel que soit le mode demandé. En métadonnées, un
  mouvement seul n'apporte souvent qu'une position, sans image : le pointeur de
  l'image restait figé, puis sautait avec ce qui se redessinait. Il clignotait
  sous une main qui bouge (97 images sur 450 sans lui).
- Avant GNOME 48, il est donc demandé dans l'image (`cursor-mode` 1 ; portail
  `cursor_mode` 2). Chaque mouvement est une image, et l'hôte dit au client de
  n'en dessiner aucun. Il suit la latence de l'image.
- À partir de GNOME 48 : en métadonnées, et le client le dessine.

### 35.5 Les invités

Un invité d'un hôte Linux encode seul : le flux commun (§34) est réservé à
Windows. Sa page demande maintenant `ride_out_loss` comme celle du owner, et son
flux passe en intra-refresh : une perte ne lui coûte plus d'image clé. Avant,
il en demandait 17 et 22 en 24 et 42 s.

### 35.6 Limites

- KDE : un invité garde sa propre sortie (`stream_output` de KWin non utilisé) ;
  60 Hz avant KWin 6.6 ; pas d'écran principal.
- GNOME 49 et après : non mesuré.
- gamescope : §35.7.

### 35.7 Une app sur son propre écran : gamescope (chapitre G)

Une carte de l'hôte Linux ouvre une app dans un gamescope sans écran
(`--backend headless`), à la taille et à la cadence du client, à côté du
bureau : ni écran ajouté ni disposition touchée, le clavier et la souris du
bureau à part. Aucune session de bureau n'est nécessaire : une session X11, ou
pas de session du tout, a la carte aussi. Mesures : banc §8s.13, §8s.14 et
§8s.15 (G6, le vrai Steam : Big Picture, un jeu, owner et invité, 60 à 240 Hz,
téléphone tenu droit, X11).

- **La taille** : la page traite ces cartes comme l'écran virtuel. Chaque
  choix de résolution y nomme une taille exacte : un téléphone a l'écran de sa
  forme tenue en paysage, une taille personnalisée est prise telle quelle
  (`App.isMadeForStream`, `7b860195`). L'hôte la prend telle quelle
  (`isMadeForStreamKey`). Tourner l'appareil ne relance rien : la session garde
  sa taille de départ.

- **Les cartes** :
  - « Steam Big Picture », quand un gamescope 3.16.22 ou plus récent (avant,
    interblocage avec PipeWire 1.6) et un Steam utilisé sont là ;
  - une carte par app de l'owner (`gamescope_apps` : nom et commande, lancée
    par `/bin/sh`), quand gamescope est là. Les commandes s'écrivent depuis
    l'hôte seulement ; elles tournent avec l'utilisateur de l'hôte.
- **gamescope de la distribution** (décision du 01/10) :
  - le binaire vient de `MW_GAMESCOPE_BIN`, du PATH, de `~/.local/bin` ou de
    `/usr/games` ;
  - il est accepté s'il a 3.16.22 ou plus (Fedora 43+, Arch, Bazzite, SteamOS,
    Debian 13 backports, Ubuntu 26.10+) ;
  - Ubuntu 24.04 n'en a pas, et la 26.04 a la 3.16.20 : la carte n'y apparaît
    pas.
- **La session** (`GamescopeSession.{h,cpp}`) :
  - unité utilisateur transitoire (`systemd-run --user`), sans `DISPLAY` ni
    `WAYLAND_DISPLAY`, le dossier de gamescope en tête du PATH
    (`gamescopereaper`) ;
  - une enveloppe d'une ligne relaie le socket EIS et le display X que gamescope
    donne à son app ; le nœud PipeWire vient de son journal ;
  - registre et verrou dans `XDG_RUNTIME_DIR`. Le stream suivant retrouve la
    session, à sa taille d'origine, cadrée comme un invité filme l'écran du
    owner. L'owner et les invités partagent la session ;
  - minuterie de 10 min, réarmée chaque minute par chaque stream : la session
    s'arrête 10 min après le dernier, même s'il est mort sans prévenir.
- **Steam** : un seul par utilisateur. La carte démarre le Steam en service,
  sinon celui connecté en dernier (paquet, snap ou Flatpak).
  - Un Steam ouvert sur le bureau est prié de quitter, par le relais de son
    runtime (`steam-runtime-steam-remote -shutdown`, son HOME). La demande est
    refusée si un jeu y tourne, ou s'il n'a pas quitté en 20 s (le navigateur
    abandonne à 25).
  - À l'arrêt de l'unité, `ExecStop` fait quitter Steam proprement, puis
    `ExecStopPost` le rouvre sur le bureau s'il y était.
- **Capture** (`PortalCapture::setGamescope`) :
  - le nœud de gamescope sur le PipeWire de la session, sans portail ;
  - son flux se met en pause quand gamescope part, sans erreur : la capture
    surveille donc son PID. L'app quittée termine le stream avec une phrase.
- **Entrées** (`EiInput`) : libei, chargée au premier usage, en émetteur sur le
  socket EIS de gamescope (clavier, souris relative et absolue, molette,
  boutons). gamescope ne lit aucun périphérique sans écran ; les manettes
  restent en uinput, et les jeux les lisent directement. Le texte est tapé sur
  une disposition US, celle de gamescope.
- **Pointeur** (`XFixesCursor`) : hors de l'image de gamescope. Il est relu sur
  son Xwayland : la forme par XFixes, la position par `XQueryPointer`. Il est
  visible quand sa forme a de l'encre, et le client le dessine. Le gestionnaire
  d'erreurs d'Xlib de Qt terminait le processus quand cet Xwayland partait ; un
  gestionnaire chaîné et une sortie par connexion (libX11 1.7 et plus) l'en
  empêchent.
- **Limites** :
  - une fenêtre plus petite que l'écran est mise à l'échelle par gamescope :
    l'absolu et le pointeur sont alors décalés ; Big Picture et un jeu plein
    écran sont justes ;
  - clavier US (amont) ;
  - pas de HDR ;
  - pas de changement de taille en cours de session ;
  - l'app choisit son GPU elle-même : sur une machine hybride, une app Vulkan
    peut prendre la carte dédiée quand gamescope compose sur l'autre, et chaque
    image traverse alors d'un GPU à l'autre (vkcube sur la GTX 1050 de
    l'UM790Pro : 2 i/s en 720p à 240 Hz, 214 i/s sur le 780M) ;
  - pas de son quand l'hôte n'a aucune sortie audio réelle (« Dummy Output »),
    comme pour le bureau.

**Concrètement, pour l'utilisateur** : sous Linux Wayland, la carte « écran
virtuel » montre un bureau à la taille de l'appareil qui regarde, à 240 Hz, sans
fenêtre à valider sur l'hôte, même sur un mini-PC sans écran. Sous GNOME, il
devient l'écran principal le temps du stream, et un invité voit le même bureau
que le owner. Sous KDE Plasma 6 aussi, à 60 Hz.

Avec un gamescope récent, la carte « Steam Big Picture » ouvre Steam en mode
console sur son propre écran, à la taille de la TV ou du téléphone, sans toucher
au bureau. L'hôte peut servir à quelqu'un d'autre pendant ce temps, même en
session X11. Les apps de l'owner, déclarées dans la page d'administration, font
de même.
- **Sunshine et Wolf** : inchangés (un encodeur par invité, trois qualités).