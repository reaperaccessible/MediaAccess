# Plan — yt-dlp nightly, toujours à jour, sans jamais installer un fichier douteux

État : LIVRÉ dans la 2.72 (2026-09-28). Relecture du diff intégrée : une sortie
demandée pendant une attente termine l'attente, abandonne l'action puis ferme
(jamais de destruction sous la boucle de l'attente) ; PlayTrack et les lectures
YouTube arrivant pendant une attente sont ignorés (plus de morceaux sautés) ; une
copie de version inconnue est identifiée par son --version avant tout
téléchargement (installation neuve) ; WM_DESTROY attend le fil avant FreeLogger.
Plan relu le même jour, corrections intégrées. Décisions de Lee le 2026-09-28.

Essais réels passés au bureau : mise à jour vers la nightly 2026.09.27.232945
(taille, SHA-256, --version, remplacement) ; fermeture par /quit pendant le
téléchargement (processus fermé après l'installation) ; vidéo YouTube passée au
lancement (attente silencieuse, puis fenêtre d'attente, vidéo chargée après
l'installation) ; réparation au démarrage (.old remis, .new non vérifié
supprimé) ; fermeture pendant la seule vérification (sortie en 0,1 s) ; script
`scripts\update_bundled_ytdlp.ps1` sous PowerShell 5.1, appelé par
`build_new.bat` (non bloquant).

Écart d'implémentation : la vérification démarre au tout début de `WM_CREATE`
(le premier essai a montré qu'une vidéo ouverte en ligne de commande partait
sinon avec l'ancien yt-dlp). Une fermeture pendant la seule vérification
(`Checking`) ne diffère pas la sortie : la vérification est abandonnée.

## Pourquoi

- La mise à jour actuelle (`src/ytdlp_updater.cpp`) ne vérifie **qu'au démarrage**,
  prend la version **stable**, et est **muette** : aucune trace dans le journal.
- **Défaut confirmé** (lignes 134-137) : si `WinHttpQueryDataAvailable` ou
  `WinHttpReadData` échoue, la boucle sort avec `failed=false` ; le fichier tronqué
  est mis en place et la version enregistrée (ligne 220). YouTube reste cassé
  jusqu'à la publication suivante.
- Le yt-dlp livré dans l'installeur date de mars (2026.03.17).
- « Cette vidéo est privée » est aussi donné pour le contrôle anti-robots
  (`src/youtube.cpp` ~856).

## Décisions (Lee)

1. **Source : nightly**, toujours à jour.
2. **Vérification à chaque lancement**, et au fil de la journée si MediaAccess reste
   ouvert.
3. **Aucune action YouTube ne part avec une vieille version** tant qu'une mise à
   jour est en cours : fenêtre d'attente **impossible à fermer**, qui se ferme
   seule à la fin.
4. **La mise à jour va au bout** même si l'utilisateur ferme MediaAccess : la
   fenêtre disparaît, la mise à jour se termine en silence, puis MediaAccess se
   ferme. Limite honnête : au-delà de 2 minutes (connexion très lente), la
   mise à jour est abandonnée proprement, l'ancien yt-dlp reste, et elle reprend
   au lancement suivant.
5. Le reste de MediaAccess (fichiers, radio, podcasts) n'est jamais bloqué, sauf
   pendant l'affichage de la fenêtre d'attente, qui bloque sa fenêtre
   propriétaire (les touches globales continuent de marcher).

## Faits vérifiés (relecture)

- Dépôt `yt-dlp/yt-dlp-nightly-builds`, dernière étiquette `2026.09.27.232945`,
  `yt-dlp.exe` 17 842 808 octets. `SHA2-256SUMS` : `<hex minuscule><2 espaces><nom>`.
  L'adresse de téléchargement renvoie un 302, déjà suivi par le code.
- `yt-dlp --version` d'une nightly affiche l'étiquette nue (retirer CR/LF).
- Lancements de yt-dlp : `RunYtdlp` (`youtube.cpp` ~694), le crochet ytdl de mpv,
  `ui.cpp:974`. Rien d'autre.
- Presque tous les appels à `RunYtdlp` partent de **fils secondaires**
  (`SearchThreadProc`, `LoadMoreThreadProc`, `DownloadThreadProc`,
  `BatchDownloadThreadProc`, `FormatsThreadProc`, `DescThreadProc`,
  `SubsCheckThreadProc`, `CaptionFetchThread`, `RefreshCacheThread`,
  `HybridDownloadThread`), qui lisent `g_ytdlpPath`.
- Le crochet ytdl de mpv 0.41 **relit `script-opts` à chaud** (preuve dans le
  script intégré à `libmpv-2.dll`).
- Sorties de l'application : `IDM_FILE_EXIT` (`PostQuitMessage` seul, sans
  `DestroyWindow`, main.cpp:1566), `IDM_TRAY_EXIT` (`DestroyWindow`, ~2010),
  fermeture par défaut (aucun `case WM_CLOSE`), `/quit` (`cli_switches.cpp:261`,
  `WM_CLOSE`), mise à jour de MediaAccess (`updater.cpp:640` `WM_CLOSE` puis
  `ExitProcess`, `updater.cpp:691` `WM_CLOSE` puis script). Réglages enregistrés
  dans `WM_DESTROY` (~2283).
- `ExtractJsonField` ne lit que des valeurs texte à leur première apparition,
  sans espaces : inutilisable tel quel pour `assets[].size`.

## Travaux

### 1. État central et vérification

- État atomique unique : `Idle`, `Checking`, `Downloading`, `Verifying`,
  `Installing`. **`Checking` commence dès le lancement** (avant l'attente de 5 s),
  pour qu'aucune action des premières secondes ne parte avec l'ancien yt-dlp.
- Requête à `/repos/yt-dlp/yt-dlp-nightly-builds/releases/latest` avec l'en-tête
  `Accept: application/vnd.github+json`. Lecture : `tag_name` ; dans `assets`,
  isoler l'objet dont `name` vaut `yt-dlp.exe` et lire dans ce même objet `size`
  (nombre) et `browser_download_url` ; idem pour `SHA2-256SUMS`. Analyse tolérante
  aux espaces.
- Délais WinHTTP de 10 s par opération, **plus** une limite globale pour le
  téléchargement entier (110 s, sous le plafond de fermeture).
- Revérification : minuterie d'une heure qui compare à l'heure de la dernière
  vérification (12 h), plus une vérification au réveil (`WM_POWERBROADCAST`,
  `PBT_APMRESUMEAUTOMATIC`).
- Sans réseau : retour à `Idle`, trace au journal, rien ne bloque.

### 2. Installer seulement un fichier sûr

Au démarrage, **avant** toute vérification, réparation :
- si `yt-dlp.exe` manque et que `yt-dlp.exe.old` existe : remettre `.old` en place ;
- supprimer tout `.old` restant (normal : un .exe en cours d'exécution se renomme
  mais ne se supprime pas) ;
- un `yt-dlp.exe.new` n'est installé que si le fichier de réglages porte son
  étiquette et son SHA-256 attendus (`[YouTube] YtdlpPendingTag`,
  `YtdlpPendingSha256`) **et** que l'empreinte recalculée correspond ; sinon il
  est supprimé.

Mise à jour, chaque étape arrêtant tout en cas d'échec (l'ancien reste) :
1. Télécharger vers `yt-dlp.exe.new` ; toute erreur de lecture ou d'écriture =
   échec. Drapeau d'abandon testé dans la boucle et entre les étapes.
2. Taille écrite == `size`.
3. SHA-256 (BCrypt) == ligne `yt-dlp.exe` de `SHA2-256SUMS`.
4. `yt-dlp.exe.new --version` (caché, 30 s) == `tag_name`.
5. Enregistrer `YtdlpPendingTag` / `YtdlpPendingSha256`.
6. Remplacer (section non interruptible, le drapeau d'abandon n'y est pas testé) :
   `yt-dlp.exe` → `.old`, `.new` → `yt-dlp.exe` (`MoveFileExW`), puis tenter de
   supprimer `.old`. Si un renommage échoue (antivirus, verrou), garder le `.new`
   vérifié ; retenter à la vérification suivante ou au lancement suivant.
7. Enregistrer `YtdlpVersion`, effacer les clés `Pending`.

### 3. Chemin de yt-dlp et moteur vidéo

- Accesseur `std::wstring GetYtdlpPath()` protégé par un `SRWLOCK`, qui renvoie une
  copie ; tous les lecteurs de `g_ytdlpPath` passent par lui (youtube.cpp,
  ui.cpp, video_engine.cpp).
- **Au démarrage, copier `lib\yt-dlp.exe` vers `%LOCALAPPDATA%\MediaAccess\` s'il
  n'y est pas.** Le chemin est alors toujours le même : le remplacement sur place
  suffit, mpv utilise le nouveau fichier sans rien changer.
- Filet : si le chemin change quand même, le fil de mise à jour l'écrit sous le
  verrou **avant** de signaler la fin, puis poste un message au fil principal qui
  réécrit `script-opts` (même conversion `\` → `/` que video_engine.cpp:542-547).

### 4. Fenêtre d'attente obligatoire pour YouTube

- `bool YtdlpWaitIfUpdating(HWND owner)`, appelée **sur le fil principal, aux
  points d'entrée, avant le lancement du fil secondaire** :
  recherche, « charger plus », liste de lecture ou chaîne, formats, téléchargement
  simple et groupé, description, début de `YouTubePlayById` (vidéo, hybride,
  dernier recours), branche YouTube de `LoadURL` (`player.cpp:486-492`), test du
  menu Aide.
- Au début de `RunYtdlp` : filet **silencieux** pour les travaux de fond
  (sous-titres, abonnements, cache). Le fil principal est reconnu par un
  identifiant noté au démarrage.
- État `Checking` : attente silencieuse (au plus 10 s). Fenêtre seulement si un
  téléchargement commence (`Downloading` ou après).
- La fenêtre : « Veuillez patienter, une mise à jour obligatoire de YouTube est
  en cours », propriétaire `GetMessageBoxOwner()`. Le focus va sur un champ de
  texte en lecture seule contenant le message (lu par NVDA dès l'ouverture).
  Aucun bouton de fermeture ; `IDCANCEL`, `WM_CLOSE`, `SC_CLOSE` ignorés. Toute
  touche, Échap compris, annonce l'avancement (« Mise à jour 40 % »). Annonces
  automatiques à 25, 50, 75 %. Fermeture seule à la fin, focus rendu au contrôle
  d'origine.
- Réentrance : indicateur « attente affichée » ; toute action YouTube arrivant
  pendant l'attente (touche globale, enchaînement automatique) est ignorée, jamais
  de seconde fenêtre.
- Plafond de 90 s : fermeture, annonce « La mise à jour de YouTube n'a pas pu se
  terminer ; la version actuelle est utilisée », l'action continue. La mise à
  jour, elle, continue en fond.

### 5. Fermeture pendant une mise à jour

- Toutes les sorties passent par une fonction unique `RequestAppExit(reason)` :
  `WM_CLOSE` (Alt+F4, `/quit`), `IDM_FILE_EXIT` (qui passe à `DestroyWindow`, donc
  les réglages sont enfin enregistrés par cette voie), `IDM_TRAY_EXIT`.
- Si une mise à jour yt-dlp est en cours : arrêter la lecture, masquer la fenêtre
  et l'icône de la zone de notification, marquer « fermeture différée ». Les
  réglages ne sont enregistrés qu'une fois, dans `WM_DESTROY`. Fin de la mise à
  jour (réussie ou non) → `DestroyWindow`.
- Plafond de 2 minutes : drapeau d'abandon, attente courte (au plus 5 s) que le
  fil s'arrête, puis `DestroyWindow`. La réparation du démarrage (point 2) couvre
  tout reste.
- Relance pendant la fermeture différée : **tout** `WM_COPYDATA` (dwData 1, 2, 3,
  4), sauf `/quit`, annule la fermeture, remet l'icône et réaffiche la fenêtre.
- **Mise à jour de MediaAccess** (`updater.cpp:640` et `:691`) : contourne la
  fermeture différée ; abandon de la mise à jour yt-dlp, attente courte du fil,
  suppression du `.new`, puis sortie normale.
- `WM_QUERYENDSESSION` / `WM_ENDSESSION` : jamais bloqués ; abandon immédiat
  (hors section de remplacement, qui dure quelques millisecondes).

### 6. Journal

Lignes `[YTDLP]` : réparation au démarrage, version en place, version disponible,
début et fin du téléchargement (octets), taille, empreinte, `--version`,
remplacement, échec et sa raison, attente silencieuse ou fenêtre affichée,
plafond atteint, fermeture différée, abandon.

### 7. yt-dlp livré dans l'installeur

Script `tools\update_bundled_ytdlp.ps1` (ASCII + BOM, PowerShell 5.1), appelé avant
la construction de l'installeur : télécharge la dernière nightly vers
`lib\yt-dlp.exe` avec la même vérification d'empreinte.

### 8. Messages d'erreur YouTube

Dans le classement (`youtube.cpp` ~856), **avant** le cas « privée » :
- `not a bot` → « YouTube demande une vérification anti-robots pour cette
  connexion. Réessayez plus tard. » (FR/EN) ;
- `confirm your age` → « Cette vidéo est réservée aux adultes. » (FR/EN) ;
- le reste de `sign in to confirm` reste dans « privée ».

## Hors du périmètre

- Le réglage de qualité vidéo YouTube (2.71).
- Les cookies de navigateur.
- La mise à jour de MediaAccess lui-même (seulement sa cohabitation, point 5).

## Tests

1. Nouvelle version : téléchargement, vérifications, remplacement, journal complet.
2. Téléchargement coupé : l'ancien reste, aucune version enregistrée, nouvel essai.
3. Empreinte fausse : refusé.
4. `.new` sans trace dans les réglages au démarrage : supprimé ; `.old` seul :
   remis en place.
5. Recherche YouTube pendant la mise à jour : fenêtre d'attente, impossible à
   fermer, Échap annonce l'avancement, fermeture seule, puis la recherche part.
6. Action YouTube dans les 5 premières secondes : attend la réponse de l'API.
7. Touche globale « suivant » pendant l'attente : pas de seconde fenêtre.
8. Plafond de 90 s : message, action avec l'ancienne version.
9. Fermeture par Alt+F4, menu Fichier, zone de notification, `/quit` pendant la
   mise à jour : fenêtre masquée tout de suite, processus terminé après ;
   « Ouvrir avec » pendant ce temps : la fenêtre revient et joue le fichier.
10. Mise à jour de MediaAccess pendant une mise à jour yt-dlp : elle passe.
11. Sans réseau : aucun blocage, aucune fenêtre.
12. Arrêt de Windows pendant une mise à jour : pas de blocage.
13. Anti-robots et vidéo réservée aux adultes : les bons messages.
