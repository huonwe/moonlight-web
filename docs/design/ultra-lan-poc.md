# « Ultra » en LAN — le POC d'un codec intra sur GPU

> Une question, chiffres à l'appui : un mode réservé au LAN, où chaque image est
> compressée seule sur le GPU de l'hôte, décodée sur le GPU du navigateur et
> présentée à l'arrivée, bat-il nettement le HEVC d'aujourd'hui en latence de
> bout en bout, à qualité acceptable et à débit raisonnable ? Et où se place-t-il
> face à PyroWave, que Steam Remote Play embarque en bêta depuis le 21/09/2026 ?
>
> Un POC : un « non » chiffré est un résultat. Le plan détaillé (phases, commits,
> efforts) vit hors du dépôt ; ce document en garde les hypothèses, le modèle,
> les portes et les résultats.

## 1. Où il se fait, et ce qui protège le produit

- Sur `main`, comme le reste du travail (décision de Bruno du 02/10/2026).
- Les labos vivent hors du produit : `tools/` (côté hôte), `scripts/bench/ultra/`
  (côté navigateur), `docs/`.
- Le code du POC dans le produit reste derrière deux clés cachées :
  `MW_NATIVE_TUNING=ultra=…` à l'hôte, `mw_ultra=1` au client. Ultra n'est jamais un
  défaut. Un refus ou un échec retombe sur le HEVC d'aujourd'hui, avec une ligne de log.
- Hôte natif Windows, route D3D12 seulement. Sunshine, Apollo, Wolf, MultiSeat et
  les hôtes Linux et macOS ne changent pas.
- Un « oui » à la dernière porte ouvre un plan de mise en produit à part. Il
  couvrirait aussi macOS (Metal, d'après le portage Metal de l'amont) et Linux
  (Vulkan, les shaders de l'amont presque tels quels). Un « non » retire le code
  produit du POC par un commit et garde les labos et le rapport.
- Code tiers : PyroWave (MIT) est porté depuis l'amont, avec sa notice, sous
  `third_party/pyrowave`, version épinglée. Les dérivés GPL (Vibepollo, clients
  Moonlight modifiés) ne sont pas lus : ni leur source, ni leurs shaders.
- Corpus : les images de jeux restent hors dépôt. Seuls les outils et les chiffres
  y entrent.

## 2. Les hypothèses du départ, relues contre les mesures

- **Le codec ne fait pas le gros de la latence partout.** Sur NVIDIA, il pèse
  ~3 ms (encodage 1,9, décodage 1,1) sur un clic → drapeau de ~35 ms à 60 Hz
  (banc §8l, §6c). Sur l'Arc, il pèse 5,4 ms en oneVPL ; sur le N95 7 à 11, sur
  l'iGPU AMD 9 à 17. Le reste vient de la cadence de capture et de la
  présentation chez le client.
- **Une image ne se décode qu'à son dernier octet.** Les formats de texture
  perdent sur le fil ce qu'ils gagnent au décodage : BC1 / ETC2 (4 bpp) font
  8,3 ms par image 1080p sur 1 GbE, contre 0,3 ms en HEVC à 20 Mbit/s. Et aucun
  n'existe à la fois sous Windows et sur mobile : Chrome sous Windows n'expose
  que BC. Ils sont mesurés hors ligne, sans encodeur temps réel.
- **Le transport est la première inconnue.** Le média passe par un DataChannel
  WebRTC. usrsctp démarre lentement après chaque pause (burst max 10, bridage par
  la fenêtre), le débit de Chrome en DataChannel n'a pas de mesure publiée, et le
  produit plafonne à 150 Mbit/s. D'où un labo transport avant tout codec.
- **La présentation peut tout reprendre.** WebGPU coûte 4 ms de rendu sous
  Windows et 10 sous macOS, contre 0,2 à 0,6 ms pour Canvas2D et WebGL
  `desynchronized` (03/09). Les présentateurs sont départagés au photon avant
  d'écrire un décodeur.
- **Le candidat principal** est un codec intra par ondelettes, porté de PyroWave
  (MIT) : CDF 9/7 sur 5 niveaux, blocs de 32×32 coefficients décodables seuls,
  sans codage entropique. Moins de 0,1 ms d'encodage et de décodage en 1080p sur
  un GPU récent ; 170 Mbit/s en 1080p60 pour une image jugée sans défaut. Aucun
  portage navigateur n'existe : le décodeur est à écrire, en WGSL ou en shaders
  WebGL2 selon le présentateur qui gagne.

## 3. Le modèle de latence — la boussole

Latence d'une image = attente de la capture + acquisition + conversion +
**encodage** + **sérialisation** (taille ÷ débit du lien) + pile réseau et
navigateur + **décodage** + attente de présentation + dalle. Le codec agit sur
les termes en gras, et sur la présentation si son décodeur impose une API de rendu.

Sérialisation d'une image 1080p au débit ligne, sans en-têtes (borne basse) :

| 1080p | Kio / image | Mbit/s à 60 i/s | 1 GbE | Wi-Fi ~400 Mbit/s réels |
|---|---|---|---|---|
| HEVC 20 Mbit/s | 41 | 20 | 0,33 ms | 0,8 |
| PyroWave, seuil subjectif (1,37 bpp) | 346 | 170 | 2,8 | 7,1 |
| ASTC 6×6 (3,56 bpp) | 900 | 442 | 7,4 | 18,4 |
| BC1 / ETC2 RGB (4 bpp) | 1 013 | 498 | 8,3 | 20,7 |

Les paquets SCTP de 1 280 octets ajoutent ~11 % ; un DataChannel qui ne tient
que 80 % du lien, 25 % de plus : PyroWave au seuil passe à 3,2-3,8 ms. Le labo
transport remplace ces facteurs par la mesure.

Segment codec prédit (encodage + sérialisation + décodage, 1080p60, 1 GbE, client
de bureau) :

| Hôte | HEVC aujourd'hui | Ultra PyroWave 170 Mbit/s |
|---|---|---|
| RTX (NVENC P1) | ≈ 3,4 ms | ≈ 3,6-4,2 |
| Arc (D3D12 VE) | ≈ 3,8 | ≈ 4,1-4,7 |
| N95 (oneVPL) | ≈ 10,5 | ≈ 6,5 |
| iGPU AMD (AMF) | ≈ 10,5-18,5 | ≈ 6,5 |

Lecture honnête : légère perte sur NVIDIA et sur l'Arc, gain de 4 à 12 ms sur
les iGPU. Côté client, un décodeur matériel mobile lent ferait pencher vers
Ultra, un présentateur plus lent que Canvas2D vers HEVC. Hors codec, la cadence
de capture coûte une demi-période (8,3 ms à 60 Hz, 2,1 à 240) : sur NVIDIA, elle
pèse plus que le codec. La barre à battre est donc « HEVC réglé Ultra »
(120 i/s, écran virtuel à 120 ou 240 Hz, P1, tearing, « Auto » avec détection),
pas le HEVC par défaut. Un codec intra paie chaque image : son débit double avec
la cadence, celui du HEVC beaucoup moins.

## 4. Le socle hérité du plan D3D12 (relevé du 02/10/2026)

- **Chaîne par GPU** (§32.19) : Intel en D3D12 (conversion D3D12 sur la file
  DIRECT, D3D12 Video Encode, débit maison, deux images en vol sur un GPU à
  mémoire propre) ; NVIDIA et AMD en D3D11 (NVENC, AMF), D3D12 n'y fait pas mieux
  (G4). Tout refus ou panne revient à D3D11 sans couper le stream.
- **Files** (G1, banc §8n.2) : `CreatorID` propre et priorité `GLOBAL_REALTIME`
  quand le processus tient REALTIME, repli en HIGH. Sur la file COMPUTE, la v1
  était préemptée par le jeu sur l'Arc et affamée sur l'iGPU AMD (117 / 249 ms) :
  un encodeur en shaders partage les unités du jeu, et se mesure sous RE9 et sous
  charge, sur cette politique de files.
- **Temps GPU** : `gputiming=1` encadre chaque soumission d'encodage de deux
  horodatages (`gpu_encode_us`). L'Arc les écrit avant que l'image soit codée et
  ne rapporte rien. La poignée de main DDA reste `ddasync=gpu`.
- **Cadence** : l'« Auto » avec détection (§33.10 de `native-capture-encoder.md`)
  porte déjà le stream au-dessus de la fréquence du client quand l'image y
  rajeunit. Il fait partie de la barre.

## 5. Les phases et les portes

| Phase | Ce qu'elle fait | Porte |
|---|---|---|
| UA | « Auto » avec détection, dans le produit | porte UA (Bruno, 02/10 : par défaut, avec une sûreté) |
| U0 | Budget de latence : E2E par image, âge du contenu, « HEVC réglé Ultra », borne Steam, les deux TV | **U0** |
| U1 | Labo transport : DataChannel à haut débit, réglages usrsctp | **U1** |
| U2 | Labo hôte : corpus, qualité, formats de texture hors ligne, PyroWave de référence, portage HLSL, sous charge | — |
| U3 | Labo navigateurs : planchers natifs, capacités, présentateurs au photon, décodeur, sonde TV | **U2** |
| U4 | Intégration derrière les clés cachées | — |
| U5 | Banc comparatif HEVC / Ultra / Steam PyroWave | **U3** (verdict) |
| U6 | Clôture | — |

Chaque porte donne un rapport chiffré et une recommandation ; Bruno tranche.
Chacune peut arrêter le POC. Les deux portages lourds (encodeur HLSL, décodeur
navigateur) ne commencent qu'après U0 et U1.

- **U0** : on continue sur les couples hôte × client où le gain prédit, sous des
  hypothèses favorables à Ultra, vaut au moins 2 ms ou 20 % de l'E2E instrumentée.
  Aucun couple : le POC s'arrête, et les gains hors codec partent dans la liste
  du plan natif.
- **U1** : sur 1 GbE, Chrome de bureau, 60 i/s, images de 350 Kio, étalement +
  RTT min / 2 au p50 ≤ 1,08 × débit ligne + 1 ms et p99 ≤ + 4 ms ; aucune erreur
  de réception UDP ; transport ≤ 10 % du fil principal du client. Le plafond
  mesuré devient l'enveloppe du codec. Sous 170 Mbit/s en filaire : plan B RTP
  sondé, puis arrêt ou décision de Bruno.
- **U2** : qualité ≥ HEVC P1 à 20 Mbit/s sur le contenu animé, à ≤ 200 Mbit/s ;
  lisibilité du texte fixe jugée par Bruno. Encodage p99 ≤ 1 ms sur la RTX,
  ≤ 2 ms sur l'Arc, ≤ 3 ms sur les iGPU, sous charge. Décodage p50 ≤ 1 ms sur un
  bureau, ≤ 3 ms sur M1 et iPhone ; présentateur sans vsync de plus.
- **U3** : E2E par image, médiane ≤ « HEVC réglé Ultra » − 2 ms (ou − 20 %) avec
  un p99 pas pire ; clic → drapeau pas pire ; qualité acceptée à l'œil par Bruno ;
  30 min sans gel ; 1 % de pertes sans gel de plus d'une image. Issues : « oui »,
  « oui limité » (par exemple hôtes à iGPU et clients filaires), « non ».
- Les portes se jugent en Ethernet 1 GbE ; le Wi-Fi est mesuré et rapporté,
  jamais bloquant.
- **Les TV** (décision du 02/10) : budget en U0, sonde du décodeur Ultra en U3.
  Elles deviennent une cible de U4 seulement si, en 720p, décodage + présentation
  tiennent sous 14 ms par image à plus de 30 images affichées par seconde. Elles
  ne bloquent aucune porte.

## 6. Résultats

*(rempli porte par porte)*

### 6.1 U0.2 — l'E2E par image (03/10/2026, `ea3d313c`)

Le POC a besoin d'une latence mesurée d'un bout à l'autre, image par image, et
non d'une somme d'étapes. La latence de l'overlay additionne des étapes, chacune
chronométrée sur sa propre horloge et moyennée sur sa propre fenêtre : une étape
que personne ne chronomètre n'y figure pas.

- **L'horloge commune existait** (plan 1, cadence de l'hôte) :
  - le pong porte les µs de l'hôte (`DataChannelRelay`, champ `host`) ;
  - `util/ClockEstimator.js` ajuste un décalage et une dérive sur les échanges
    proches du RTT le plus court ;
  - ses tests (dérive, RTT asymétrique, saut d'horloge, valeur aberrante) sont
    dans `ContentAgeProbe.test.js`.
- **Ce que U0.2 ajoute** : `stream/FrameLog.js`. Il met l'horodatage de chaque
  image (`backendTs`) sur l'horloge du client, avec sa propre estimation sur
  60 s, nourrie par le ping de 2 s. Il en tire l'âge de l'image à la fin de son
  dessin. On le retrouve :
  - dans le détail de latence, en ligne « Mesurée (hôte → dessin) » (moyenne et
    p99 sur 2 s). Elle est affichée, pas additionnée : c'est ce que la somme
    devrait lire, et l'écart entre les deux est une étape que personne ne
    chronomètre ;
  - dans la ligne `[perf]` (`e2e measured`) ;
  - dans un journal de toutes les images : un anneau de colonnes de
    16 384 images (une minute à 240 i/s), lu par CDP avec `mwFrameLog.csv()` ou
    `summary()`. `age.py run` le vide au départ et l'enregistre en
    `<tag>.frames.csv` à côté du JSON de la passe.
- **Où le trajet commence** :
  - hôte natif : à la présentation de l'image à l'écran, à la milliseconde. Il
    lit 0 à 2 ms de trop, parce que les deux moitiés de l'horodatage sont
    arrondies à la milliseconde ;
  - hôte GameStream : à l'arrivée de la première image du stream au backend.
    Sa capture, son encodage et son trajet jusqu'au backend manquent, du même
    montant sur chaque image.
- **Limites** :
  - un lien plus lent dans un sens que dans l'autre (la montée en Wi-Fi) fausse
    l'âge de la moitié de l'écart, sur chaque image ;
  - seul le décodage sur le fil principal est couvert, pas le worker (option) ;
  - AV1 n'a pas d'horodatage d'hôte sur ce chemin.
- **Vérifié** :
  - Vitest : 7 tests (âge, horloge pas encore prête, dérive de 50 ppm sur 3 min
    avec un ping toutes les 2 s, bouclage 32 bits, horodatage aberrant,
    anneau et CSV, résumé) ; suite complète 1192/1192.
  - En vrai stream (03/10, 08:37) : deux passes locales sur DualRTX, hôte Arc
    (D3D12 VE), HEVC 2560×1440 à 60 i/s, écran virtuel à 240 Hz, client sur
    l'iGPU AMD. Toutes les images dessinées ont un âge (1200 et 1199) :

    | Passe | Médiane | Moyenne | p99 | Somme des étapes (overlay) |
    |---|---|---|---|---|
    | r0 | 10,5 ms | 13,3 ms | 28,4 ms | 11,3 ms |
    | r1 | 10,2 ms | 12,8 ms | 27,2 ms | 11,7 ms |

    - Les médianes des étapes du journal (présent → arrivée 8,2-9,0 ms,
      décodage 0,6, attente 0,0, dessin 0,2) s'additionnent à 9,0-9,8 ms, sous
      la médiane de bout en bout : c'est cohérent.
    - La somme des étapes de l'overlay lit 1,1 à 2 ms sous la moyenne mesurée.
      Une partie de cet écart est l'arrondi de l'horodatage (0 à 2 ms) ;
      l'autre reste à attribuer.
    - L'horloge : 226-227 échanges (le ping de 2 s et ceux de la sonde d'âge),
      RTT minimal 0,3 ms, dérive estimée 0,4-0,9 ppm (0 en vrai : une seule
      machine), aucun saut. L'estimation sœur de la sonde d'âge, faite sur les
      mêmes pongs, tombe à +0,02 / +0,03 ms de l'horloge vraie.
    - Le contrôle croisé (09:50, deux passes de plus, `ua-u02d-*`) : la bande
      était illisible dans la matinée parce qu'une invite pare-feu de Windows,
      ouverte à 06:38 par le banc T7 pour un exe sans règle, restait posée en
      0,0 de l'écran capturé. Bruno l'a autorisée, et la bande se relit. Image
      par image, le journal et la colonne `capture` de la sonde d'âge
      concordent : −0,09 ms de médiane (p10 −0,19, p90 +0,01) sur 296 et 285
      images appariées. Même horodatage, même horloge : c'est le contrôle
      attendu.
    - **Ce que le contrôle a montré en plus : la sonde d'âge retarde les images
      qu'elle lit.** Elle en lit une sur quatre (`every 4`) ; ces images-là
      attendent 12,2 ms entre leur décodage et leur dessin, les autres 0,0.
      Médianes de bout en bout : 22,0 ms pour les images lues, 8,9 pour les
      autres. La sonde ne calcule ses âges que sur les images lues : ses
      chiffres absolus (`drawn`, `shown`, `capture`) portent ce retard, sur ce
      client du moins (iGPU AMD de DualRTX, Chrome). Les écarts entre modes,
      mesurés avec la même sonde, le portent des deux côtés. Le coût sur les
      autres clients (Mac, N95, UM790Pro) est à mesurer de la même façon,
      avec le journal.
    - Pièges de la matinée : l'instance dev écoute maintenant sur 8080/8443
      (et non plus 18080/18443), et une première passe a été arrêtée par
      Claude Code, faute de mémoire.

### 6.2 U0.2 bis — la sonde d'âge ne retarde plus ce qu'elle mesure (03/10/2026)

Le contrôle de U0.2 (§6.1) a montré que la sonde d'âge du contenu retardait le
dessin des images qu'elle lisait. Elle copiait la bande sur le fil principal,
avant le dessin de l'image. Elle passe maintenant l'image à un worker
(`bandReadWorker.js`, `b83f3dac`), qui fait la copie. Si le navigateur refuse
de transférer l'image, ou si le worker meurt, la sonde copie comme avant. Le
résumé de chaque passe dit par quel chemin la copie est passée (`reader`), et
ce que le fil principal a encore payé par lecture (`readCost`, affiché par
`age.py`). Avec `MW_BENCH_INLINE_READ=1`, une passe copie comme avant : c'est
ce qui a servi à mesurer l'avant et l'après (`659a017b`).

Mesure du 03/10 (12:43-13:00) : hôte Arc, stream 60 i/s, une passe par mode,
deux sur DualRTX. On compte l'attente entre le décodage d'une image lue et le
début de son dessin, donnée par le journal par image ; les autres images
attendent 0,0 à 0,1 ms.

| Client | Avant (fil principal) | Après (worker) | `capture` de la sonde, avant → après |
|---|---|---|---|
| DualRTX, client local (iGPU AMD) | 11,3 / 13,2 ms | 0,1 / 0,1 ms | 19,4 / 25,5 → 8,9 / 9,1 ms |
| N95, Wi-Fi | 7,3 ms | 0,2 ms | 49,8 → 34,8 ms |
| UM790Pro sous Windows, Ethernet | 4,0 ms | 0,1 ms | 16,9 → 12,1 ms |

- **Les âges du contenu mesurés jusqu'ici étaient trop hauts, à peu près de ce
  coût.** La sonde ne calcule ses âges que sur les images qu'elle lit, et
  chacune portait son retard : jusqu'à 12 ms sur le client local, 7 sur le N95,
  4 sur l'UM790Pro. Sur le Mac, ce coût n'a pas été mesuré.
- **Les écarts entre modes** (UA, §8t du banc) portent ce retard des deux
  côtés. Ils restent comparables, tant que le coût ne change pas d'un mode à
  l'autre. Une image lue de plus par seconde, à 240 i/s, aurait pu en ajouter :
  ce n'est pas vérifié pour les passes passées.
- La colonne `capture` de la sonde rejoint maintenant la médiane du journal
  par image (8,9 contre 9,2 ms en local) : la sonde ne mesure plus son propre
  retard.

### 6.3 U0.3 — les séries du 03/10/2026 (N95, UM790Pro, iPhone, Mac)

Hôte DualRTX, client le N95 (Chrome, Wi-Fi), de 15:57 à 16:50 : 20 passes,
chacune avec 30 s d'âge du contenu, le journal par image et 30 clics. Deux modes
alternés D U D U dans chaque case, l'écran virtuel rendu par le GPU de la case :

- **D, « HEVC par défaut »** : le produit d'aujourd'hui, Auto avec détection ;
- **U, « HEVC réglé Ultra »** : la barre (§2), 120 i/s demandés, écran virtuel
  à 240 Hz, tearing, détection ;
- **A** : U en AV1. Les cases AV1 alternent U A U A.

`pass.py --codec` vient de `69475ba6`. Médianes de deux passes par mode (l'âge
montré de A sur l'Arc, d'une seule : l'autre n'a rien pu lire) :

| Hôte | Mode | Âge montré | E2E par image | Clic → drapeau (clics mesurés) | Plus longue coupure du lien |
|---|---|---|---|---|---|
| RTX (NVENC) | D | 44,9 ms | 28,0 ms | 83,0 ms (53 / 60) | 0,8 s |
| RTX (NVENC) | U | 270 ms | 242 ms | 135 ms (22 / 60) | 8,8 s |
| Arc (D3D12 VE) | D | 58,2 ms | 39,9 ms | 88,6 ms (54 / 60) | 1,2 s |
| Arc (D3D12 VE) | U | 130 ms | 114 ms | 204 ms (12 / 60) | 4,4 s |
| iGPU AMD (AMF) | D | 51,7 ms | 31,3 ms | 93,9 ms (46 / 60) | 2,4 s |
| iGPU AMD (AMF) | U | 320 ms | 295 ms | 101 ms (17 / 60) | 14,8 s |
| Arc (oneVPL) | A | 448 ms | non mesuré | aucun (0 / 60) | 31 s |
| RTX (NVENC) | A | 671 ms | non mesuré | 185 ms (8 / 60) | 20,8 s |

- **Sur le N95 en Wi-Fi, demander 120 i/s fait décrocher le lien.** Le lien se
  coupe plusieurs secondes et le débit retombe de 7 à 5 Mbit/s. Le stream ne
  livre que 41 à 82 i/s, et plus d'un clic sur deux n'obtient pas de drapeau. Le même
  écart se répète sur les trois GPU, passe après passe : ce n'est pas un
  incident. Le mode par défaut, lui, reste à la cadence du client : sa détection a
  essayé 116 i/s et y a renoncé (la file du décodeur se remplissait, puis
  l'image arrivait trop tard).
- **La cause : la vidéo attend dans usrsctp, sur l'hôte.** Une série de plus
  (18:00, RTX, D U D U, `relaylog=1`, `scripts/bench/wifi/flagpath.py`) coupe
  le trajet de chaque image. Sur toutes les images de la minute des clics :

  | Mode | Réseau p50 / p90 | dont avant de quitter usrsctp, p50 / p90 | Décodage d'une image de drapeau |
  |---|---|---|---|
  | D | 18-22 / 43-67 ms | 12-13 / 35-54 ms | 2-3 ms |
  | U | 38-49 / 204-263 ms | 25-32 / 186-232 ms | 18-22 ms |

  L'air (la moitié du RTT de SCTP) ne prend que 24-28 ms au p90, et SCTP ne
  retransmet rien. C'est la fenêtre de congestion d'usrsctp qui retient la
  vidéo à 120 i/s, comme dans W1 du plan Wi-Fi. Le décodeur du N95 ralentit
  aussi, mais il ne compte que pour une quinzaine de millisecondes. Pour
  corriger, il faut agir sur l'envoi (le plan Wi-Fi), pas sur le décodeur.
- **L'AV1 est pire encore sur ce client.** Le décodage prend 0,9 à 1,1 s par
  image, et la bande sort gris-violet, illisible sur l'Arc. Le N95 ne décode
  pas l'AV1 1080p à cette cadence. Il n'est pas vérifié si son décodeur AV1
  est matériel ou logiciel.
- **Trou de l'outil, corrigé ensuite.** Le journal par image (U0.2) n'avait
  vu aucune image AV1. La voie AV1 donne au décodeur un horodatage inventé et
  un tampon d'hôte nul, pour que le pacer présente au décodage, et le journal
  lisait ce zéro. Depuis `1cdb4eb9`, le tampon suit l'image jusqu'au journal
  seul. Le pacer, la grille de vsync, la détection et la sonde d'âge ne voient
  toujours rien en AV1 : **la détection de l'« Auto » ne mesure donc pas
  l'AV1**. C'est noté, non corrigé. La correction n'est pas encore vérifiée en
  vrai stream.
- **Pas de case sous charge GPU dans cette série.** `mw-gpu-load` s'arrête au
  bout de 60 s, plus court qu'une passe. Il faudra le brancher dans `pass.py`,
  entre la calibration et la mesure.

**L'UM790Pro en Ethernet** (même jour, 18:33-19:20). Client UM790Pro sous
Windows (Chrome, 780M, écran virtuel à 120 Hz, câble 1 Gbit/s), même hôte et
mêmes cases, `relaylog=1` sur toutes les passes. Le `build\` de 18:02 contient
`e0324f4e` (clé `retrcut` du plan Wi-Fi, éteinte par défaut : rien ne change
sans elle). Médianes de deux passes par mode :

| Hôte | Mode | Cadence | Âge montré | E2E par image, médiane / p99 | Clic → drapeau (clics mesurés) |
|---|---|---|---|---|---|
| RTX | D | 239 i/s | 23,8 ms | 8,4 / 126 ms | 34,8 ms (58 / 60) |
| RTX | U | 120 i/s | 24,7 ms | 9,8 / 24,7 ms | 32,9 ms (57 / 60) |
| Arc | D | 238-239 i/s | 27,1 ms | 10,2 / 25,3 ms | 38,5 ms (58 / 60) |
| Arc | U | 120-121 i/s | 25,0 ms | 12,9 / 30,3 ms | 36,4 ms (56 / 60) |
| iGPU AMD | D | 227-234 i/s | 24,6 ms | 11,4 / 33,2 ms | 42,5 ms (58 / 60) |
| iGPU AMD | U | 119-121 i/s | 28,9 ms | 13,9 / 27,2 ms | 40,0 ms (58 / 60) |
| Arc | A (AV1) | 120-124 i/s | 27,1 ms | 15,5 / 41,0 ms | 44,0 ms (28 / 60) |
| RTX | A (AV1) | 120-121 i/s | 28,3 ms | 11,8 / 30,3 ms | 35,3 ms (58 / 60) |

- **En Ethernet, l'Ultra tient, mais le mode par défaut fait déjà aussi bien.**
  La détection monte à 230-240 i/s et y reste. U, à 120 i/s, fait jeu égal :
  son âge médian par image est de 1 à 3 ms plus haut, mais sa queue est plus
  courte (p99 de 25-30 ms, contre 25-126 ms pour D, le pire étant la RTX à
  240 i/s). Le N95 en Wi-Fi est deux à trois fois plus lent, même en D (âge de
  45-58 ms, clic de 83-94 ms).
- **La fenêtre de congestion d'usrsctp ne retient la vidéo qu'en Wi-Fi.** Ici,
  le réseau a un p90 de 7 à 15 ms, dont 5 à 12 ms avant de quitter usrsctp,
  quel que soit le mode, contre 186-232 ms sur le N95 en U.
- **L'AV1 tient sur le 780M, sans rien gagner** : 1 à 4 ms de plus que le HEVC
  à la même barre, pour 20 à 50 % de débit en plus (36-47 contre 29-32 Mbit/s).
- **Outil.** `1cdb4eb9` est vérifié en vrai stream : les passes AV1 ont leur
  âge par image. Une passe est illisible (Arc, AV1, A1) : tous les pixels lus
  de la bande et du drapeau sont blancs (255), alors que le décodage et l'âge
  par image étaient normaux. La passe suivante sur le même hôte est passée, et
  ses clics manquent au tableau (28 / 60). Cause probable : la fenêtre blanche
  du kiosque de contenu, déjà vue le 22/09 (`acceptance/run.py`,
  `content_start`). Le banc d'âge l'ouvre sans la vérification d'image qui
  relance un kiosque resté blanc (`probe=False`). Le calage de la bande, par
  DevTools, réussit quand même. Ce n'est pas vérifié.

**L'iPhone de Bruno** (même soir, 21:26-21:37, Safari, Wi-Fi). L'hôte est la
`--dev` de DualRTX, avec son écran virtuel au défaut du produit (rendu par
l'Arc, D3D12 VE Intel). Quatre passes en alternance Auto, 120, Auto, 120 (la
page de banc défile, l'iPhone reste immobile 60 s), relevées sur les captures du
détail de la latence. Sur iOS, ni la sonde d'âge ni le clic → drapeau ne
tournent sans console : seule la ligne « Mesurée » de U0.2 donne l'âge.

| Mode | Cadence, taille | « Mesurée » moyenne / p99 | Décodage | Réseau |
|---|---|---|---|---|
| Auto | 60 i/s, 2532×1170 | 41,2 / 69,1 puis 47,9 / 80,4 ms | 5,6 puis 9,7 ms | 4,5 à 40 ms (une coupure de 0,37 s) |
| 120 | 99 i/s, 2336×1080 | 39,8 / 67,2 puis 30,5 / 56,4 ms | 7,3 puis 4,7 ms | 5,3 à 17 ms |

- **Le 120 gagne de 5 à 10 ms sur l'iPhone**, avec deux passes par mode
  seulement, en Wi-Fi.
- **Le décodage HEVC matériel de l'iPhone est mesuré pour la première fois** :
  5 à 10 ms en moyenne, 8 à 21 ms au pire.
- **À creuser** : 38 à 47 % d'images comptées « dropped (jitter) » dans les deux
  modes, alors qu'aucune n'est perdue sur le réseau.
- **Lien** : sur iOS, la `--dev` ne se joint pas à son adresse du LAN
  (`https://192.168.1.66:8443/`). Safari n'étend pas au WebSocket de
  signalisation l'exception de certificat acceptée pour la page, et le journal
  de l'hôte dit « certificate unknown » toutes les 4 s. Son lien de staging
  (`stream.dev.moonlightweb.top/<id>`, vrai certificat, vidéo restée en direct
  sur le LAN) marche.

**Le Mac M1 en Wi-Fi** (même nuit, 22:49-23:19, Chrome de banc, capot fermé).
Même hôte, mêmes cases en HEVC (le M1 ne décode pas l'AV1), `relaylog=1`.
Médianes de deux passes par mode :

| Hôte | Mode | Âge montré | E2E par image, médiane / p99 | Clic → drapeau (clics mesurés) |
|---|---|---|---|---|
| RTX | D | 29,4 ms | 14,1 / 165 ms | 63,7 ms (58 / 60) |
| RTX | U | 36,2 ms | 22,4 / 304 ms | 60,9 ms (55 / 60) |
| Arc | D | 37,1 ms | 22,4 / 157 ms | 72,2 ms (58 / 60) |
| Arc | U | 43,9 ms | 26,4 / 184 ms | 71,0 ms (56 / 60) |
| iGPU AMD | D | 32,7 ms | 17,0 / 145 ms | 74,9 ms (55 / 60) |
| iGPU AMD | U | 43,2 ms | 24,6 / 228 ms | 71,2 ms (57 / 60) |

- **Le défaut monte déjà à 114-125 i/s**, la cadence de l'écran du Mac, et fait
  mieux que U de 7 à 11 ms d'âge. Le clic est le même dans les deux modes.
- **Le Mac tient les 120 i/s, contrairement au N95.** Son p90 avant de quitter
  usrsctp est de 50 à 79 ms dans les deux modes, contre 186-232 ms sur le N95
  en U et 5-12 ms en Ethernet. Le Wi-Fi du Mac fait attendre, sans décrocher.
- **Bilan provisoire de U0.3** : sur ordinateur, la barre « HEVC réglé Ultra »
  ne bat l'« Auto » détecté sur aucun client. Le N95 en Wi-Fi décroche à
  120 i/s, et l'UM790Pro et le Mac font jeu égal ou mieux en Auto. Sur
  l'iPhone, le 120 gagne 5 à 10 ms. Restent l'iPad, RE9 sur la RTX, les cases
  sous charge GPU et Android.

**L'iPad de Bruno sous RE9** (04/10, 19:17-19:36, Safari, Wi-Fi). RE9 (la copie
de banc) tourne sur l'écran de la RTX (DISPLAY5, M27Q 120 Hz, NVENC), streamé
par la tuile de cet écran (pas d'écran virtuel). La `--dev` est celle du build
`ded56fc6`, jointe par stream.dev. Relevé sur les captures du détail :

| Passe | Taille, cadence | « Mesurée » moy. / p99 | Décodage moy. / max | Réseau | « dropped (jitter) » |
|---|---|---|---|---|---|
| Auto | 2162×1216, 50 i/s | 82 / 145 ms | 21,6 / 75 ms | 13,8 ms | 44 % |
| 120 | 1920×1080, 62 i/s | 65 / 127 ms | 16,4 / 43 ms | 19,5 ms | 45 % |
| Auto, 1080p imposé | 1920×1080, 26 i/s | 31 / 49 ms | 8,0 / 11 ms | 2,3 ms | 3 % |

- **Le décodeur de l'iPad sature à 50-60 i/s.** Il prend 16 à 22 ms par image
  en moyenne, et jusqu'à 75 ms. À 26 i/s, il descend à 8 ms, et l'image n'a
  plus que 31 ms.
- **Safari annonce un écran à 32-51 Hz**, sans mode Économie d'énergie
  (vérifié par Bruno). L'« Auto » a suivi ce chiffre : 26 i/s à la passe 3
  (« 26 fps stream for a 51 Hz client… every 2nd refresh » dans le journal),
  alors que RE9 présentait environ 65 images par seconde. Sur cet iPad, c'est
  ce qui le sert le mieux, mais par accident. La mesure du rafraîchissement
  sous Safari est à revoir.
- **Le compteur « dropped (jitter) » suit la cadence** : 44-45 % à 50-62 i/s,
  3 % à 26 i/s, sans aucune perte réseau. Comme sur l'iPhone, ce sont des
  images que le décodeur n'a pas pu suivre, pas des pertes du lien. C'est une
  hypothèse, non vérifiée.
- DualRTX a fait un écran bleu (0x133, `DPC_WATCHDOG`) à 18:58, pendant un RE9
  sur la RTX juste avant ces passes, et ne l'a pas refait ensuite.

### 6.4 U0.3 — synthèse provisoire (04/10/2026)

**La barre à battre n'est pas « HEVC réglé Ultra », c'est l'« Auto » détecté.**
Sur les trois clients d'ordinateur, la barre du plan (§2 : 120 i/s, écran
virtuel à 240 Hz, tearing, P1) ne fait jamais mieux que l'« Auto » avec
détection (UA, sur `main`). Sur un client qui suit, la détection monte d'elle-même
au-dessus : 230-240 i/s sur l'UM790Pro, la cadence de l'écran (120 i/s) sur le
Mac. Sur un client qui ne suit pas (le N95 en Wi-Fi), elle reste à la cadence
de l'écran, là où forcer 120 i/s fait décrocher le lien.

| Client, lien | « Auto » détecté : âge montré / clic | Barre Ultra : âge montré / clic | File usrsctp p90 (D / U) |
|---|---|---|---|
| UM790Pro, Ethernet | 24-27 / 35-43 ms | 23-29 / 33-40 ms | 5-12 / 5-12 ms |
| Mac M1, Wi-Fi | 29-37 / 61-75 ms | 36-44 / 61-71 ms | 50-67 / 55-79 ms |
| N95, Wi-Fi | 45-58 / 83-94 ms | 130-320 ms / un clic sur deux perdu | 35-54 / 186-232 ms |
| iPhone, Wi-Fi (« Mesurée ») | 41-48 ms | 31-40 ms | — |

**Le budget d'un clic sur l'UM790Pro en Ethernet** (`flagpath.py`, médianes
des 20 passes, en ms) : montée du clic 1,5-3, le drapeau dessiné sur l'hôte
12-16 (la boucle de messages de l'overlay de banc, pas le produit), jusqu'à la
capture 1-5, encodage 2-6, envoi 0,2, réseau 6-10, décodage 0,4-5, dessin
0,2-6. Hors drapeau de banc, il reste 20 à 27 ms. Un codec intra ne peut
gagner que sur l'encodage et le décodage, soit 3 à 10 ms à se partager, et la
cadence (déjà à 240 i/s) n'a plus de marge.

**Ce que cela change pour la suite du POC (proposition pour la porte U0)** :
- La référence de U3 et U5 devient l'« Auto » détecté, plus la barre §2. Le
  critère de §2 (− 2 ms ou − 20 % de médiane, p99 pas pire) se lit contre
  elle.
- Le Wi-Fi n'est pas un terrain pour Ultra. Même en HEVC à 20-30 Mbit/s, la
  fenêtre de congestion d'usrsctp retient déjà la vidéo à 120 i/s (plan Wi-Fi,
  W1). À 150-200 Mbit/s, elle ne passerait pas. Ultra reste « Ethernet
  seulement », comme prévu en §1.
- L'AV1 ne gagne rien sur le HEVC à la même barre (+1 à 4 ms, +20 à 50 % de
  débit, UM790Pro), et il casse sur le N95. Il ne sert pas de référence.

**Le 04/10 (UM790Pro, build `2ef56bfe` : `sctpburst=0` et `retrcut=3`
par défaut depuis ce build, à dire en comparant aux passes d'avant)** :
- **`6a7c3b43` est vérifié.** En « Auto » sur l'Arc, un stream AV1 monte à
  240 i/s comme un HEVC : décision prise vers 5-18 s, gardée. Le filet joue
  aussi en AV1 : une fois, une file de décodeur qui tient a fait redescendre la
  détection, qui a ensuite regagné 240 i/s. L'AV1 reste derrière : clic de
  43 ms contre 37, 40-46 Mbit/s contre 25-27.
- **Sous charge GPU** (`mw-gpu-load` à ~45 i/s, `4ba3f0d1`), deux passes par
  mode :

  | GPU chargé | Mode | Âge montré | E2E hôte → dessin | Avant la capture | Clic → drapeau |
  |---|---|---|---|---|---|
  | Arc | D | 126-127 ms | 38 ms | ~67 ms | 84-86 ms |
  | Arc | U | 129-137 ms | 39-52 ms | ~67 ms | 84-86 ms |
  | iGPU AMD | D | 163-164 ms | 13-14 ms | 137-158 ms | 135-137 ms |
  | iGPU AMD | U | 168-187 ms | 13 ms | 137-158 ms | 131-135 ms |

  Sous charge, c'est surtout la page de banc elle-même qui attend son GPU,
  avant la capture. Le stream n'ajoute que 13 ms (AMD) à 38-52 ms (Arc). U n'y
  change rien de net. Mais la fenêtre de charge était sur l'écran virtuel
  capturé, qui devient l'écran principal pendant le stream (corrigé dans
  `beeabde4`).
- **Cases refaites** (04/10, 17:04-17:25, build `ded56fc6`, fenêtre de charge
  sur un écran à part, deux passes par mode). L'écran virtuel de l'UM790Pro
  était entre-temps passé de 120 à 240 Hz, sans le banc : ce n'est pas tout à
  fait la même case que le matin.

  | GPU chargé (charge) | Mode | Âge montré | E2E hôte → dessin | Avant la capture | Clic → drapeau |
  |---|---|---|---|---|---|
  | Arc (30 i/s) | D | 192 ms | 48 ms | ~105 ms | 99 ms |
  | Arc (30 i/s) | U | 189 ms | 50 ms | ~105 ms | 96 ms |
  | iGPU AMD (47-52 i/s) | D | 107 ms | 23 ms | 70-75 ms | 93 ms |
  | iGPU AMD (47-52 i/s) | U | 118 ms | 28 ms | 70-75 ms | 97 ms |

  Le verdict ne change pas. Sous charge, la page de banc attend son GPU, U et
  D font jeu égal, et le stream reste la plus petite part de l'âge. Au même
  niveau, la charge tourne à 30 i/s sur l'Arc au lieu de 45 le matin : sa
  fenêtre, posée sur un écran physique, ne pèse plus pareil.

**Reste avant la porte U0** : les TV (U0.3 quater), la borne Steam sur l'iGPU et MoonlightWeb au même outil (U0.4) et le
rapport U0.5.

### 6.5 U0.4 — la borne Steam, hôte RTX (04/10/2026, 22:10-22:40)

Steam Remote Play (bêta « Steam Beta Update »), hôte DualRTX, l'écran
principal étant celui de la RTX (NVENC pour le HEVC). Client l'UM790Pro sous
Windows en 1 GbE (780M, décodage matériel, 1920×1080, débit automatique,
modificateur de qualité au milieu, 4:4:4 coupé). Mesure sans caméra
(`scripts/bench/photon/`, `2e0a012b`, `5de1e2af`) : l'hôte affiche une
fenêtre qui passe du noir au blanc à chaque clic, streamée comme jeu non-Steam.
Le client clique dans la fenêtre du stream et relit ce pixel sur son propre
bureau composé (GDI), jusqu'au changement. Le clic → photon compte donc tout,
de la montée du clic à la composition du client, sauf le balayage de l'écran
lui-même. 60 clics par passe, aucun manqué ; résultats dans
`bench-out/photon/steam-*.json`.

| Passe | Codec | Low Latency Networking | Médiane | p90 | Min - max |
|---|---|---|---|---|---|
| 1 | HEVC | non | 49,9 ms | 58,9 ms | 32,6 - 67,2 |
| 2 | PyroWave | non | 42,7 ms | 59,0 ms | 32,7 - 66,6 |
| 3 | HEVC | non | 57,9 ms | 66,6 ms | 40,6 - 91,7 |
| 4 | PyroWave | non | 42,6 ms | 58,3 ms | 32,4 - 83,9 |
| 5 | HEVC | oui | 58,6 ms | 83,2 ms | 41,3 - 375,8 |
| 6 | PyroWave | oui | 49,7 ms | 59,1 ms | 32,9 - 92,3 |

- ⚠️ **Correction (05/10, 00:10)** : de 22:40 à 23:00, un Chrome de banc de la
  session Wi-Fi tournait sur DualRTX, avec la RTX et DISPLAY5, l'écran
  streamé ; il était lancé là par un défaut de son outil, corrigé par `09df1c3c`.
  La passe 1 (HEVC, finie à 22:56) en est entachée. Toutes les autres passes
  ont tourné après 23:00.
- **PyroWave natif bat le HEVC de Steam de 15 ms en médiane sur la RTX, en ne
  gardant que les passes propres** : 42,6-42,7 ms contre 57,9 ms (58,6 avec Low
  Latency Networking). Avec la passe 1 entachée, l'écart était de 7 à 15 ms. Il est très stable d'une passe à l'autre,
  là où le HEVC varie de 8 ms. Les p90 sont proches (58-59 contre 59-67 ms).
  **La porte de U0.4 (au moins 2 ms sur NVIDIA) est franchie** : le POC ne se
  resserre pas sur les hôtes à iGPU.
- « Low Latency Networking » de Steam dégrade les deux codecs (+7 ms en
  médiane pour PyroWave ; pour le HEVC, une queue à 83 ms au p90 et une pointe
  à 376 ms).
- **MoonlightWeb, même couple, même outil** (23:22, `--dev` du build
  `ded56fc6`, écran de la RTX streamé en « Auto », 120 i/s, tearing, Chrome
  sur l'UM790Pro) : médiane 58,2 ms, p90 75,0 ms (42 à 108), 60 clics sur 60.
  C'est au niveau du HEVC de Steam (49,9-57,9), 15 ms derrière son PyroWave.
  Une 2e passe est invalide (58 clics manqués sur 60, 23:26:40-23:27:41). Le
  Chrome de banc de la session Wi-Fi a redémarré sur DISPLAY5 à 23:27, en plein
  écran sur l'écran streamé : c'est la cause la plus probable. Le drapeau de
  latence de la `--dev` au point du clic est l'autre suspect.
- **Les deux mesures de MoonlightWeb ne disent pas la même chose.** Son propre
  clic → drapeau donnait 33-43 ms sur ce couple (U0.3), parce qu'il lit le
  drapeau dans le canevas, au moment du dessin. `click-photon.ps1` lit le
  bureau composé du client : il compte en plus la composition de Chrome et
  celle du DWM. L'écart, environ 15 à 25 ms, est ce que coûte le chemin de
  présentation du navigateur, qu'un client natif comme Steam n'a pas. Pour
  Ultra, c'est un poste que le codec ne touche pas (U3, présentateurs au
  photon).
- **Passes du 05/10 au matin** (07:10-07:55, même couple, même outil,
  `bench-out/photon/*-3.json` et `steam-amd-*.json`) :

  | Hôte (encodeur) | Client | Passes, médiane (p90) |
  |---|---|---|
  | RTX (NVENC) | Steam HEVC | 58,5 ms (75,6) |
  | RTX | Steam PyroWave | 49,2 ms (58,0) |
  | RTX | MoonlightWeb HEVC, lecture à 200 px du clic | 57,3 ms (74,1) |
  | iGPU AMD (écran de l'AMD en principal) | Steam HEVC | 57,9 (66,7), puis 49,4 ms (59,0) |
  | iGPU AMD | Steam PyroWave | 41,7 (58,2), puis 49,6 ms (58,6) |

- **Bilan de la borne Steam, passes propres seulement** :
  - RTX : PyroWave 42,6, 42,7 et 49,2 ms contre 57,9 et 58,5 ms pour le
    HEVC, soit un gain de 9 à 16 ms ;
  - iGPU AMD : PyroWave 41,7 et 49,6 ms contre 49,4 et 57,9 ms, soit environ
    8 ms en moyenne.
  - MoonlightWeb (57,3 et 58,2 ms) est au niveau du HEVC de Steam, sur les
    deux passes.
- **Les médianes tombent sur des marches d'environ 8,3 ms** (41,7, 49,4-49,6,
  57,3-58,5), la période d'un écran à 120 Hz (le M27Q de l'hôte et le client à
  120 Hz). Les écarts entre codecs valent donc une ou deux images
  d'affichage, et une même configuration peut tomber d'une marche à l'autre
  d'une passe à l'autre. C'est une lecture, pas encore une mesure.

### 6.6 U0.5 — rapport de la phase U0 (version finale du 05/10/2026)

Le brouillon du 04/10 (`3c8ea54b`) avait servi à la porte U0, franchie le soir
même. Cette version le complète avec ce que la phase U1 a appris depuis sur le
banc. Seules les TV (U0.3 quater) manquent encore, et elles ne bloquent aucune
porte.

**0. Une correction qui vaut pour tout ce qui suit.** Le « 1 GbE » entre
DualRTX et l'UM790Pro passe par un saut Wi-Fi 7 entre deux répéteurs (§6.10) :
2,6 ms d'aller-retour, TCP à 74-90 Mbit/s, UDP propre jusqu'à ~150 Mbit/s, et un
lien partagé avec la maison. Les chiffres « Ethernet » de U0 sont donc ceux d'un
bon Wi-Fi. Le poste réseau du tableau (6-10 ms) en est gonflé d'environ 2 ms, et
le reste est inchangé. Ils seront refaits sur câble.

**1. Budget par étape, mesuré.** Les deux couples les mieux couverts, en ms :

| Étape | UM790Pro en Ethernet, hôte RTX/Arc/AMD (U0.3, flagpath) | iPhone / iPad en Wi-Fi (U0.3, détail de latence) |
|---|---|---|
| Montée du clic | 1,5-3 | — |
| Hôte : capture → remis au relais | 2-6 (encodage NVENC 1,4-2, Arc 3,5-4,5, AMF 4-4,3) | 4-6 |
| Réseau, dont l'attente dans usrsctp | 6-10, dont 3-5 | 2-20 (pointes des coupures Wi-Fi) |
| Décodage | 0,4-5 | iPhone 5-10 ; iPad 16-22 à 50-62 i/s, 8 à 26 i/s |
| Attente et dessin | 0,2-6 | 7-11 (file de rendu) |
| Âge de l'hôte au dessin (journal par image) | 8-14 médiane | iPhone 31-48 ; iPad 31-82 |

En Wi-Fi, le réseau domine quand la cadence monte. La fenêtre de congestion
d'usrsctp retient la vidéo : 186-232 ms au p90 sur le N95 à 120 i/s, 50-79 ms
sur le Mac (plan Wi-Fi, `docs/design/network-latency-findings.md`). Sur un
client mobile, c'est le décodeur. Sur le chemin de l'UM790Pro (« Ethernet »),
aucun poste ne dépasse 6 ms, sauf le réseau : celui-ci compte le saut Wi-Fi 7.

Deux postes que ce tableau ne montre pas, trouvés depuis :
- **La composition chez le client** : 15-25 ms entre le dessin dans le canevas
  et le bureau composé (U0.4). Le présentateur et le plein écran n'y changent
  rien (U3, §6.9).
- **La retenue de Chrome sur une piste vidéo RTP** : 7,8 ms en moyenne, le
  métronome à 64 Hz de Blink (§6.11). Elle ne touche pas le DataChannel de
  U0, mais tout transport RTP de la suite. La route audio la ramène à 0,2 ms.

**2. Le modèle du §3, recalé.**
- Le segment codec du HEVC est bien de 3 à 6 ms sur la RTX et l'Arc, en
  Ethernet, comme prévu.
- La cadence pèse plus que le codec, comme prévu. La détection de l'« Auto »
  la règle déjà au mieux : 240 i/s sur l'UM790Pro, 120 sur le Mac, la cadence
  de l'écran sur le N95 et l'iPad. La barre « HEVC réglé Ultra » du §2 ne
  bat donc jamais l'« Auto » détecté (§6.4).
- Ce que le modèle n'avait pas : **le chemin de présentation du navigateur.**
  Le même clic, mesuré sur le bureau composé du client (`click-photon.ps1`),
  coûte 58 ms dans Chrome contre 33-43 ms lu dans le canevas : 15 à 25 ms
  passent dans la composition de Chrome et du DWM. Steam, client natif, n'a
  pas ce poste.
- Ce que le modèle sous-estimait : **le transport.** Le §3 comptait 2,8 ms de
  sérialisation pour PyroWave à 170 Mbit/s. Le DataChannel plafonne à
  95-107 Mbit/s sur ce chemin, et fait attendre la vidéo derrière toute
  charge lourde de la même association (U1.2, §6.8). Le gain d'Ultra
  dépend donc d'abord de la piste qui le porte (U1.4 : la route audio).
- Segment codec, mesuré contre prédit : RTX 1,4-2 ms d'encodage (prédit 3,4
  avec le décodage), Arc 3,5-4,5, AMF 4-4,3 en Ethernet. L'iGPU AMD encode
  donc plus vite que prévu. Avec le décodage (0,4-5 ms), son segment HEVC
  fait 5-9 ms, pas 10,5-18,5. Contre ~3,5-4 ms pour PyroWave au seuil, la
  marge d'Ultra sur cet hôte tombe à 1-6 ms, au lieu de 4-12.

**3. La borne Steam (U0.4, RTX puis iGPU AMD, §6.5).** Sur les passes propres,
PyroWave natif bat le HEVC de Steam de 9 à 16 ms sur la RTX (42,6-49,2 contre
57,9-58,5 ms), et d'environ 8 ms sur l'iGPU AMD. MoonlightWeb, au même outil,
est au niveau du HEVC de Steam (57,3-58,2 ms). La porte de U0.4 (≥ 2 ms sur
NVIDIA) est franchie.
- Les médianes tombent sur des marches d'environ 8,3 ms, la période de l'écran
  à 120 Hz : l'écart vaut une à deux images d'affichage.
- Ce gain dépasse ce que le codec peut gagner seul : le modèle du §3 prédisait
  plutôt une légère perte sur NVIDIA, et l'encodage plus le décodage du HEVC ne
  coûtent ici que 3 à 7 ms.
- Steam semble donc mettre sur sa voie HEVC une attente que sa voie PyroWave
  n'a pas : une file de décodage ou un rythme de présentation. Ce n'est pas
  vérifié : il faudrait le relevé de performance de Steam, ou une caméra.
- Lecture prudente : PyroWave dans un navigateur ne gagnera sur notre HEVC que
  ce que le codec fait vraiment gagner, soit 3 à 7 ms en Ethernet sur ces
  hôtes. Et seulement si le décodeur et le présentateur ne reprennent pas ce
  gain (U3).

**4. Le verdict de l'essai « cadence de l'hôte »** (plan 1 `framerate-hote`,
U0.3 bis et ter). C'est la détection côté client (« Auto » avec détection, UA)
qui est sur `main` et sert de référence. `host-guarded` n'a pas battu
l'« Auto » d'aujourd'hui (UA.3).

**La barre « HEVC réglé Ultra » qui en découle** : l'« Auto » détecté lui-même
(240 i/s sur l'UM790Pro, la cadence de l'écran ailleurs), HEVC, P1, tearing.
Forcer 120 i/s avec un écran virtuel à 240 Hz ne fait jamais mieux, et fait
décrocher le lien du N95 en Wi-Fi (§6.4).

| Client, lien | « Auto » détecté : âge montré / clic → drapeau |
|---|---|
| UM790Pro, « Ethernet » | 24-27 / 35-43 ms |
| Mac M1, Wi-Fi | 29-37 / 61-75 ms |
| N95, Wi-Fi | 45-58 / 83-94 ms |
| iPhone, Wi-Fi (« Mesurée ») | 41-48 ms (120 i/s forcé : 31-40) |

L'iPhone est le seul client où forcer 120 i/s gagne (5-10 ms).

**5. La porte U0, tranchée** (Bruno, 04/10, environ 23:45 : « Ok, go ») :
- U1 lancé, avec trois corrections au plan :
  1. La référence de U3 et U5 devient l'« Auto » détecté.
  2. Ultra reste réservé à l'Ethernet, avec PyroWave au cœur.
  3. Le chemin de présentation (U3) passe avant le décodeur.
- Depuis, U3 a montré que le présentateur ne rend pas ces 15-25 ms (§6.9).
  Le levier qui reste est le transport. Le DataChannel ne porte pas Ultra
  (U1.2). La route audio le porte sur ce chemin, sans retenue de Chrome et
  sans faire attendre la vidéo (U1.4 ter et quater, §6.11).

**6. Ce qui reste ouvert de U0** :
- **Les TV** (U0.3 quater) : budget par étape sur la Mi TV et la Freebox Player
  POP, au créneau que Bruno fixera.
- **Le câble** : les passes de U0, U1.2 et U1.4 sont à refaire sur un vrai
  1 GbE, pour retirer le saut Wi-Fi 7 du poste réseau.
- **MoonlightWeb sur l'iGPU AMD au même outil que Steam** : seule la RTX a sa
  passe MoonlightWeb au photon.

### 6.7 Après la porte U0 : U1 lancé, U3 préparé (05/10/2026, nuit)

**Porte U0 franchie** (Bruno, 04/10, environ 23:45 : « Ok, go »).
- La référence de la suite est l'« Auto » détecté.
- Ultra ne vaut qu'en Ethernet, avec PyroWave au cœur.
- Le chemin de présentation passe avant le décodeur.

**U1.1 fait.**
- `e8f02ce0` côté hôte : la clé `ultra=synthetic:<Kio>` envoie, après chaque
  image vidéo, une charge incompressible de cette taille. Elle part sur le
  canal négocié id 5 (l'id 4 est celui du HID), au format des chunks vidéo et
  avec le tampon de l'image, par un `FrameSender` à part. `ultrachannel=`
  choisit la fiabilité du canal.
- `5ee304da` côté client : `mw_ultra_sink` (`UltraSink.js`) compte par seconde
  les images, les Mbit/s, les pertes, l'étalement et le retard en plus.
- `3178043e` : chaque passe de banc garde ces chiffres.
- Essai local sur DualRTX (04/10, 23:44) : 273 images de 200 Kio, aucune
  perdue, étalement de 3 à 9 ms. Les réglages de la table U1.1 que le plan
  Wi-Fi a déjà rendus clés de banc (`sctpburst`, `sctpbuf`, `sctpcc`,
  `retrcut`, `relaylog`) sont repris, pas refaits.
- **U1.2 est prêt**, à passer quand l'UM790Pro sera libre : 6 tailles de 40 à
  2000 Kio × 60 et 120 i/s, au transport du produit. Puis, à 350 et 1000 Kio
  et 120 i/s : un tampon d'envoi de 4 Mio, le canal non ordonné, et
  l'ancien burst maximal.

**U3, le chemin de présentation, à mesurer avant le décodeur.** Mesuré
au `click-photon.ps1`, le clic coûte 58 ms dans Chrome, contre 33-43 ms lu
dans le canevas : 15 à 25 ms pour la composition de Chrome et du DWM (§6.5).
Le client a déjà quatre présentateurs : Canvas2D désynchronisé (le défaut en
tearing), WebGL2, WebGPU, et `<video>` nourri au décodage (menu de débogage).
Une première matrice, sans code neuf :
- ces quatre présentateurs × en fenêtre / en plein écran, le plein écran
  pouvant laisser le DWM passer en « independent flip » ;
- sur l'UM790Pro en Ethernet (écran à 240 Hz), hôte RTX ;
- `click-photon.ps1 -ReadDx 200` lit un point écarté du clic, pour que le
  drapeau de banc (`latency_flag_enabled`) ne le masque plus (`0bceb41e`) ;
- 60 clics par case, en alternance, et le même couple refait sous Steam en
  référence native.

Ce qui en sortira : le présentateur et le mode d'écran qui rendent le plus de
ces 15-25 ms. Le décodeur d'Ultra (U3.4) se branchera sur ce présentateur-là.

### 6.8 U1.2 — ce que le DataChannel porte à haut débit (05/10/2026, 06:56-08:46)

Hôte DualRTX : la `--dev` du build `66f71c56`, avec les défauts Windows
`sctpburst=0` et `retrcut=3`. L'écran streamé est un écran physique, DISPLAY1
(celui de l'Arc, à 120 Hz), choisi par Bruno pour ne faire aucune bascule
d'écran virtuel ; la page de banc défile dessus. Client l'UM790Pro sous
Windows, Chrome, en 1 GbE. Chaque passe ajoute `ultra=synthetic:<Kio>`
(§6.7), `relaylog=1`, 30 s d'âge du contenu et 30 clics. Médianes sur les
30 dernières secondes, lues par le client (`mw_ultra_sink`) ; le retard est
le p95 au-dessus du plus petit de la session.

| Kio / image | Débit demandé à 60 / 120 i/s | Reçu à 60 i/s | Reçu à 120 i/s | Retard p95 (60 / 120) | Vidéo à 60 / 120 i/s |
|---|---|---|---|---|---|
| 40 | 20 / 39 Mbit/s | 19,4 | 37,7 | 77 / 16 ms | 60 / 111 i/s |
| 200 | 98 / 197 | 19,2 | 98,9 | 1 427 / 397 ms | 10 / 59 i/s |
| 350 | 172 / 344 | 62,4 | 103,3 | 818 / 717 ms | 12 / 34 i/s |
| 500 | 246 / 492 | 105,7 | 95,1 | 605 / 1 320 ms | 25 / 21 i/s |
| 1 000 | 492 / 983 | 105,2 | 95,7 | 1 508 / 2 685 ms | 11 / 13 i/s |
| 2 000 | 983 / 1 966 | 106,7 | 97,0 | 3 121 / 3 961 ms | 6 / 5 i/s |

Variantes, à 120 i/s (reçu, retard p95, images Ultra perdues) :

| Variante | 350 Kio | 1 000 Kio |
|---|---|---|
| canal Ultra non ordonné, sans retransmission (`ultrachannel=unordered`) | 90,7 Mbit/s, 820 ms, 3 | 63,2, 2 809 ms, 15 |
| l'ancien burst maximal (`sctpburst=10`) | 97,0, 810 ms, 0 | 15,4, 3 421 ms, 22 |
| un tampon d'envoi de 1 Mio (`sctpbuf=1024`, le plus que la clé accepte) | 64,5, 765 ms, 11 ; **la vidéo ne s'affiche plus** | 49,5, 4 357 ms, 22 ; idem |

- **Le DataChannel plafonne à 95-107 Mbit/s sur ce lien 1 GbE**, quelle que
  soit la taille des images, à 60 comme à 120 i/s. PyroWave en demande 170 à
  son seuil subjectif (§3) : **avec le transport d'aujourd'hui, Ultra ne tient
  pas**. Au-delà du plafond, l'attente monte à des secondes, et la vidéo tombe
  avec elle.
- **Un expéditeur à part ne protège pas la vidéo.** Ultra et la vidéo
  partagent la même association SCTP, donc sa fenêtre de congestion et ses
  tampons : une charge Ultra au plafond fait tomber la vidéo à 5-25 i/s. La
  promesse de U1.1 (« jamais la vidéo ») ne vaut que pour la file de
  l'expéditeur, pas pour le lien.
- Aucune variante ne relève le plafond.
  - Le canal non ordonné perd des images.
  - L'ancien burst maximal s'effondre à 1 000 Kio : le défaut `sctpburst=0`
    est le bon.
  - Le tampon de 1 Mio fait pire, et casse la vidéo : `sctpbuf` fixe aussi la
    taille du plus grand message.
- **Un piège de banc** : une valeur hors bornes (`sctpbuf=4096`) fait ignorer
  **toute** la chaîne `MW_NATIVE_TUNING`, Ultra compris. Deux passes perdues,
  mises de côté dans `bench-out/content-age/u1-void/`.
- À 200 Kio et 60 i/s, le débit reçu (19 Mbit/s) est bien plus bas qu'à 350
  Kio (62). Ce n'est pas expliqué, ni encore refait.

**Ce que U1.3 doit trancher** : d'où vient ce plafond. L'hôte (le fil d'envoi
et libjuice) ? Le client (la réception SCTP de Chrome, la boucle de messages) ?
Ou usrsctp lui-même, dont le plan Punktfunk relevait déjà le plafond sous
pertes (A0, §8r.1) ? La table U1.1 prévoit encore le MTU de 1 500 et la
réception dans un worker. Si rien ne passe nettement 170 Mbit/s, la porte U1
ouvre la sonde du plan B (le flux Ultra dans une piste RTP, par Encoded
Transform).

### 6.9 U3 d'abord — le chemin de présentation de Chrome (05/10/2026, 08:47-09:00)

Hôte DualRTX (`--dev` `66f71c56`), qui streame l'écran de la RTX (DISPLAY5,
sans écran virtuel), le drapeau de `click-photon` par-dessus. Client
l'UM790Pro sous Windows, Chrome, en 1 GbE, « Auto » avec détection, en
tearing. Pour chaque case : 60 clics, le pixel lu à 200 px du clic
(`u3_series.py`, `pass.py --setting/--fullscreen`, `885070b2`) ;
`bench-out/photon/u3-*.json`.

| Présentateur | Fenêtre agrandie, médiane (p90) | Plein écran, médiane (p90) |
|---|---|---|
| Canvas2D (le défaut) | 58,8 ms (68,1) | 65,4 ms (74,3) |
| `<video>` nourri au décodage | 65,3 ms (73,6) | 59,4 ms (74,3) |
| WebGL2 (`gl-fsr1`) | 65,0 ms (82,2) | 65,5 ms (81,9), 2 clics manqués |
| WebGPU (`fsr1`) | 65,3 ms (74,2) | 65,7 ms (74,5) |

Repères, même couple, même outil (§6.5) : Steam PyroWave 42,6-49,2 ms,
Steam HEVC 57,9-58,5 ms, MoonlightWeb HEVC 57,3-58,2 ms.

- **Aucun présentateur ni le plein écran ne rend les 15-25 ms.** Toutes les
  cases tombent entre 58,8 et 65,7 ms : deux paliers à 6,5 ms d'écart,
  sans tendance par présentateur ni par mode. Le plein écran, qui pouvait
  laisser le DWM passer en « independent flip », ne gagne rien ici.
- Le meilleur reste le défaut (Canvas2D en fenêtre, 58,8 ms), au niveau du
  HEVC de Steam. L'écart avec PyroWave natif (10 à 16 ms) n'est donc pas une
  affaire de présentateur dans Chrome.
- **Non vérifié** : que chaque case a bien tourné avec le présentateur
  demandé. Le réglage passe par `video_enhancement_algo`, mais aucune trace ne
  le confirme après coup. Le pilote coupait la passe `pass.py` avant qu'elle
  écrive son JSON (overlay compris), et la page ne nomme son présentateur qu'à
  un endroit : la sonde de latence, et seulement sur un clic manqué
  (`LatencyProbe.js`, `via`). Une reprise doit relever le présentateur en
  direct par CDP, pendant la case. Les paliers de 6,5 ms
  laissent aussi penser que la cadence de présentation du client découpe les
  mesures, plus que le présentateur lui-même. À refaire en relevant le
  présentateur et la cadence d'affichage du client.
- Conséquence pour la suite (U3) : le gain ne viendra pas d'un autre
  présentateur, mais d'abord du transport (§6.8, plafond à 100 Mbit/s) et de
  la cadence de bout en bout.

### 6.10 Le chemin du banc, et U1.4 : la vidéo sur une piste RTP (05/10/2026, 10:00-11:50)

**Le « 1 GbE » de §6.1-6.9 n'en est pas un.** DualRTX et l'UM790Pro sont
chacun câblés à 1 Gbit/s sur un répéteur Freebox, mais les deux répéteurs
(rez-de-chaussée, 2e étage) sont reliés entre eux en Wi-Fi 7. Mesuré par un
outil socket (TCP, UDP rythmé, écho UDP ; `network-latency-findings.md`,
`4aebfc9a`) :
- aller-retour à vide de 2,6 ms en médiane, là où un câble donne ~0,3 ms ;
- TCP à 74-90 Mbit/s ;
- UDP sans perte jusqu'à 150 Mbit/s, puis ~155-175 Mbit/s reçus avec des
  pertes.

Le plafond de 95-107 Mbit/s de §6.8 vient donc d'abord du chemin. Toutes les
passes « Ethernet » entre ces deux machines (U0, U1.2, U3, Steam U0.4) l'ont
emprunté. Bruno rapproche les deux PC sur un câble le soir du 05/10 : U1.2 et
U1.4 y seront refaites.

**U1.4 (décision de Bruno, 05/10)** : la vidéo sur une piste RTP plutôt que sur
le DataChannel, pour les quatre codecs, avec un interrupteur par codec et par
type d'hôte. Mesurée exprès sur ce chemin Wi-Fi, pour ses pertes.
- Hôte (`e399d604`) : `rtp_video` dans settings.json, ou `MW_RTP_VIDEO` au
  banc, au format `native:h264+hevc+av1+ultra;other:h264+hevc+av1`. Vide par
  défaut, donc SCTP partout.
- Client (`b2f6d264`) : `RTCRtpScriptTransform` dans un worker, avant le
  décodeur de Chrome, puis le même `onVideo` que le DataChannel.
- Chrome 154 négocie H.264, H265 et AV1 en RTP.

Banc :
- **Natif** : client l'UM790Pro, Chrome, l'écran de l'Arc (DISPLAY1) streamé, la
  page de banc qui défile, 30 s d'âge du contenu, 30 clics, deux manches
  alternées (`u14_series.py`).
- **Autre hôte** : le Sunshine de DualRTX (l'écran DISPLAY5), relayé par la
  `--dev`, une manche.

| Hôte natif, médiane (manches 1 / 2) | Âge du contenu | Hôte → affiché | i/s affichées |
|---|---|---|---|
| H.264, RTP | 109 / 100 ms | 23,8 / 26,4 ms | 53 |
| H.264, SCTP | 86 / 89 ms | 15,3 / 16,0 ms | 52 |
| HEVC, RTP | 55 / 54 ms | 18,3 / 18,7 ms | 60 |
| HEVC, SCTP | 42 / 44 ms | 8,8 / 11,6 ms | 60 |
| AV1, RTP | 112 / 123 ms | 37,5 / 38,9 ms | 42 |
| AV1, SCTP | 115 / 117 ms | 32,9 / 32,9 ms | 40 |

| Sunshine (une manche) | Âge du contenu | Clic → drapeau, médiane (30 clics sur 30) |
|---|---|---|
| H.264, RTP / SCTP | 53,0 / 46,7 ms | 47,1 / 47,2 ms |
| HEVC, RTP / SCTP | 44,3 / 38,6 ms | 47,8 / 46,7 ms |
| AV1, RTP / SCTP | 52,2 / 46,5 ms | 59,9 / 48,1 ms |

Sur l'hôte natif, les clics ne passent qu'à 2-7 sur 30, en RTP comme en SCTP
(le drapeau sur DISPLAY1 n'est pas lu) : on ne s'y fie pas. L'H.264 et l'AV1 de
l'Arc encodent lentement (11 ms par image en H.264), ce qui gonfle l'âge des
deux transports de la même façon.

**Ultra (PyroWave simulé, `ultra=synthetic:250`, ~122 Mbit/s à 60 i/s), la
vidéo HEVC à côté**, deux manches :

| Transport d'Ultra et de la vidéo | Ultra reçu | Attente en plus d'Ultra, p50 / p95 | Vidéo hôte → affiché | i/s vidéo |
|---|---|---|---|---|
| RTP | 119-122 Mbit/s, 0 perte | 15 / 25-30 ms | 19,7 / 21,0 ms | 57-60 |
| SCTP | 122 Mbit/s, 0 perte | 35-52 / 62-90 ms | 42,5 / 64,8 ms | 59-60 |

- **Sans charge, RTP coûte 5 à 10 ms**, sur les trois codecs et sur les deux
  types d'hôte. Ce retard est fixe, quelle que soit la taille de l'image, et ce
  n'est pas l'hôte : il envoie l'image 4 ms après la capture dans les deux cas,
  et `sendFrame` prend 0,37 ms. Le saut du worker vers la page prend 0,2 ms.
  **C'est Chrome qui retient l'image** entre l'arrivée de son dernier paquet
  (`receiveTime` de ses métadonnées) et sa remise au transform : 7,5 ms en
  médiane, p90 14 ms, au plus 17 ms, réparties uniformément sur une période de
  60 Hz. On dirait une cadence interne de Chrome. `jitterBufferTarget = 0`
  n'y change rien, ni `--disable-features=WebRtcMetronome`. La cause exacte
  reste à trouver.
- **Avec une charge Ultra, RTP gagne nettement.** En SCTP, la vidéo attend
  derrière Ultra dans la même association (42-65 ms de l'hôte à l'affichage).
  En RTP, chaque piste a son propre chemin : la vidéo reste à 20 ms et Ultra
  attend trois fois moins.
- **Aucune perte n'a été vue sur ce chemin Wi-Fi**, en RTP comme en SCTP : les
  cas de pertes restent à mesurer, avec pertes injectées.
- Recommandation provisoire : SCTP reste le défaut pour H.264, HEVC et AV1 tant
  que la retenue de Chrome n'est pas levée. Pour Ultra, RTP est le bon
  transport : il porte PyroWave sans faire attendre la vidéo.

À refaire sur câble ce soir : le même jeu de passes, plus une passe de base en
`sctpburst=0` avec l'UM790Pro pour client (demande de la session Wi-Fi).

### 6.11 U1.4 ter : le métronome de Chrome, et la route audio (05/10/2026, 12:40-14:25)

Demande de Bruno : « il y a un gain énorme à faire sur ce point ». Même chemin,
mêmes machines, HEVC sur l'hôte natif sauf mention contraire.
`network-latency-findings.md`, `b01d9454`. Code : `36f1a492`, `c83acc30`.

**La cause.** Chrome remet les images vidéo reçues au transform sur une grille
fixe de 15,625 ms, soit 64 fois par seconde :
- **La preuve par la mesure.** Les heures de remise se calent sur cette période
  (cohérence de phase 0,98). Les heures d'arrivée des paquets ne s'y calent pas
  (0,01-0,07). Les images du DataChannel, sur le même socket, non plus.
- **La source.** `VideoMetronomeWorker`, dans
  `rtc_encoded_video_stream_transformer.cc`, met chaque image en file jusqu'au
  prochain tick d'un métronome à 64 Hz. Il est actif par défaut depuis 2024,
  pour réveiller moins souvent le JavaScript des grosses visios. Son
  interrupteur (`RTCAlignReceivedEncodedVideoTransforms`) a été retiré en
  octobre 2025.
- **Ce qui n'y change rien :**
  - l'ancienne API `createEncodedStreams`, sur le thread de la page ;
  - les réglages de minuteur de Chrome.
- **`VSyncDecoding` empire les choses :** 50 ms de retenue.
- **Les images audio ne sont pas retenues.**

**Le contournement : la « route audio ».** L'item `aroad` de `rtp_video` (banc
seulement) découpe chaque image en paquets Opus sur une piste audio, avec 8
octets d'en-tête. Le worker du transform les réassemble. La retenue de Chrome
tombe de 7,8 ms à 0,2 ms.

| Hôte → affiché, médiane | Piste vidéo RTP | Route audio | SCTP |
|---|---|---|---|
| HEVC, sans charge | 17,3 / 18,1 ms | 9,1 / 9,3 / 10,1 ms | 9,6 / 9,8 ms |
| H.264, sans charge | 23,7 ms | 14,6 ms | 14,8 ms |
| AV1, sans charge (40 i/s seulement, à refaire) | 30,9 ms | 31,1 ms | 21,1 ms |
| HEVC sous Ultra 250 (~122 Mbit/s) | 21,4 ms | 11,1-11,6 ms | 123-129 ms |

Sous la charge Ultra, l'attente en plus d'Ultra, en médiane / p95, est de :
- 16,8 / 41,4 ms sur une piste vidéo ;
- 7,2 / 22,3 ms sur la route audio ;
- 141-155 / 185-200 ms en SCTP.

- **La route audio efface toute la pénalité du RTP** : sans charge, elle fait
  jeu égal avec SCTP. Sous charge, elle garde la vidéo presque aussi fraîche
  que sans charge, et elle rend Ultra deux fois plus réactif que la piste
  vidéo.
- **Il lui manque la retransmission.** Chrome n'envoie pas de NACK pour une
  piste audio dont il ne joue rien. Sous 122 Mbit/s, il y a eu 10 demandes
  d'image clé en 2 minutes. Prochaine étape (U1.4 quater) : un NACK à nous,
  qui envoie au hôte les morceaux manquants par le canal d'entrée.
- **Le Wi-Fi 7 du banc est partagé avec la maison.** De 13h27 à 13h55, un
  iPhone mal capté streamait de la vidéo par rafales :
  - Ultra en RTP est tombé de 122 à 78-109 Mbit/s, avec des pertes ;
  - la sonde UDP cadencée restait propre jusqu'à ~200 Mbit/s ;
  - une fois l'iPhone arrêté, les chiffres du matin sont revenus.

  Les passes sous charge de 14h10-14h25 ont été faites sans lui.

**U1.4 quater, le NACK de la route audio (14h35-15h10, `a95e95d9` ; constats
`f4ad0dfd`).** Préférence de Bruno : la latence passe avant la qualité, et une
image partiellement dégradée quelques secondes est acceptable.

Le fonctionnement :
- Le worker redemande un morceau manquant dès qu'il voit le trou, par le canal
  d'entrée.
- L'hôte le renvoie depuis un historique des 60 dernières images.
- Une image vidéo complète attend au plus 15 ms une image plus ancienne, puis
  part marquée perdue.
- `MW_AROAD_DROP` (pour mille) jette des morceaux au premier envoi, au banc
  seulement.

| HEVC à vide, hôte → affiché | p50 | p90 | i/s affichées | Images abandonnées |
|---|---|---|---|---|
| Route audio, sans perte | 9,1-9,3 ms | 13,2 ms | 59,4-59,7 | — |
| Route audio, 1 % de morceaux jetés | 9,7 ms | 14,5 ms | 59,9 | 0 |
| Route audio, 5 % de morceaux jetés | 11,8 ms | 16,3 ms | 59,4 | 0 |
| SCTP, `loss=50` (5 % des messages jetés avant SCTP) | 8,4 ms | 11,2 ms | 38 | — |

- La route audio répare 5 % de pertes pour 2,5 ms de plus, sans perdre une
  seule image.
- Le DataChannel, lui, n'affiche plus qu'une image sur deux ou trois et
  demande 153 reprises en 2 minutes. Ses 8,4 ms ne comptent que les images
  arrivées intactes.
- Sous Ultra 250, avec la vidéo à côté, un morceau vidéo renvoyé attend
  derrière les rafales d'Ultra et dépasse les 15 ms : il reste 11 demandes
  d'image clé en 2 minutes. Dans le produit, Ultra remplace la vidéo au lieu de
  rouler à côté, ce cas n'existe donc qu'au banc.
- **AV1 refait** (deux manches), hôte → affiché en médiane :
  - route audio : 30,0 / 18,0 ms ;
  - SCTP : 32,9 / 31,7 ms ;
  - piste vidéo : 39,9 / 38,8 ms.

  Le 21 ms de SCTP mesuré plus tôt était du bruit.

**Recommandation provisoire, à confirmer sur câble :**
- la route audio, plutôt que la piste vidéo, pour la case RTP de l'interrupteur
  U1.4, pour les trois codecs et pour Ultra ;
- SCTP reste le défaut tant que Bruno n'a pas basculé les cases.

**À corriger avant de passer la route audio au produit hors Windows**
(constat de la session audio + DSCP, 05/10). libdatachannel marque chaque
paquet d'une piste « audio » en EF, DSCP 46 (`track.cpp:215-220`), et les
autres pistes en AF42. Sur un hôte Linux ou macOS, toute la vidéo de la route
audio partirait donc en EF. Un point d'accès qui suit la RFC 8325 la range dans
la file voix du Wi-Fi, qui n'agrège pas les trames : son débit s'effondre, et
la vraie voix de la maison en pâtit. Sous Windows, libjuice ne marque rien, et
les bancs sur DualRTX ne sont pas touchés. Il faut que la route audio porte une
marque vidéo (AF4x) et que seul le vrai son garde EF. Le plan DSCP choisira les
marques.

Nuance mesurée le 06/10 sur la Freebox du banc (D0, constats réseau
`559ed14e`, §3). La Freebox ne suit pas la RFC 8325 :
- quand le câble de l'hôte et le Wi-Fi du client sont sur le même appareil,
  tout part en BE, quel que soit le DSCP ;
- derrière un répéteur lointain (celui du N95, par exemple), elle applique
  l'ancienne règle, file = DSCP >> 3. La route audio en EF part donc en VI,
  comme la piste vidéo RTP en AF42, et non en VO. Mais le SCTP en AF11 part
  en BK, la file de fond.

Sur un hôte Linux ou macOS, une comparaison des routes vers un client
lointain mêle donc la différence de file à celle du transport : SCTP en BK
contre la route audio en VI. La correction reste due pour un point d'accès
qui suit la RFC 8325.

### 6.12 U2.4 : le PyroWave de référence, l'oracle des portages (05/10/2026, soir)

Priorité donnée par Bruno, le 05/10 au soir : « compléter l'implémentation de
PyroWave, je veux savoir les résultats ». L'ordre retenu avec le coordinateur :
l'oracle, puis le décodeur WebGPU (U3.4), puis l'encodeur HLSL (U2.5), puis
l'intégration U4.

**L'outil** (`e6d2a275`). `mw-pyrowave-ref` compile le PyroWave de l'amont
(Hans-Kristian Arntzen, MIT, commit `509e4f88`), en Vulkan, depuis l'arbre
vendorisé par punktfunk. Cet arbre ajoute des correctifs qui ne changent pas le
flux. L'outil encode et décode des Y4M 4:2:0, et mesure le PSNR. Il n'entre ni
dans le build du produit, ni dans l'installeur.
- `scripts/bench/ultra/make_corpus.py` : un lot d'images de test en 1080p,
  60 images par clip, rangé hors du dépôt. Il comprend du texte qui défile,
  des dégradés, un damier fin avec des lignes de 1 px, du bruit, et 60 images
  du clip de jeu du banc (`cod.webm`). Rien n'est capturé à l'écran.
- `scripts/bench/ultra/oracle.py` : passe tout le lot par GPU et par débit, et
  garde les flux et les images décodées pour les portages.

**Ce que l'oracle dit déjà** (temps GPU de la bibliothèque, par image,
1080p, en ms) :

| Clip | Débit | PSNR-Y (RTX / iGPU AMD) | Encodage RTX / AMD | Décodage RTX / AMD |
|---|---|---|---|---|
| Jeu | 170 Mbit/s | 51,9 / 50,3 dB | 0,14 / 1,9 | 0,10 / 1,7 |
| Texte | 170 Mbit/s | 33,4 / 33,3 dB | 0,15 / 2,2 | 0,10 / 1,2 |
| Texte | 250 Mbit/s | 44,5 / 43,9 dB | 0,16 / 2,2 | 0,10 / 1,4 |
| Dégradés | ≤ 97 Mbit/s (n'a pas besoin de plus) | 67,0 / 54,3 dB | 0,10 / 1,3 | 0,10 / 1,3 |
| Damier, bruit | 170 Mbit/s | 16-17 dB (incompressibles) | 0,21-0,23 / 2,9-3,6 | 0,10 / 1,4 |

- **Sur la RTX, le codec est pratiquement gratuit** : 0,1-0,24 ms d'encodage,
  0,1 ms de décodage, contre 1,4-2 ms pour NVENC (§6.6).
- **Sur l'iGPU AMD, il coûte 1,3-3,7 ms à l'encodage**, contre 4-4,3 ms pour
  AMF. Le gain sur cet hôte est donc de 1 à 3 ms, au bas de la marge du rapport
  U0.5. Les temps viennent d'images isolées sur un GPU au repos : ils seront
  repris en continu.
- **Le texte est le cas dur.** 33 dB à 170 Mbit/s, 44,5 dB à 250 : un texte
  fin demande plus que le seuil subjectif publié. Le jugement à l'œil de Bruno
  (U2) devra porter sur du texte.
- **L'encodeur Vulkan de l'amont est faux sur l'Arc A380** : la moitié droite
  de l'image sort grise (PSNR-Y 10 dB). Le décodeur de l'Arc lit bien le flux
  de la RTX, c'est donc l'encodeur qui est en cause, sans doute ses tailles de
  sous-groupe. L'oracle tourne donc sur la RTX. Le portage HLSL (U2.5) devra
  être vérifié à part sur l'Arc.
- **Un piège de la machine** : une couche Vulkan implicite (de capture)
  plante la création du périphérique. L'outil coupe les couches implicites.

### 6.13 U3.4 : le décodeur PyroWave dans Chrome, en WebGPU (05/10/2026, soir)

**Le décodeur** (`96383922`, `95c71a44`) : `frontend/js/stream/ultra/PyroWaveDecoder.js`,
un portage en WGSL du décodeur de l'amont (MIT, en-tête et provenance dans le
fichier), sans `subgroups`.
- Les sommes cumulées passent par la mémoire du groupe de travail. Le même code
  vaut donc aussi pour Safari et les mobiles.
- Les coefficients sont dans un seul tampon f32.
- La transformée inverse travaille par tuiles de 32×32 en mémoire partagée, en
  une passe par niveau.
- Le bord : l'échantillonneur miroir de l'amont, avec ses décalages, revient à
  l'extension symétrique de JPEG 2000 sur le signal entrelacé. Le portage la
  calcule directement.

Le labo est `scripts/bench/ultra/decoder-lab.html`, avec son pilote
`decoder_lab.py`. Il tourne dans un Chrome headless à lui, sans aucune fenêtre,
et vise un GPU par `--use-adapter-luid`.

| 1080p, 170 Mbit/s | Écart à l'oracle | Décodage GPU dans Chrome (p50) | Amont en Vulkan (dequant + iDWT) |
|---|---|---|---|
| RTX 5060 Ti | ≤ 1 code, 5 clips × 3 débits | 0,17 ms | 0,10 ms |
| Arc A380 | ≤ 1 code | 1,16 ms | 0,43 ms |
| iGPU AMD (2 CU) | ≤ 1 code | 5,1 ms | 1,3-1,7 ms |

- **Le jalon est tenu** (cible : ±2 codes). La justesse est la même sur les trois
  GPU.
- **La vitesse est bonne sur une carte dédiée, à reprendre sur un petit GPU.**
  Sur l'iGPU AMD, la déquantification égale l'amont (1,1 ms contre 0,6-1,0).
  La transformée inverse, elle, coûte 4 ms contre 0,7. Ni les barrières entre
  passes, ni les fréquences de repos, ni les contrôles de bornes de Chrome n'en
  sont la cause. C'est le nombre d'opérations par échantillon : l'amont lit
  4 texels d'un coup par `textureGather`, laisse le matériel faire le miroir,
  et calcule en FP16. Ce sera la piste à suivre, à mesurer d'abord sur le 780M
  de l'UM790Pro (12 CU), un vrai client Ultra.
- **Sans les contrôles de bornes de Chrome** (`disable_robustness`), la version
  committée reste juste, sur la RTX comme sur l'AMD. Elle ne fait donc aucun
  accès hors limites. Une version intermédiaire, sans tuiles, sortait faux
  ainsi.
- **La conversion en 8 bits** (0,6 ms sur l'AMD) ne sert qu'au labo. Le
  produit dessinera directement depuis les plans f32.
- **Sur le 780M de l'UM790Pro** (05/10, 21:10), Chrome headless dans la
  session minis, sans fenêtre : au plus 1 code d'écart avec l'oracle, et 2,0 ms
  par image 1080p. Ce temps se répartit en 1,44 ms d'iDWT, environ 0,35 ms de
  déquantification et 0,21 ms de conversion, qui ne sert qu'au labo. Dans le
  produit, le décodage prendrait donc environ 1,8 ms : il tient dans le budget
  de ce client.

### 6.14 U2.5 : l'encodeur PyroWave en HLSL sur D3D12 (05/10/2026, soir)

**L'encodeur** (`9c4ddb39`) :
`backend/native-host/tools/pyrowave-d3d12/src/PyroWaveEncoder12.{h,cpp,hlsl}`.
C'est la classe que reprendra l'encodeur Ultra du moteur (U4). Elle porte les
six passes de l'amont : DWT, quantification, analyse et résolution du débit,
assemblage. Le découpage en paquets se fait côté CPU, comme dans l'amont.
- **SM 5.0, sans instruction de vague.** Les opérations de sous-groupe de
  l'amont deviennent un thread par bloc 8×8 (la quantification) ou par bloc
  32×32 (l'analyse et l'assemblage), qui travaille en série. Rien ne dépend de
  la taille des vagues, alors que l'encodeur Vulkan de l'amont est faux sur
  l'Arc. Le tout compile avec le `d3dcompiler` que le moteur utilise déjà.
- **Ce qui diffère de l'amont.** Les coefficients sont en f32, et chaque
  sous-bloc 4×2 a un emplacement fixe de 16 octets, sans allocation atomique.
  Le flux n'est donc pas identique octet pour octet, mais sa qualité est la
  même.
- **Un outil autonome**, avec son propre dossier de build. Il ne touche pas à
  `build/`, dont se servent les `--dev` des autres sessions.

**Vérifié sur WARP** (le D3D12 logiciel), à 170 Mbit/s, sur 10 images :

| Clip | PSNR-Y contre la source | Amont (oracle) | Débit obtenu |
|---|---|---|---|
| Texte | 33,39 dB | 33,38 dB | 169,97 Mbit/s |
| Jeu | 51,64 dB | 51,63 dB | 169,98 Mbit/s |

- Les 20 920 blocs ont exactement la taille qu'ils annoncent, et le décompte de
  l'en-tête de trame est juste. Le décodeur WebGPU (§6.13) les lit.
- La couche de débogage, avec la validation côté GPU, ne signale rien, à 170
  comme à 60 Mbit/s.

**Un incident, et ce qu'il a appris.** Le premier essai sur la RTX a provoqué
sept réinitialisations du pilote NVIDIA (TDR, 20:27-20:28). La RTX pilote
l'écran principal de Bruno. La prod et la `--dev` de la session Wi-Fi ont
survécu. Deux défauts en étaient la cause :
- **Une lecture hors du tampon.** Un descripteur racine n'a pas de taille, donc
  rien ne borne un accès, et le compilateur peut exécuter les deux côtés d'une
  branche. Les niveaux de la DWT qui lisent un plan LL calculaient quand même
  une adresse dans la source 8 bits, hors de celle-ci. Chaque adresse est
  désormais bornée dans son tampon.
- **Un décalage variable de 24 bits qui donnait 0** (d3dcompiler + WARP). Les
  octets se placent désormais par des décalages constants. Un auto-test
  (`SelfTestCS`, `--selftest`) garde le cas.

Depuis, l'outil tourne sur WARP sauf si `--vendor` désigne un GPU. Le passage
sur un vrai GPU attend le feu vert du coordinateur.

**Les temps GPU** (20:41-20:42, au feu vert du coordinateur, un GPU à la fois,
`--repeat 50` sur 10 images 1080p à 170 Mbit/s). Aucune erreur D3D12, et
aucun événement de pilote dans le journal Système après les passes.

| GPU | Encodage p50 (p99) | Dont DWT / quantification / assemblage | Amont en Vulkan | Encodeur matériel du produit |
|---|---|---|---|---|
| RTX 5060 Ti | 0,63-0,68 ms (0,66-0,73) | 0,15 / 0,11-0,14 / 0,24-0,32 | 0,14 ms | NVENC 1,4-2 ms |
| iGPU AMD (2 CU) | 8,0-8,2 ms (8,5-8,7) | 4,5 / 1,2-1,6 / 1,2-1,4 | ~2-3 ms | AMF 4-4,3 ms |
| Arc A380 | 4,4-4,5 ms (4,5-7,8) | 0,7 / 2,8-6,1 / 0,5-0,6 | faux (§6.12) | VE 3,5-4,5 ms |

- **Sur la RTX, c'est déjà moins que NVENC**, mais quatre fois l'amont : les
  passes en série (un thread par bloc) et l'assemblage octet par octet coûtent.
- **Sur l'Arc (22:19, au feu vert), c'est à peu près la vitesse de VE.** La
  quantification en prend l'essentiel : un thread par bloc 8×8, qui boucle en
  série, convient mal aux GPU Intel.
- **Sur le petit iGPU AMD, c'est plus lent qu'AMF.** Comme pour le décodeur
  (§6.13), ce GPU à 2 CU est limité par le calcul, et la DWT en prend la
  moitié. Paralléliser les passes en série et alléger la DWT est la suite côté
  hôte, à mesurer aussi sur le 780M.

**Reste** : l'intégration (U4) : l'encodeur dans le moteur, la
route audio comme transport, et le décodeur de §6.13 dans le client.

### 6.15 U4 : PyroWave dans MoonlightWeb, derrière des clés cachées (05/10/2026, nuit)

Écrit sans banc ni GPU réel. La première mesure de bout en bout attend le feu
vert du coordinateur.

**L'hôte** (`3c1e908a`, `e28183a2`, `e27da614`).
- `PyroWaveEncoder12` est désormais un encodeur du moteur
  (`src/encode/windows/d3d12/`). L'outil de labo compile le même fichier.
- `UltraEncoder12` est un troisième `IVideoEncoder12` de la route D3D12, à côté
  de VE, NVENC et AMF.
  - Il attend la conversion sur le GPU, puis copie les deux plans de l'image
    NV12 dans un tampon, à leurs empreintes de copie.
  - Il encode sur une file COMPUTE à lui, avec la priorité que le moteur donne
    à ses files.
  - Il rend l'image comme une image clé : tous ses paquets PyroWave bout à
    bout. Une demande d'image clé ne lui coûte rien, et une image perdue ne
    demande aucune réparation.
- La DWT lit la chroma entrelacée du NV12 (`kind` 2). Sur WARP, le flux est
  identique octet pour octet à celui de la source en plans séparés, et la
  validation côté GPU ne signale rien.
- Les clés de banc : `pipeline=d3d12,enc12=pyrowave`, et `ultrambps=<Mbit/s>`
  (170 par défaut). La route D3D12 l'accepte sur ses trois GPU : il n'y a ni
  codec à négocier, ni vague de rafraîchissement à obtenir. Les tests natifs de
  choix de route le couvrent.
- Le transport ne change pas : c'est la route audio, avec le numéro d'image
  (`native:hevc+aroad`).

**La page** (`82ba6d5f`, `216c690f`).
- `PyroWaveDecoder.present()` dessine l'image en RGB (BT.709, plage limitée)
  dans un canevas WebGPU. Le chemin complet décodage → OffscreenCanvas →
  `VideoFrame` → Canvas2D reste à 3 niveaux près de la conversion faite en
  JavaScript sur l'image de l'oracle.
- `UltraPlayer` garde une seule image en vol, et la plus fraîche en attente
  gagne. Il rend une `VideoFrame`.
- Avec `localStorage mw_ultra=pyrowave`, StreamView dimensionne le lecteur
  d'après le premier en-tête de début de trame. Il fait ensuite passer ses
  images par `onDecodedFrame` : la cadence, le présentateur Canvas2D, le
  journal par image et la sonde de latence marchent sans changement.

**La première passe proposée** : la `--dev` de `build/` (seul exe à avoir sa
règle de pare-feu), avec `MW_NATIVE_TUNING=pipeline=d3d12,enc12=pyrowave` et
`MW_RTP_VIDEO=native:hevc+aroad`, l'hôte RTX, et Chrome sur l'UM790Pro avec
`mw_ultra=pyrowave`. Puis la même passe en HEVC, sur le même chemin, pour
comparer.

### 6.16 Sur un vrai câble : NetProbe, U1.2, U1.4 et UA.4 rejoués (06/10/2026, 14:34-15:30)

DualRTX hôte, l'UM790Pro client sous Windows, en câble seul sur le commutateur
de DualRTX (`Ethernet 2`, 1 Gbit/s). `build/` de 12:10. Tout passe sur l'écran
virtuel du produit à 120 Hz (240 Hz pour U1.2 et le mode U), et non sur l'écran
physique de l'Arc comme au §6.10. Il y a eu 23 passes, toutes enregistrées
(`scratchpad/cable_run.sh`, noms `u1c-*`, `u14c-*`, `u03-umcab-*`).

**Le chemin (NetProbe).** Aller-retour UDP à vide p50 0,38 ms (2,6 ms par le
Wi-Fi 7), TCP 948 Mbit/s sur un ou quatre flux (74-90). L'UDP cadencé passe
sans perte jusqu'à 500 Mbit/s, avec un délai en plus p99 ≤ 2,8 ms ; à
800 Mbit/s, 0,4 % de pertes. Le commutateur virtuel Hyper-V de DualRTX ne
compte pas. Le plafond de 150 Mbit/s du §6.10 venait bien du saut Wi-Fi.

**U1.2, le DataChannel à haut débit** (source synthétique, 120 i/s) :

| Taille | Demandé | Porté | Âge de la vidéo à côté |
|---|---|---|---|
| 350 Kio | 344 Mbit/s | 85 Mbit/s | 697 ms, RTT 151 ms |
| 1 000 Kio | 983 Mbit/s | 528 Mbit/s | 257 ms |
| 2 000 Kio | 1,97 Gbit/s | 534 Mbit/s, 9 perdues | — |
| 1 000 Kio, canal non ordonné | | 524 Mbit/s | délai p95 365 ms (527) |
| 1 000 Kio, `sctpburst=10` | | 513 Mbit/s | délai p95 376 ms |

- L'association SCTP porte jusqu'à ~530 Mbit/s, trois fois les 170 de
  PyroWave. Au-delà de ce qu'elle porte, la vidéo qui partage l'association
  attend des centaines de millisecondes.
- La passe à 350 Kio s'est effondrée à 85 Mbit/s : elle demandait pourtant
  moins que ce que 1 000 Kio a porté. C'est la seule passe de ce genre ;
  elle est à refaire, avec 200 Kio, avant d'en tirer quoi que ce soit.
- Ultra 250 Kio à 60 i/s (123 Mbit/s, ci-dessous) tient sans gêner la vidéo.

**U1.4, les trois transports** (médianes ; âge = âge du contenu montré,
hôte → dessin = journal par image ; deux manches, sauf la route audio sous
Ultra) :

| | SCTP | Piste vidéo RTP | Route audio |
|---|---|---|---|
| HEVC seul, âge | 23,1 / 23,2 ms | 32,9 / 31,6 ms | 23,3 / 23,1 ms |
| HEVC seul, hôte → dessin | 8,8 / 8,7 ms | 19,2 / 18,7 ms | 8,7 / 9,1 ms |
| HEVC seul, clic | 47,3 / 47,1 ms | 55,2 / 59,6 ms | 48,7 / 42,2 ms |
| Ultra 123 Mbit/s à côté, âge | 23,3 / 22,1 / 22,7 ms | 31,3 / 31,6 ms | 22,5 ms |
| Ultra, délai en plus du flux Ultra p50 | 7,0 / 5,9 / 8,0 ms | 13,4 / 16,3 ms | 6,6 ms |

- Sur le câble, SCTP ne pénalise plus la vidéo quand Ultra passe à côté (en
  Wi-Fi : 125 ms, §6.11). La route audio fait jeu égal avec lui.
- La piste vidéo garde ses 9-10 ms de retenue (le métronome de Chrome, §6.11),
  et Chrome y envoie toujours ses PLI en boucle.

**5 % de pertes injectées (HEVC)** :

| | Âge | Images dessinées par seconde | Clics vus | Perdus |
|---|---|---|---|---|
| Route audio (`MW_AROAD_DROP=50`) | 24,5 ms | 58,7 | 29/30 | 0 % |
| SCTP (`loss=50`) | 22,6 ms | 31,9 | 20/30 | 6,8 % |

La réparation de la route audio coûte +1,3 ms et ne perd rien. SCTP en perd
une sur deux, comme en Wi-Fi.

**UA.4, l'Auto détecté sur le câble** (cellule Arc, deux manches) :

| | Mode | Âge montré | p99 |
|---|---|---|---|
| D | Auto détecté | 14,4 / 14,6 ms | 21,7 / 22,0 ms |
| U | 120 i/s forcés, écran virtuel à 240 Hz | 19,4 / 18,5 ms | 32,1 / 25,2 ms |

L'Auto monte à 120 i/s et bat l'Ultra forcé de 4-5 ms.

**Ce que ça tranche.**
- Pour U1 : sur 1 GbE, le DataChannel a la place de PyroWave. Le plafond du
  §6.8 venait du chemin, pas de SCTP.
- La recommandation du §6.11 tient. La route audio devient la case RTP de
  l'interrupteur. Elle égale SCTP sur un lien propre, ne perd rien sous
  pertes, et ne retient rien comme la piste vidéo.
- Reste à comprendre la passe à 350 Kio.

### 6.17 PyroWave de bout en bout, contre HEVC, sur le câble (06/10/2026, 18:30-19:15)

Même banc qu'au §6.16 : l'hôte DualRTX encode sur la RTX, l'écran virtuel du
produit est en 2560×1440 à 120 Hz, le flux à 60 i/s, et le client est
l'UM790Pro sur le câble. PyroWave passe par les clés du §6.15 :
`pipeline=d3d12,enc12=pyrowave` côté hôte, `mw_ultra=pyrowave` côté page. Il
envoie des images de ~354 Ko, soit ~155 Mbit/s. La série compte deux manches
alternées et 30 clics par passe (`scratchpad/pw_run.sh`, noms `u14p-*`).

| Médianes | HEVC, route audio | PyroWave, route audio | HEVC, SCTP | PyroWave, SCTP |
|---|---|---|---|---|
| Hôte → dessin | 7,2 / 7,2 ms | 15,9 / 15,7 ms | 7,0 / 7,1 ms | 17,6 / 15,9 ms |
| Clic | 42,9 / 41,2 ms | 48,2 / 48,3 ms | 42,7 / 42,0 ms | 54,8 / 51,2 ms |
| Âge montré | 25,6 / 25,7 ms | 35,3 / 26,1 ms | 25,4 / 8,9 ms | 35,9 / 26,3 ms |
| Images dessinées par seconde | 59,9 | 60 / 59,9 | 60 | 60 |

- PyroWave marche sur les deux routes, sans perte et à 60 i/s. Il coûte
  pourtant ~9 ms de plus que HEVC de l'hôte au dessin, et 6-10 ms de plus au
  clic.
- L'âge montré dépend de la phase de l'écran : il varie de 9 à 26 ms d'une
  manche à l'autre pour le même HEVC. Il ne départage pas les deux codecs ici.
- **La piste vidéo RTP ne porte pas PyroWave** (deux passes, aucune
  enregistrée). L'hôte y emballe l'image comme du HEVC : il la découpe aux
  codes de début de NAL. Le flux d'ondelettes contient de faux codes de début,
  et l'hôte les coupe (`[HEVC-PATCH] NAL[0..19]` sur un même paquet). Il
  faudrait un emballage propre à PyroWave. La route audio fait déjà mieux que
  cette piste en HEVC (piste vidéo : 22 ms de l'hôte au dessin).
- Piège du banc : `mw_ultra=pyrowave` reste dans le profil du Chrome de banc
  d'une passe à l'autre. Une passe HEVC qui suit doit poser `mw_ultra=off`,
  sinon la page lit du HEVC comme du PyroWave et ne dessine rien.

Reste à expliquer les ~9 ms : le temps d'encodage sur la RTX, le décodage
WebGPU sur la 780M, la taille des images, ou le chemin de présentation de
`UltraPlayer`.

### 6.18 D'où viennent les ~9 ms de PyroWave (06/10/2026, 19:20-19:35)

**Le découpage par image** se lit dans les relevés du §6.17, sans nouvelle
passe (`scratchpad/pw_split.py`). Les postes sont :
- l'encodage : de la capture à la remise au relais, sur l'horloge de l'hôte ;
- le trajet : de cette remise à l'arrivée de l'image entière dans la page. Il
  comprend l'envoi, le câble et Chrome ;
- le décodage : de l'arrivée à l'image décodée ;
- le dessin.

| Médianes (p90) | HEVC | PyroWave | Écart |
|---|---|---|---|
| Encodage (RTX) | 3,8 ms (4,5) | 2,5 ms (3,5) | −1,3 ms |
| Trajet | 1,7 ms (3,7) | 5,3-7,0 ms (10) | +3,5 à +5 ms |
| Décodage (780M) | 0,7 ms (1,1) | 6,9 ms (8,5) | **+6,2 ms** |
| File et dessin | 0,3 ms | 0,2 ms | 0 |

- **L'encodeur n'est pas en cause** : il est plus rapide que NVENC.
- **Le trajet tient à la taille.** Une image de 354 Ko demande 2,8 ms de
  sérialisation sur 1 GbE, quel que soit le transport. L'image HEVC est 5 à
  10 fois plus petite. Comme l'image part entière puis se décode entière, ce
  temps s'ajoute aux autres au lieu de se recouvrir avec eux.
- **Le décodage est le premier poste.**

**Le décodage, poste par poste.** `UltraPlayer` note maintenant chaque étape.
Le GPU est mesuré par timestamp-query, une image sur huit. Le relevé est dans
`globalThis.__mwUltraPlayer.summary()`, que `pass.py` range sous
`ultraPlayer`. Deux passes (`u14q-*`), médianes :

| | Route audio | SCTP |
|---|---|---|
| Lecture des paquets | 0,1 ms | 0,1 ms |
| Préparation et envoi du travail | 0,2 ms | 0,2 ms |
| Envoi → `onSubmittedWorkDone` | 6,1 ms | 6,4 ms |
| dont GPU, décodage (déquantification + iDWT) | 3,15 ms | 3,15 ms |
| dont GPU, affichage en RGB | 0,59 ms | 0,59 ms |
| VideoFrame depuis le canevas | 0,1 ms | 0,1 ms |
| Décodage complet, vu du journal | 6,5 ms | 6,7 ms |

- **La passe « pack » est retirée.** La conversion en 8 bits ne sert qu'au labo
  (§6.13), puisque l'affichage lit les plans f32. Le décodage gagne environ
  0,4 ms (6,9 → 6,5-6,7 ms).
- **Le GPU travaille 3,7 ms sur les 6,1 à 6,4 ms d'attente.** À 1440p, 3,15 ms
  de décodage correspondent bien aux 1,8 ms du labo à 1080p (§6.13).
- **Rendre l'image dès l'envoi ne gagne rien à l'écran.** Interrupteur de banc
  `mw_ultra_early=1` : la VideoFrame part à la page dès la soumission, sans
  attendre `onSubmittedWorkDone`.
  - L'hôte → dessin tombe à 8,1 ms (route audio) et 9,4 ms (SCTP).
  - Le clic, lui, ne bouge pas : 45,4 contre 41,6 ms, et 48,0 contre 48,2 ms.
  - Le gain n'était qu'une heure notée plus tôt : le dessin attend de toute
    façon la fin du GPU. Les ~2,4 ms hors timestamps ne sont donc pas seulement
    un rappel tardif. L'interrupteur reste éteint par défaut.
  - Piège du banc : `mw_ultra_early` reste lui aussi dans le profil du Chrome
    de banc. Les passes suivantes posent `mw_ultra_early=0`.
- **Le clic départage moins que l'hôte → dessin.** Sur ces quatre passes, il
  vaut 41,6 à 48,2 ms, et la route audio égale déjà le HEVC (41-43 ms). À
  30 clics par passe, un écart de ±4 ms reste du bruit.

**Ce que disent les sources** (recherche du 06/10 : le blog de l'auteur, les
dépôts `pyrowave` et `pyrofling`, la PR #355 de Nova, Hacker News, la doc
NVENC et GeForce NOW). Notre résultat est cohérent, pas anormal.
- **Les chiffres de l'auteur sont ceux d'un gros GPU en natif** : sur une
  RX 9070 XT, 0,13 ms d'encodage à 1080p et moins de 0,1 ms de décodage. Il
  ne publie aucune mesure de bout en bout. Son client `pyrofling` est un client
  natif Vulkan, qui prend la dernière image prête à chaque cycle, sans
  décodage par tranches.
- **Le gain promis se fait surtout côté client**, contre un décodeur matériel
  qui retient des images. Sur une Retroid Pocket 6 en 1080p60, Nova mesure 1,9
  à 2,7 ms en PyroWave contre 11 ms en HEVC. S'y ajoutent l'absence de file et
  de lookahead dans l'encodeur, un temps fixe, et des pertes qui ne coûtent
  qu'un bloc flou.
- **Notre HEVC est déjà dans le cas favorable** : 3,8 ms de NVENC et 0,7 ms de
  WebCodecs sans retenue. Il n'y a presque plus rien à gagner, alors que
  PyroWave paie 2,8 ms de sérialisation et un décodage WebGPU sur un iGPU.
- **GeForce NOW joue surtout sur la cadence** : 240 et 360 i/s, Reflex côté
  serveur, Adaptive Sync. NVENC sait aussi rendre la main par tranche
  (`enableSubFrameWrite`).

**Pistes, par gain attendu :**
1. **Envoyer et décoder par tranches.** Le format s'y prête (blocs 32×32
   indépendants). La sérialisation se recouvrirait alors avec l'encodage et le
   décodage : 2 à 4 ms attendues.
2. **Accélérer l'iDWT en WGSL** : lecture de 4 texels à la fois, FP16
   (§6.13). Sur un petit GPU, c'est 4 fois l'amont : de l'ordre de 1,5 ms à
   gagner.
3. **Passer à 120 i/s à débit égal**, avec des images deux fois plus petites.
   Ou baisser le débit à ~100 Mbit/s.
4. Même tout cela fait, PyroWave rejoindrait le HEVC sur ce client (7 à 9 ms)
   sans le battre nettement. Son terrain reste les clients dont le décodeur
   retient des images (TV, mobiles, certains Mac) et les liens avec pertes.

### 6.19 PyroWave à 120 images/s : il passe devant le HEVC (06/10/2026, 19:40)

Même banc qu'au §6.18 (RTX → câble → UM790Pro, écran virtuel à 120 Hz), mais
le flux à 120 images/s, et 60 clics par passe au lieu de 30. PyroWave par la
route audio, HEVC par la piste vidéo RTP.

| Médianes, ms         | HEVC 120 | PyroWave 120 |
| -------------------- | -------: | -----------: |
| Clic → drapeau       |     41,4 |     **32,6** |
| Clic p90             |     59,0 |         43,7 |
| Montré (âge à l'œil) |     37,2 |         24,9 |
| Hôte → dessin (e2e)  |     13,0 |         10,6 |
| Décodage GPU (pyro)  |        — |         1,77 |
| Présentation GPU     |        — |         0,33 |
| Attente du GPU       |        — |          4,6 |

- À 120 images/s, chaque image PyroWave fait la moitié des octets : la
  sérialisation tombe vers 1,4 ms.
- Le GPU de la 780M décode en 1,77 ms au lieu de 3,15 ms à 60 images/s : la
  charge continue garde ses horloges hautes.
- Le HEVC à 120 images/s se dessine tard (« drawn » 33 ms, 3 200 images
  répétées par minute) : le décodeur ou le dessin de la page ne suit pas la
  cadence sur ce client. PyroWave n'en répète que 800.
- L'écart au clic (−8,8 ms sur 60 clics) dépasse le bruit (±4 ms à 30 clics).

**Verdict.** La promesse de PyroWave tient à haute cadence : c'est là que le
HEVC ralentit et que les images PyroWave deviennent assez petites. La suite :
confirmer en répétant les passes, puis 120 images/s comme cadence d'Ultra.

### 6.20 Trois passes de plus : l'avance se réduit, mais reste (06/10/2026, 20:10)

Même banc qu'au §6.19, trois passes de plus de chaque, en ordre alterné
(PyroWave, HEVC, HEVC, PyroWave, PyroWave, HEVC), 60 clics par passe.

| Clic → drapeau, médiane, ms | Passe 0 | Passe 1 | Passe 2 | Passe 3 | Les 4 réunies |
| --------------------------- | ------: | ------: | ------: | ------: | ------------: |
| HEVC 120                    |    41,4 |    42,3 |    33,8 |    30,9 |          35,2 |
| PyroWave 120                |    32,5 |    38,3 |    29,8 |    31,2 |      **32,8** |

| Les 4 passes réunies, ms    | HEVC 120 (227 clics) | PyroWave 120 (229 clics) |
| --------------------------- | -------------------: | -----------------------: |
| Clic, médiane               |                 35,2 |                 **32,8** |
| Clic, moyenne               |                 38,6 |                     34,4 |
| Clic p90                    |                 49,0 |                     43,6 |
| Hôte → dessin (e2e), passes |            12,7-13,2 |                10,2-11,2 |
| Images répétées / min       |          3 230-3 410 |                  620-1 030 |

- L'écart de −8,8 ms du §6.19 était en partie un tirage : le HEVC varie d'une
  passe à l'autre (deux passes vers 42 ms, deux vers 31-34 ms). Réunies,
  l'écart est de −2,4 ms en médiane, −4,2 ms en moyenne, −5,4 ms au p90.
- Ce qui ne bouge pas d'une passe à l'autre : PyroWave dessine ~2 ms plus tôt
  (e2e), répète quatre fois moins d'images, et sa queue est plus courte. Le
  décodage GPU reste à 1,77 ms à chaque passe.
- « Montré » varie trop entre passes (HEVC de 18,6 à 41,4 ms) pour servir de
  juge ; le clic reste la mesure.

**Verdict.** À 120 images/s, PyroWave est devant le HEVC sur ce client, de
peu en médiane (~2-3 ms) et nettement sur la queue et la régularité. Ce n'est
pas un écart de 9 ms ; c'est assez pour faire de 120 images/s la cadence
d'Ultra. Le gros du reste est côté page (~20 ms de clic hors e2e, communs aux
deux codecs).

**Freebox Player POP (sondée le 06/10 au soir).** TV Bro (WebView Chrome 153,
Android 10, Mali-G31) expose `navigator.gpu` mais `requestAdapter()` rend
`null` : pas de WebGPU (Android le réserve à 12+). WebGL2 y est, avec
`EXT_color_buffer_float`. PyroWave sur une TV passe donc par le repli WebGL2.

### 6.21 120 images/s par défaut, et le repli WebGL2 du décodeur (06/10/2026, 20:45)

**La cadence.** Il n'y a rien à changer : l'« Auto » du produit prend déjà
la fréquence de l'écran du client, plafonnée à 120 (`AUTO_FPS_MAX`). Une passe
PyroWave avec la cadence et l'écran virtuel laissés au produit (UM790Pro,
dalle à 240 Hz) donne 1920×1080 à 120 i/s, écran virtuel créé à 240 Hz
(`u14f-…-pw-ar-auto`). Un client à 60 Hz reste à 60 : « si l'écran le
permet ». Seul le banc forçait 60 (`U14_FPS`, défaut 60 dans `u14_series.py`).

**Le repli WebGL2** (`a53d7eed`, `PyroWaveDecoderGL.js`). Sans calcul ni
écriture dispersée, chaque étape devient une passe où chaque fragment calcule
une sortie : décalages des blocs 8×8 et des signes par bloc 32×32 ;
magnitudes des 128 fils d'un bloc (MRT : 8 valeurs et leur compte de
non-nuls) ; comptes par groupe de 16 fils ; signes, à la somme exclusive des
non-nuls qui précèdent (le scan que l'amont fait en mémoire partagée). Puis,
par niveau et composante, une passe en lignes qui lit les coefficients
directement dans la texture des fils, et une en colonnes. Chaque échantillon
déroule les quatre pas CDF 9/7 sur sa propre fenêtre de 9, miroir aux bords.
Le parseur de paquets passe dans `PyroWaveFrame.js`, commun aux deux.

| Banc du décodeur (`decoder_lab.py`), 5 clips 1080p, 60 images | WebGPU | WebGL2 |
| ------------------------------------------------------------- | -----: | -----: |
| Écart max avec la référence (Y, C)                            |   1, 1 |   1, 1 |
| Affichage : écart RGB max                                     |      3 |      3 |
| Décodage GPU, RTX 5060 Ti, p50                                | 0,17 ms | 0,83 ms |

SwiftShader d'abord (la règle « WARP d'abord ») : juste, 270 ms par image.

| De bout en bout, UM790Pro (780M), 120 i/s, 30 clics | WebGPU (§6.20) | WebGL2, minuteries | WebGL2, messages |
| --------------------------------------------------- | -------------: | -----------------: | ---------------: |
| Images dessinées / s                                |        113-118 |                 96 |              113 |
| Remplacées avant décodage                           |         79-112 |              1 167 |              150 |
| Soumis → GPU fini, p50                              |    4,3-4,9 ms |             9,2 ms |           5,7 ms |
| Clic, médiane                                       |   29,8-38,3 ms |            42,3 ms |          41,9 ms |

- La première version attendait la barrière GPU par `setTimeout(0)` en
  chaîne, bridé à 4 ms par le navigateur : le décodage suivant attendait, un
  dixième des images était remplacé. Par `MessageChannel`, le repli tient les
  120 i/s.
- Le clic à ~42 ms sur une passe de 30 clics est au-dessus des passes WebGPU,
  mais une passe seule ne départage rien (§6.20). Le repli ne sert de toute
  façon que là où WebGPU manque.
- Clé de banc : `mw_ultra_api=webgl2` force le repli. Elle reste dans le
  profil du Chrome de banc ; une passe WebGPU qui suit doit poser
  `mw_ultra_api=webgpu`.

**Freebox.** Pas encore mesurée : la box a quitté le réseau dans la soirée
(plus d'adresse MAC, le paquet magique ne la réveille pas), sans doute mise en
veille profonde par la TV (HDMI-CEC). Prochaine étape dès qu'elle est
rallumée : le repli sur la Mali-G31.

### 6.22 Le repli WebGL2 sur la Freebox : faux, et 400 ms par image (06/10/2026, 22:15)

Le labo du décodeur (`decoder_lab.py --api webgl2 --remote-cdp`) a tourné dans
TV Bro, sur la Mali-G31 de la Freebox Player POP (WebGL2, `EXT_color_buffer_float`,
pas de minuteur GPU). La box est sur son port Ethernet.

- **Compilation.** Le compilateur GLSL du Mali refusait la passe des signes
  (« no default precision defined for variable 'float[8]' » : un constructeur
  de tableau, malgré `precision highp float`). Elle remplit désormais son
  tableau élément par élément (`0753ba4a`). Le résultat est inchangé sur la
  RTX et en SwiftShader (écart 1 avec la référence, 3 au présent).
- **Exactitude.** Sur la Mali, l'image est fausse sur tous les clips essayés,
  même le dégradé : écart jusqu'à 255, PSNR ~10 dB contre la référence. La
  cause n'est pas cherchée (les limites lues sont suffisantes : 4 cibles,
  textures de 4096, flottants et entiers 32 bits).
- **Vitesse.** Décodage + présentation + lecture d'un pixel : **~405 ms par
  image 1080p** (p50 sur trois clips), soit 2,5 i/s. C'est cinquante fois trop
  lent pour 120 i/s, et vingt-cinq fois pour 60.

Verdict : **PyroWave ne vaut pas pour la Freebox**, même avec un repli juste.
Son GPU n'a ni WebGPU ni la puissance de calcul ; HEVC, décodé par le circuit
de la box, reste sa voie. Le repli WebGL2 garde son intérêt pour un client
au GPU de PC sans WebGPU (il tient 120 i/s sur le 780M, §6.21). Le mode Ultra
étant à activer à la main, l'image fausse sur Mali ne touche aucun
utilisateur.

**La Mi TV (22:45), même GPU.** Le même labo dans son TV Bro (MT5867, une
autre Mali-G31, pas d'adaptateur WebGPU non plus) donne la même image fausse
(PSNR 9-10 dB sur le dégradé, le jeu et le texte) et **~627 ms par image
1080p** (p50), soit 1,6 i/s. Le verdict vaut donc pour les deux TV du banc :
PyroWave reste un codec de PC, et les TV gardent le HEVC.

**L'iPhone 13 Pro (23:15), lui, décode juste et vite.** Safari d'iOS 26.5 a
un adaptateur WebGPU (« apple », minuteur GPU compris), et le décodeur
principal y tourne sans repli : écart 1 avec la référence (PSNR ≥ 65 dB),
**4,8 ms de GPU par image 1080p** (p50, 7,7 au p99 ; clip `game10-1080p`,
170 Mbit/s, GPU froid). C'est l'ordre du 780M au même régime (3,15 ms à
60 i/s, §6.20), sous les 8,3 ms d'une image à 120 i/s. La page était servie
par `tailscale serve` (HTTPS du réseau privé ; elle affiche désormais aussi
ses erreurs, faute de DevTools sur un téléphone). Le décodage n'est donc pas
l'obstacle sur un iPhone ; le débit l'est, PyroWave restant réservé à
l'Ethernet. Le repli WebGL2 y est juste lui aussi (écart 1), en **19 ms
d'horloge par image** (p50, 23 au p99 : décodage + présentation + lecture
d'un pixel, sans minuteur GPU) : assez pour 30 à 50 i/s, pas pour 60 ; sur
iPhone, la voie est WebGPU.

### 6.23 Décoder par tranches, pendant que l'image arrive (06/10/2026, 23:20)

Première piste du §6.18. L'hôte envoie déjà les blocs dans l'ordre de leur
index, du niveau le plus grossier au plus fin : la page n'a donc pas besoin de
l'image entière pour commencer (`2e3c70a3`).

- **Le worker de la route audio** passe à la page le début d'une image encore
  en route, par morceaux contigus d'au moins 48 Kio (clés de banc
  `mw_ultra_slices=1`, `mw_ultra_slice_kb`). Le dernier morceau ne part jamais
  seul : l'image entière suit comme avant.
- **`UltraPlayer`** soumet chaque morceau au GPU dès qu'il arrive. Il
  déquantifie les blocs déjà là, puis inverse l'ondelette de chaque niveau
  devenu complet. À l'arrivée de l'image, il ne reste que le dernier morceau,
  le niveau le plus fin et l'affichage. S'il manque un morceau, ou si une
  image par tranches attend encore le GPU, l'image suivante se décode
  entière, comme avant.
- **Le labo** (`decoder_lab.py --slices N`) coupe chaque image en N morceaux
  n'importe où, au milieu des blocs. Il vérifie le décodage et chronomètre ce
  que laisse le dernier morceau.

| GPU, 1080p, `game10` / `text10` | Image entière | 4 morceaux | 8 | 16 |
|---|---|---|---|---|
| Écart à la référence | 1 | 1 | 1 | 1 |
| Reste à la dernière tranche, p50 | 1,98 / 1,97 ms | 1,46 / 0,82 ms | 0,82 / 0,78 ms | 0,78 / 0,77 ms |

- Le résultat est exact, sur SwiftShader comme sur la 780M.
- À 8 morceaux, il ne reste que **0,8 ms au lieu de 2,0 ms**. Le plancher est
  l'ondelette inverse du niveau le plus fin, qui attend sa dernière bande.
- Pour descendre sous ce plancher, l'hôte devrait entrelacer les rangées de
  blocs des trois bandes du niveau fin. La page pourrait alors inverser ce
  niveau par bandes horizontales.

**Sur le câble (23:32-23:49).** Même banc qu'au §6.20 : la RTX encode,
l'écran virtuel est en 2560×1440 à 120 Hz, le flux à 120 i/s, la route audio
le transporte, et l'UM790Pro est le client. Six passes alternées, par
tranches (`sl1`) contre image entière (`sl0`), 60 clics chacune
(`scratchpad/pwslice_run.sh`, `pwslice_report.py`).

| Médianes | Image entière (3 passes) | Par tranches (3 passes) |
|---|---|---|
| Images décodées par tranches | 0 % | 98 % (~3 morceaux par image) |
| GPU restant à l'arrivée de l'image | 1,77 ms | 1,44 ms |
| Envoi → fin du GPU | 4,1-4,5 ms | 3,7-4,4 ms |
| Hôte → dessin (p90) | 10,3-10,8 ms (12,7-13,0) | 9,3-10,0 ms (11,6-12,5) |
| Clic, ~173 clics réunis (p90) | 31,9 ms (41,1) | 34,3 ms (41,8) |
| Images répétées par minute | 504-586 | 474-678 |

- **Le gain est réel mais petit** : −0,7 ms de l'hôte au dessin, à chaque
  passe. À 120 i/s, une image ne fait que ~180 Ko, soit trois morceaux de
  48 Kio. Et le dernier, avec le niveau le plus fin, reste à faire à
  l'arrivée.
- **Le clic ne le voit pas.** Il est même 2,4 ms plus haut en médiane, mais
  son p90 est égal. C'est dans le bruit relevé au §6.20 (31 à 42 ms d'une
  passe à l'autre) : rien n'explique un recul de 2 ms quand l'hôte → dessin
  gagne 0,7 ms.
- **La clé reste éteinte par défaut.** Une seconde moitié rendrait le gain
  visible : que l'hôte entrelace les rangées du niveau fin, et que la page
  inverse ce niveau par bandes horizontales.

### 6.24 Le niveau fin entrelacé par rangées, inversé par bandes (07/10/2026, 06:40)

La seconde moitié du §6.23 (`5e9ef99c`) :

- **L'hôte envoie les blocs dans l'ordre des index, sauf le niveau fin.**
  Ses trois bandes (luma seule, la moitié des octets) partent entrelacées
  par rangées de blocs : rangée 0 de HL, de LH, de HH, puis rangée 1, etc.
  Chaque bloc porte son index, et un décodeur ne dépend pas de l'ordre.
- **Aucun bit libre ne signale cet ordre** dans l'en-tête de PyroWave. La
  page le suppose donc. Un bloc qui arrive derrière la frontière (un hôte
  qui envoie un autre ordre) annule les tranches de l'image, qui est alors
  décodée entière. Le résultat reste juste, il n'y a que le gain de perdu.
- **La page déquantifie dans l'ordre d'envoi**, par une table position →
  index. Elle inverse le niveau fin par bandes de tuiles de 32 lignes, dès
  que les rangées de blocs qu'elles lisent sont réglées (16 lignes de bande
  plus 2 de marge de chaque côté).

Le labo (`decoder_lab.py --slices N`, le corpus de référence remis dans
l'ordre d'envoi) est exact, comme l'image entière : écart max 1 sur
SwiftShader et sur le 780M. GPU restant au dernier morceau, 780M, 1080p :

| Morceaux | `game10` §6.23 → §6.24 | `text10` §6.23 → §6.24 |
|---|---|---|
| Image entière | 1,98 ms | 1,97 ms |
| 4 | 1,46 → 1,46 ms | 0,82 → 0,58 ms |
| 8 | 0,82 → 0,48 ms | 0,78 → 0,33 ms |
| 16 | 0,78 → 0,34 ms | 0,77 → 0,18 ms |

- **Le plancher de ~0,8 ms est tombé.** Le dernier morceau ne porte plus
  que la fin de l'inverse du niveau fin, pas l'inverse entier.
- **À 4 morceaux, `game10` ne gagne rien** : son dernier quart commence
  avant le niveau fin, et tout le niveau fin reste à faire à l'arrivée.
- Sur le câble à 120 i/s, une image fait ~3 morceaux de 48 Kio. Le
  morceau plus petit (`mw_ultra_slice_kb`) est donc à essayer avec cette
  passe.

### 6.25 U3.7 B1 : Chrome voit trop tard que le GPU a fini (09/10/2026, soir)

> ⚠️ Les verdicts des §6.19-6.20 ne tiennent plus. Leur HEVC passait par la
> piste vidéo RTP (métronome de Chrome, ~8 ms), et la sonde du clic se
> mesurait elle-même jusqu'à `652fc726` (`click-waits.md` §6). La remesure à
> 120 i/s contre le HEVC du produit (SCTP) attend l'UM790Pro sous Windows.

Le point de départ est B0 (`click-waits.md` §3.4), sur le 780M, en plein flux :
5,1 ms de la soumission à la fin pour 2,2 ms de travail GPU, et 3,6 ms pour une
soumission vide. B1 reprend la question hors flux, avec
`scripts/bench/ultra/gpuwait-lab.html` et `gpuwait_lab.py` :

- les mêmes soumissions et la même trace que `UltraPlayer`, que
  `gpuwait.py --dir bench-out/ultra-lab` lit telle quelle ;
- un Chrome 155 headless à part, un GPU de DualRTX à la fois
  (`--use-adapter-luid`) ;
- la page isolée (COOP/COEP), donc une horloge à la µs et non à 0,1 ms ;
- 1 500 soumissions par cas, deux tours ABBA. Médianes en ms.

| Cas | RTX 5060 Ti | Arc A380 | iGPU AMD (2 CU) |
|---|---|---|---|
| commande vide, sans passe | 0,10 | 0,11 | 0,10 |
| passe vide horodatée | 2,9-3,2 | 3,0-3,2 | 3,0-3,1 |
| dont après la fin du GPU | 2,6-2,8 | 2,7-2,8 | 1,8-2,3 |
| image 1080p décodée, 120 i/s | 3,7 | 4,2 | 8,0-8,3 |
| dont travail du GPU | 0,19 | — * | 5,4 |
| dont après la fin du GPU | 2,8 | — * | 1,3-1,5 |

\* En décodage, l'Arc rend des horodatages faux.

- **Ce n'est pas le 780M.** Les trois GPU, de trois marques, paient les mêmes
  ~3 ms pour une passe vide, que le GPU exécute en quelques µs. La montée en
  fréquence du GPU (hypothèse 2 de B1) n'en est donc pas la cause.
- **C'est le processus GPU de Chrome, qui ne relève ses barrières que de
  temps en temps (hypothèse 1).**
  - Une commande vide, qui n'attend rien, revient en 0,1 ms : l'aller-retour
    lui-même ne coûte rien.
  - Le temps passe entre la fin du travail et le moment où Chrome s'en
    aperçoit : 2 à 3 ms. Les fins ne tombent sur aucune grille fixe : le
    délai court depuis la soumission.
  - Lecture probable : un relevé différé d'~2 ms après la dernière commande
    reçue, puis toutes les ~2 ms. Ce n'est pas vérifié dans les sources de
    Chromium.
- Rien d'autre ne change ce délai : attendre par `mapAsync` au lieu
  d'`onSubmittedWorkDone`, décoder dans un worker plutôt que sur le fil
  principal, soumettre à 120 i/s ou d'affilée.
- Sur l'iGPU AMD, le décodage (5,4 ms) plus l'attente dépassent l'intervalle
  de 8,33 ms : 109 à 117 images par seconde au lieu de 120.

**Le levier : une commande vide pendant l'attente.** `queue.submit([])` toutes
les 0,25 ms (ou toutes les 1 ms), par une boucle de messages, tant que l'image
attend :

| Décodage à 120 i/s | Soumission → fin | Après la fin du GPU | Début du GPU | Fil principal par image | Images/s |
|---|---|---|---|---|---|
| RTX, sans | 3,7 | 2,8 | 0,5-0,6 | 0,6-0,8 | 120 |
| RTX, toutes les 0,25 ms | 1,0-1,2 | 0,34-0,37 | 0,5-0,6 | 1,9-2,3 | 120 |
| RTX, toutes les 1 ms | 1,3 | 0,63 | 0,6-0,7 | 2,5 | 120 |
| iGPU AMD, sans | 8,1-8,2 | 1,3-1,4 | 1,3 | 0,6-0,8 | 109-115 |
| iGPU AMD, toutes les 0,25 ms | 6,6-6,7 | 0,36-0,43 | 0,75-0,8 | 7,2 | 120 |
| iGPU AMD, toutes les 1 ms | 7,2 | 1,0 | 0,7-0,8 | 7,5 | 120 |

- **N'importe quelle commande réveille Chrome.** Une soumission vide,
  4 octets par `writeBuffer` ou un `onSubmittedWorkDone` de plus ramènent tous
  le délai à ~0,35 ms. Sur l'iGPU AMD, le GPU commence aussi plus tôt (1,3 →
  0,75 ms), et le décodage retrouve ses 120 images par seconde.
- **Le prix : le fil principal tourne pendant toute l'attente.** La boucle de
  messages coûte 1,1 à 1,6 ms par image sur la RTX, et ~6,5 sur l'iGPU AMD.
  Des minuteries ne marchent pas à la place : dans ce Chrome, un
  `setTimeout(1)` part 1,5 à 3 ms plus tard, et le délai ne bouge pas.
- **Ce qu'on peut en attendre sur le 780M**, d'après les chiffres de B0 : la
  fin vue 1,2 à 1,8 ms plus tôt, et le GPU qui commence ~0,5 ms plus tôt. Soit
  1,5 à 2 ms par image, sur le clic comme sur l'hôte → dessin, à condition que
  l'affichage ne repaie pas ce délai plus loin.
- Le HEVC ne passe pas par ce chemin : WebCodecs remet ses images autrement.
  C'est un handicap propre à PyroWave dans la page.

**À vérifier avant d'en tirer un gain :**

- **Le 780M dans le même labo**, et une fenêtre affichée (hypothèse 3 : la
  file partagée avec le compositeur).
- **Ce qui arrive à l'écran.** La sonde corrigée date le dessin, pas
  l'affichage. Une image remise plus tôt à la page (`mw_ultra_early=1`, ou B2.1
  par le canevas WebGPU) avance cette heure sans prouver que l'écran avance.
  Un tel levier se juge donc en bout de chaîne, sur l'écran du client
  (`scripts/bench/photon/`).
- **La relance dans `UltraPlayer`**, derrière une clé de banc, en ne tournant
  qu'autour de la fin attendue (les horodatages GPU la donnent) pour épargner
  le fil principal. Puis des passes ABBA sur le câble, jugées au clic et à
  l'hôte → dessin.

## 7. Concrètement, pour l'utilisateur

Pendant le POC, rien ne change : Ultra est caché derrière deux clés de banc et
n'est jamais choisi seul. S'il gagne, il deviendra plus tard, dans un autre plan,
un mode « Ultra (LAN) » pour un appareil relié en Ethernet. L'image arriverait
plus tôt, surtout quand l'hôte a un GPU Intel ou AMD intégré, et sans image clé :
une perte ne ferait plus sauter l'image, elle flouterait quelques blocs pendant
une image. En échange, le débit passerait de 20-40 à 150-200 Mbit/s. Sur un PC
NVIDIA, il faut s'attendre à une égalité, voire une légère perte. S'il perd, on
saura pourquoi et de combien, et les gains trouvés en chemin hors codec (cadence
à 120 ou 240 Hz, présentateur, réglages du transport) iront à tout le monde. Les
TV passent au banc elles aussi : si le décodeur Ultra tourne assez vite sur leur
petit GPU, elles en profiteront ; sinon elles gardent leur décodeur matériel.

Déjà visible (U0.2) : le détail de la latence, dans les statistiques du stream,
gagne une ligne « Mesurée (hôte → dessin) ». C'est l'âge réel de l'image à
l'écran, pris d'un bout à l'autre. Si elle lit nettement plus que le total
au-dessus d'elle, une partie du trajet n'est chronométrée par personne. En Wi-Fi,
elle peut lire un peu faux, de la moitié de l'écart entre la montée et la
descente.

Déjà visible (U0.3, N95) : sur un portable modeste en Wi-Fi, forcer 120 i/s
dégrade tout. L'image a 130 à 320 ms de retard au lieu de 45 à 58, avec des
coupures de plusieurs secondes, et plus d'un clic sur deux reste sans réponse visible.
L'« Auto » d'aujourd'hui fait le bon choix sur cet appareil : il essaie de
monter, voit que ça ne tient pas, et reste à la cadence de l'écran. En Ethernet, sur un mini-PC (U0.3, UM790Pro), le même « Auto » monte à
240 i/s : l'image a 24 à 27 ms de retard et le clic s'affiche en 35 à 43 ms,
aussi bien que le réglage Ultra forcé.

Déjà visible (U0.4, la borne Steam) : sur un PC à carte NVIDIA relié en
Ethernet, le codec PyroWave de Steam affiche un clic 7 à 15 ms plus tôt que
son HEVC. C'est ce qui justifie de poursuivre le POC. Une partie de ce gain
vient sans doute de la façon dont Steam présente l'image, et pas seulement du
codec : dans le navigateur, le gain à attendre est plus petit, et le chemin
d'affichage de Chrome (15 à 25 ms) devient le prochain poste à travailler.

Pas encore visible (U1.4, la vidéo sur une piste RTP) : un interrupteur caché
peut faire passer la vidéo par RTP au lieu du DataChannel, codec par codec,
séparément pour l'hôte natif et pour Sunshine ou Apollo. Il est coupé par défaut
et rien ne change pour l'utilisateur. Aujourd'hui, pour un stream seul, RTP fait
attendre l'image 5 à 10 ms de plus, parce que Chrome la retient avant de nous la
rendre. En revanche, quand un flux très lourd comme Ultra passe à côté de la
vidéo, RTP évite que la vidéo attende derrière lui : elle reste aussi fraîche que
sans charge. Si la retenue de Chrome se lève, RTP pourra servir à tout le monde.
Sinon, il restera le transport d'Ultra seulement.

Pas encore visible (U1.4 ter, la route audio) : la retenue de Chrome vient d'une
horloge interne que la page ne peut pas couper. Mais elle ne touche que les
pistes vidéo. En faisant voyager les images de la vidéo par une piste audio,
qu'on ne joue jamais, le RTP rattrape le DataChannel pour un stream seul. Quand
un flux lourd passe à côté, la vidéo reste fraîche, à 11 ms de l'hôte à l'écran
au lieu de 21 ms sur une piste vidéo et de 125 ms sur le DataChannel. Elle sait
maintenant redemander un morceau perdu. Même quand 5 % des paquets se perdent,
chaque image arrive, avec 2 à 3 ms de retard en plus. Le DataChannel, lui, en
perd une sur trois. Rien de tout cela n'est encore activé pour les joueurs :
la mesure sur câble vient d'abord, puis Bruno choisit, codec par codec.

Mesuré sur câble (06/10) : sur un vrai câble Ethernet, le DataChannel d'aujourd'hui
suffit déjà. L'image a le même âge par la route audio et par le DataChannel,
même quand un flux Ultra de 120 Mbit/s passe à côté. La route audio ne se
distingue que quand des paquets se perdent : elle garde toutes les images, et
le DataChannel en perd une sur deux. Sur ce câble, l'« Auto » fait mieux que le
120 i/s forcé, de 4 à 5 ms.
