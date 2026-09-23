# Plan — Gestionnaire de listes de lecture (demande Nicolas, studio radio)

Etat : LIVRE dans la 2.69 (2026-09-23). Les trois travaux sont faits, plus ce que Lee a
demande en testant : Espace = lecture/pause partout dans la fenetre (option), focus
initial sur la deroulante, annonces de bord, F2 pour renommer, fleche droite = nombre
de pistes puis emplacement, Ctrl+S qui reecrit le fichier de la liste affichee.
Specification arretee avec Lee le 2026-09-23.

## Contexte

Nicolas utilise MediaAccess en studio de radio, la liste de lecture tenant lieu de
cartoucheur. Deja livre (2.69, non publie) : la fenetre de liste ne se ferme plus
quand on lance un titre, le curseur suit la piste jouee, l'ouverture depuis
l'Explorateur ne vole plus le focus. Il reste trois demandes.

Rappel : l'enchainement automatique est deja desactivable (Options > Lecture >
"Passer automatiquement a l'element suivant"). Rien a coder, a mentionner dans le
manuel.

## Ce qui est demande

1. Un dossier dedie, reglable, pour l'enregistrement des listes.
2. Ctrl+V d'un fichier de liste.
3. Une liste deroulante des listes enregistrees dans le gestionnaire.

## Decisions arretees (Lee)

- **Selection dans la deroulante** : charge les pistes DANS LA FENETRE, ne touche
  PAS a la lecture en cours. Tab amene aux pistes, Entree lance. Regle de
  securite d'antenne : choisir une liste ne doit jamais couper ce qui passe.
- **Enregistrement** : la boite "Enregistrer sous" reste (l'utilisateur tape un
  nom), mais elle s'ouvre dans le dossier dedie. L'enregistrement ajoute
  automatiquement la liste a la deroulante.
- **Contenu de la deroulante** : un REGISTRE de listes, pas un balayage de
  dossier. Une liste peut vivre n'importe ou sur la machine et reste dans la
  deroulante tant que son fichier existe. Les deux formats lisibles par
  MediaAccess sont acceptes (.m3u/.m3u8 et .pls).
- **Fichier disparu** : entree ignoree, SANS annonce. C'est la responsabilite de
  l'utilisateur (decision explicite de Lee).
- **Suppression** : DEUX boutons distincts, pour ne pas confondre les deux
  sens de "supprimer" :
  - supprimer un titre de la liste courante (comportement actuel de Suppr) ;
  - supprimer la liste : retire l'entree du registre ET demande si le fichier
    doit etre efface du disque.
- **Ctrl+V** : autorise dans la deroulante (ajoute la liste au registre) ET dans
  l'interface principale, ou coller un fichier de liste REMPLACE la lecture en
  cours, comme n'importe quel autre fichier colle. Le collage de fichiers audio
  existant ne change pas.
- **Dossier par defaut** : sous-dossier `Playlist` du dossier de telechargement
  EFFECTIF (celui d'Options > YouTube s'il est regle, sinon le defaut
  historique). Cree automatiquement. Une preference dediee permet de le changer.

## Etat verifie du code (avant travaux)

- `src/ui_playlist.cpp` — fenetre IDD_PLAYLIST. Liste `IDC_PLAYLIST_LIST` en
  **LBS_EXTENDEDSEL**, sous-classee (`PlaylistListProc`) pour Entree, Suppr,
  Alt+Haut/Bas, Ctrl+V. Un seul bouton : `IDC_PLAYLIST_SAVE`. `WM_SIZE`
  repositionne a la main la liste et le bouton (a etendre pour les nouveaux
  controles). `WM_GETMINMAXINFO` impose 350x200 minimum.
- L'enregistrement part de `wchar_t filePath[MAX_PATH] = L"playlist.m3u"` sans
  `lpstrInitialDir` : le fichier tombe la ou Windows a laisse la boite la
  derniere fois.
- `GetFilesFromClipboard()` (`src/ui.cpp`) filtre par `IsOpenableMediaPath()`,
  qui ne contient PAS .m3u/.m3u8/.pls : un fichier de liste colle est donc
  rejete aujourd'hui ("Aucun media dans le presse-papiers").
- `ParsePlaylist()` / `ParseM3U()` / `ParsePLS()` (`src/ui.cpp`) existent deja et
  gerent la signature UTF-8. `IsPlaylistFile()` existe.
- `GetDownloadsTargetDir()` (`src/youtube.cpp`, statique) : preference
  `g_ytDownloadPath` si joignable ET inscriptible, sinon
  `Telechargements\MediaAccess\YouTube`. **Le defaut n'est pas
  `Musique\MediaAccess`** : c'est le reglage personnel de Lee.
- Patron existant pour "champ dossier + Parcourir" : `IDC_YT_DOWNLOAD_PATH` /
  `IDC_YT_DOWNLOAD_PATH_BROWSE` dans `src/ui_options.cpp`.

## Travaux

### 1. Dossier des listes

- Nouveau reglage `g_playlistFolder`, INI `[Playlist] Folder` (vide = defaut).
- Resolution : si vide, `<dossier de telechargement effectif>\..\Playlist` —
  autrement dit le frere du dossier `YouTube`, sous `MediaAccess`. A trancher a
  la relecture : frere de `YouTube`, ou sous-dossier de la preference de
  telechargement telle quelle. Creation automatique, test d'ecriture comme
  `GetDownloadsTargetDir` (repli silencieux si injoignable).
- La fonction de resolution du dossier de telechargement est `static` dans
  `youtube.cpp` : l'exposer proprement plutot que la dupliquer.
- Options : champ + bouton Parcourir, sur le meme patron que YouTube. Onglet a
  choisir (Lecture, probablement).
- `lpstrInitialDir` de la boite d'enregistrement pointe sur ce dossier.

### 2. Ctrl+V d'un fichier de liste

- Ajouter .m3u/.m3u8/.pls a ce que le collage accepte, SANS elargir
  `IsOpenableMediaPath()` (qui sert aussi aux selections multiples de
  l'Explorateur et aux balayages de dossier — l'elargir ferait entrer des
  listes la ou on attend des medias). Prevoir un test dedie.
- Interface principale : un fichier de liste colle est developpe par
  `ParsePlaylist()` et REMPLACE la liste courante, puis lecture, comme le
  collage de fichiers audio.
- Dans la deroulante du gestionnaire : ajoute l'entree au registre et charge les
  pistes dans la fenetre, sans toucher a la lecture.

### 3. Deroulante + registre + boutons

- Registre persistant : INI `[Playlist] RecentN=<chemin>` (nombre a plafonner).
  Alimente par l'enregistrement, par Ctrl+V, et par l'ouverture d'une liste.
- Controle `COMBOBOX` **CBS_DROPDOWNLIST** en haut de la fenetre, avant la liste
  des pistes dans l'ordre de tabulation. Libelle statique associe pour le
  lecteur d'ecran.
- A la selection (`CBN_SELCHANGE`) : `ParsePlaylist()` dans une liste de travail
  affichee, sans toucher `g_playlist` ni la lecture. Entree sur une piste
  bascule alors cette liste de travail en liste active, puis joue.
  **C'est le point de conception le plus delicat** : la fenetre montre
  aujourd'hui `g_playlist` directement. Il faut distinguer "liste affichee" et
  "liste en lecture", sinon on casse le suivi du curseur et Alt+Haut/Bas.
- Entrees dont le fichier n'existe plus : ignorees en silence.
- Deux boutons : "Supprimer le titre" et "Supprimer la liste" (celui-ci demande
  confirmation avant d'effacer le fichier). Suppr dans les pistes garde son
  sens actuel.
- Mettre a jour `WM_SIZE` (nouveaux controles), la ligne d'aide en bas, et
  `WM_GETMINMAXINFO` si la hauteur minimale ne suffit plus.

## Risques

- **Regression sur la fenetre qui vient d'etre corrigee** (2.69) : le suivi du
  curseur, Entree, Alt+Haut/Bas et Suppr dependent tous de `g_playlist`.
  L'introduction d'une liste affichee distincte les touche tous.
- **Ordre de tabulation et annonces** : la deroulante devient le premier
  controle ; verifier que l'ouverture de la fenetre annonce toujours quelque
  chose d'utile et que le focus initial reste coherent.
- **Deux boutons "supprimer"** : risque de confusion a l'oreille. Les libelles
  doivent etre explicites, pas "Suppr" et "Suppr liste".
- Elargir le collage sans elargir les selections de l'Explorateur : verifier
  qu'aucun chemin ne fait entrer un .m3u la ou un media est attendu.

## Tests (verifiables a l'oreille pour Lee)

1. Enregistrer une liste : la boite s'ouvre dans le dossier dedie ; apres
   enregistrement, la liste est dans la deroulante.
2. Choisir une autre liste dans la deroulante PENDANT une lecture : le son ne
   doit PAS s'interrompre. Tab, puis Entree sur une piste : la nouvelle liste
   prend la main.
3. Ctrl+V d'un .m3u dans la fenetre principale : remplace et joue.
4. Ctrl+V d'un .m3u sur la deroulante : la liste s'ajoute, la lecture continue.
5. Supprimer un titre, puis supprimer une liste (repondre non, puis oui a
   l'effacement du fichier) et verifier ce qui reste.
6. Non-regression 2.69 : Entree ne ferme pas la fenetre, le curseur suit la
   piste jouee, Echap ferme.
