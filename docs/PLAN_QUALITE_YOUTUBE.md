# Plan — Option « Qualité vidéo YouTube » (1080p par défaut)

État : LIVRÉ dans la 2.71 (2026-09-28). Correction de la relecture du diff intégrée :
chaque chargement YouTube emporte sa qualité en option PAR FICHIER (échappée %N%),
car mpv restaure les options sauvegardées d'un fichier à sa fin, ce qui écrasait
une qualité changée pendant un flux audio. Six essais passés au bureau (défaut,
480p, meilleure possible, mode audio, changement à chaud, changement pendant un
flux audio). Historique : PLAN VALIDÉ par relecture (2026-09-28). Décision de Lee :
option réglable, 1080p par défaut.

Relecture : sélecteur prouvé avec le yt-dlp de l'utilisateur (1080 → 248+251 VP9,
720 → 247+251 VP9, 480 → 244+251 VP9, jamais d'AV1) ; `ytdl-format` relu à chaque
chargement, chaîne vide = défaut ; repli automatique `bestaudio/best` du crochet
confirmé dans la DLL livrée (mpv 0.41). Corrections intégrées ci-dessous : point 4
(bloquant), trace du point 6, endroits oubliés du point 5. Nuance : une vidéo
verticale plafonnée à 1080 de haut joue en 608×1080.

## Problème prouvé

Sur le portable ASUS de Lee (Intel UHD 620 + NVIDIA MX150, i5-8265U), une vidéo
YouTube lue en flux a le son mais pas l'image. Les vidéos locales et les vidéos
YouTube téléchargées s'affichent.

Cause : sans consigne de format, le crochet ytdl de mpv prend le meilleur format
proposé, soit pour une vidéo courante `401+251`, c'est-à-dire de l'**AV1 en 3840×2160**.
Aucune des deux puces graphiques du portable ne décode l'AV1 en matériel. Le
journal du portable, le 2026-06-13, le montre pour les trois voies d3d11, dxva2
et cuda : « Your platform doesn't support hardware accelerated AV1 decoding. »
Le processeur seul ne tient pas le rythme d'une vidéo AV1 en 4K : le son passe,
l'image décroche. Sur l'ordinateur de bureau (décodage AV1 matériel d3d11va),
tout fonctionne ; un agent l'a mesuré : première image en moins de 3 s, aucune
image perdue.

Pistes déjà écartées par mesure : fenêtre YouTube par-dessus (réelle mais pas la
cause du cas de Lee), `demuxer-lavf-analyzeduration` et `demuxer-lavf-probesize`
(A/B : aucun effet), propriété `vid` (corrigée en 2.68).

## Ce que fait le correctif

Nouvelle option **Options → YouTube → « Qualité vidéo YouTube »**, une liste
déroulante :

- **1080p maximum** (par défaut, recommandé)
- **720p maximum**
- **480p maximum**
- **La meilleure possible** (comportement actuel : 4K, AV1 compris)

Les trois choix plafonnés **excluent aussi l'AV1**, parce que YouTube propose
aussi l'AV1 en 1080p (format 399) : plafonner la hauteur sans exclure l'AV1
laisserait le portable en décodage logiciel. Ils se rabattent sur H.264 ou VP9,
que les deux puces du portable décodent en matériel.

## Travaux

### 1. Réglage

- Nouvelle variable `g_ytVideoQuality` (entier : 0 = meilleure possible, sinon
  la hauteur maximale 1080 / 720 / 480). Défaut **1080**.
- INI `[YouTube] VideoQuality`, lu dans `LoadSettings` près de `VideoMode`
  (`src/settings.cpp` ~ligne 419), écrit dans `SaveSettings` (~ligne 886).
  Valeur inconnue → 1080.

### 2. Traduction en sélecteur de format

Fonction unique, par exemple `std::string YtFormatForQuality(int q)` :

- `q == 0` → chaîne vide (ne pas poser `ytdl-format` : mpv garde son comportement
  par défaut).
- sinon, pour `H` = 1080 / 720 / 480 :
  `bestvideo[height<=?H][vcodec!^=av01]+bestaudio/best[height<=?H][vcodec!^=av01]/best[height<=?H]/best`
  Les replis successifs garantissent qu'une vidéo sans format conforme joue
  quand même plutôt que d'échouer.

### 3. Application à mpv

- `ytdl-format` est une option mpv ordinaire, relue par le crochet ytdl à chaque
  chargement : elle peut être posée à tout moment avec
  `mpv_set_property_string(g_mpv, "ytdl-format", ...)`.
- Fonction `MPVApplyYouTubeQuality()` dans `src/video_engine.cpp` : si `g_mpv`
  existe, pose la chaîne (ou la remet à vide pour « meilleure possible »).
- Appelée : dans `InitMPV` après la pose des options (~ligne 481, près de
  `ytdl=yes`), et à la validation des Options quand le réglage change.

### 4. Mode audio (chemin hybride) — à ne pas casser

Point à vérifier en relecture : quand `ytdl-format` n'est pas posé et que la
vidéo est désactivée (`vid=no`), le crochet ytdl de mpv choisit de lui-même
`bestaudio/best`. Or depuis la 2.68, le chemin hybride de `YouTubePlayById`
(`src/youtube.cpp`) ne coupe `vid` qu'APRÈS le chargement : le crochet voit donc
la vidéo active et télécharge le flux vidéo pour rien. Avec un `ytdl-format`
explicite, ce serait systématique.

~~Correctif initial : poser `bestaudio/best` avant le `LoadURL`, restaurer après.~~
**Rejeté à la relecture** : `loadfile` est asynchrone, le crochet ytdl lit l'option
plus tard (crochet `on_load`, fil de mpv) ; la restauration arriverait presque
toujours AVANT sa lecture. Même aléa pour `vid=no` posé après le chargement.

**Correctif retenu : options propres au fichier dans la commande `loadfile`.**
mpv 0.41 (celui livré) : `{"loadfile", url, "replace", "-1",
"ytdl-format=bestaudio/best,vid=no", NULL}`. mpv applique ces options avant les
crochets `on_load` et les annule d'elles-mêmes à la fin du fichier : aucun aléa,
rien à restaurer. Il faut une variante de `MPVLoadURL` qui accepte ces options,
transmise de `LoadVideoURL` jusqu'au chemin hybride. `MPVSetAudioOnly(true)`
après le chargement devient superflu (sans danger à laisser).

### 5. Options → onglet YouTube

- `MediaAccess.rc` (sans accents), sous « Autoplay next result » (y=265) :
  `LTEXT "YouTube &video quality:"` à y≈283, puis
  `COMBOBOX IDC_YT_VIDEO_QUALITY ... CBS_DROPDOWNLIST` sur la même ligne. La place
  est libre jusqu'aux boutons OK/Annuler (y=332). Vérifier que la lettre
  mnémonique `v` n'est pas déjà prise sur l'onglet.
- Nouveaux identifiants dans `resource.h` : **1776 et 1777** (1775 est déjà pris
  par `IDC_PLAYLIST_RENAME_EDIT`).
- `COMBOBOX` avec le style complet `CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP` et
  une hauteur de liste d'au moins 60, comme `IDC_YT_CAPTION_LANG`. Le `LTEXT` juste
  avant le `COMBOBOX`, et les deux avant le commentaire « SoundTouch », pour que le
  libellé nomme la liste et que la tabulation suive « Autoplay next result ». La
  lettre `v` est libre sur l'onglet (déjà prises : A, D, w, n).
- Les quatre choix ajoutés par `CB_ADDSTRING` avec `T(...)`, comme `kCaptionLangs`.
- Déclarer `g_ytVideoQuality` dans `src/globals.cpp` près de `g_ytAutoplayNext`, et
  son `extern` dans `globals.h` ; prototypes de `MPVApplyYouTubeQuality` et
  `YtFormatForQuality` dans l'en-tête du moteur vidéo.
- Ajouter les deux contrôles au tableau des contrôles de l'onglet YouTube
  (`src/ui_options.cpp` ~ligne 284) pour qu'ils s'affichent sur le bon onglet.
- Remplir la liste et sélectionner la valeur courante à l'ouverture (~ligne 806) ;
  relire à la validation (~ligne 1342) puis appeler `MPVApplyYouTubeQuality()`.
- Traductions FR/EN des cinq libellés dans `src/translations_rc.cpp` (accents
  permis dans ce fichier).

### 6. Trace de diagnostic (permanente)

Dans le fil d'événements de mpv (`src/video_engine.cpp`) :
- à `MPV_EVENT_FILE_LOADED` (~ligne 416) : codec de la piste vidéo
  (`current-tracks/video/codec`) et sa taille (`current-tracks/video/demux-w` et
  `demux-h` — `width`/`height` ne sont pas encore connues à ce stade), plus le
  **nombre de pistes vidéo dans `track-list`** (qui compte aussi les pistes
  désélectionnées : c'est lui qui prouve qu'aucune vidéo n'est reçue en mode audio,
  `current-tracks/video` étant absent dès que `vid=no`) ;
- à `MPV_EVENT_VIDEO_RECONFIG` : `hwdec-current` et `video-params/w`/`h` (peut
  arriver plusieurs fois par fichier).
Chaque `mpv_get_property_string` peut rendre NULL : tester avant `LogF`, puis
libérer avec `fn_mpv_free`.

**Correction après mesure (implémentation)** : compter les pistes vidéo de
`track-list` ne prouve rien. Le crochet ytdl de mpv 0.41 est en mode
`all_formats (separate)` : il RÉPERTORIE chaque format comme une piste, mais ne
l'ouvre que si elle est sélectionnée. Mesuré hors application : en mode audio,
yt-dlp est bien appelé avec `--format bestaudio/best` (l'option par fichier
atteint donc le crochet), une piste vidéo est quand même listée, mais aucune
n'est sélectionnée. La trace journalise donc le format demandé
(`options/ytdl-format`, valeur par fichier comprise) et la piste vidéo
sélectionnée.
Une ligne `LogF("VIDEO", ...)` chacune. Un prochain « pas d'image » dira de
lui-même le format reçu et si le décodage matériel a pris.

## Hors du périmètre

- Les téléchargements YouTube (ils fonctionnent, leur format est choisi ailleurs).
- Les sous-titres allemands affichés d'office sur certains flux (`sid=auto`) :
  repéré, à traiter séparément.
- La fenêtre YouTube qui peut recouvrir l'image : réelle, traitée séparément si
  Lee le souhaite.

## Tests

À l'oreille pour Lee, et par une personne voyante pour l'image :
1. Portable, réglage par défaut (1080p) : une vidéo YouTube en flux a une image.
2. Portable, « La meilleure possible » : le défaut revient (preuve que c'est
   bien le format).
3. Bureau, 1080p : image, et le journal indique un codec H.264 ou VP9, jamais AV1.
4. Mode audio (vidéo YouTube décochée) : le son joue, et le journal ne montre
   aucune piste vidéo reçue.
5. Changer le réglage dans les Options sans redémarrer : la vidéo suivante suit
   le nouveau réglage.
