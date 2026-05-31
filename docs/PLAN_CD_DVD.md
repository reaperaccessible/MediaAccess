# Plan d'implémentation CD audio + DVD vidéo (DIFFÉRÉ)

> **Statut** : Planifié mais en attente. L'utilisateur n'a pas de DVD de test sous la main au moment de la planification (2026-05-31). Reprendre cette implémentation dès qu'un échantillon de DVDs (non protégé + protégé CSS + audiodécrit) sera disponible.

## Origine

- **Isabelle** (utilisatrice) : « Du bon travail en effet mais je regrette qu'il ne semble pas prendre en charge les supports physiques, CD ou DVD. C'est le cas de beaucoup de lecteurs modernes mais, pour moi, cela n'en fera jamais le successeur de VLC pour les DVDs audiodécrits. »

## Vue d'ensemble

| Livrable | Nouvelles lignes | Effort |
|---|---:|---:|
| CD audio (cd_engine + dialogue) | ~600 | 6-8 h |
| DVD vidéo (video_engine + dvd_dialog) | ~750 | 14-18 h |
| Polish accessibilité (audiodescription + resume SQLite) | ~300 | 4 h |
| Module partagé `disc_drives.cpp/h` | ~120 | 1 h |
| Intégration finale (RC, actions, translations, docs, version, build, installer) | ~250 | 4 h |
| **Total** | **~2020 lignes** | **~30-35 h** |

Calendaire avec 4 agents parallèles + 1 intégrateur : ~1.5 jour.

## Allocation d'IDs `resource.h` (plage 1810-1899 réservée)

| ID | Nom |
|---|---|
| 1810-1822 | CD : menu + dialogue browser + 8 contrôles |
| 1830-1843 | DVD : menu + dialogue browser + 9 contrôles |
| 1850-1852 | DVD : dialogue CSS protégé |
| 1860-1863 | DVD : cycle/jump audiodescription, next/prev title |

(L'ID 1800 a déjà été consommé par la toggle de Sèb en v1.70.)

## Schémas SQLite à ajouter

```sql
CREATE TABLE IF NOT EXISTS cd_resume (
    cddb_id TEXT PRIMARY KEY,
    track INTEGER NOT NULL,
    position_ms INTEGER NOT NULL,
    last_played INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS dvd_resume (
    disc_id TEXT PRIMARY KEY,        -- hash(disc-title + total_ms + title_count)
    disc_title TEXT,
    title_num INTEGER NOT NULL,
    chapter_num INTEGER NOT NULL,
    position_ms INTEGER NOT NULL,
    audio_track INTEGER NOT NULL DEFAULT -1,
    sub_track INTEGER NOT NULL DEFAULT -1,
    last_played INTEGER NOT NULL
);
```

## Décisions clés tranchées en planification

1. **libdvdcss : NON bundlé** (zone grise DMCA USA). Stratégie HandBrake : dialogue clair quand DVD CSS détecté, lien vers VideoLAN, instructions dans le manuel.
2. **Stratégie chargement DVD** : `dvd://N` (pas `dvdnav://`) — meilleure accessibilité, pas de menus animés inaccessibles.
3. **Heuristique audiodescription** : matching insensible à la casse sur `lang` + `title` pour mots-clés : `audiodescr`, `audio description`, `audiodécr`, `dvs`, `-ad-`, ` ad `, `(ad)`, `fra-ad`, `eng-ad`.
4. **Sélection titre par défaut DVD** : le plus long (heuristique VLC, fiable 95% des films).
5. **basscd.lib absent du dossier `lib/`** : à confirmer en début d'implémentation. Si absent, charger en delay-load via `LoadLibrary` (pattern `phonon.dll`).
6. **Module partagé `disc_drives.cpp/h`** : factorisation impérative entre CD et DVD. À créer **en premier**.
7. **Pas de tab Vidéo dans Options en v1.70** (hors scope). Indicateur libdvdcss reporté.
8. **CDDB réseau et CD-TEXT** : reportés. v1 affiche "Track 1", "Track 2", etc., avec resume basé sur le CDDB ID local (calculé sans réseau).
9. **DVD-Audio (DVD-A) et Blu-ray** : explicitement hors scope, à mentionner dans le changelog.
10. **Pas d'AutoPlay à l'insertion** : reporté.

## État actuel des libs

| Composant | Bundlé ? | Emplacement |
|---|---|---|
| **basscd.dll** (CD audio) | **Oui** (29 KB) | `lib\basscd.dll` |
| **basscd.h** | **Oui** | `include\basscd.h` |
| Chargement actuel | Référencé comme plugin BASS (`player.cpp:71`) | Aucune fonction CD réellement appelée |
| **libmpv-2.dll** (DVD) | **Oui** (117 MB — build complète FFmpeg) | `lib\libmpv-2.dll` |
| Engine vidéo | `src/video_engine.cpp` — déjà mature : tracks audio/sub, chapitres, annonces NVDA | 947 lignes, prêt à recevoir `dvd://` |

## Modules à créer / modifier

### Nouveau module `cd_engine.h/cpp`
- Namespace `mediaaccess::cd`
- API : `EnumerateDrives()`, `LoadCdDrive(idx)`, `GetTrackCount()`, `GetTrackLengthSec(t)`, `StartTrack(t)`, `GetCddbId()`, `SaveCdResumePosition()`, `ResumeCdIfKnown()`, `FreeCdSession()`, `IsCdActive()`, `GetCurrentTrack()`
- Intégration BASS : `BASS_CD_StreamCreate` retourne HSTREAM standard → passe par helper privé `FinalizeStreamPipeline(rawDecodeStream, virtualName)` extrait de `LoadFile` (refactor du pipeline pour partage entre file/URL/CD)

### Extension `video_engine.cpp`
- `bool MPVLoadDVD(wchar_t driveLetter, int title)`
- `int MPVGetDvdTitleCount()`
- `double MPVGetDvdTitleLength(int titleIdx)`
- `bool MPVSelectDvdTitle(int title)`
- `std::wstring MPVGetDvdDiscTitle()`
- `bool MPVIsDvdEncrypted()`
- `int MPVFindAudioDescriptionTrack()` (avec heuristique mots-clés)
- Hook `MPV_EVENT_LOG_MESSAGE` pour capter "encrypted" / "css" / "libdvdcss" → `g_dvdEncrypted` + WM_DVD_ENCRYPTED

### Module partagé `disc_drives.cpp/h`
```cpp
enum class DriveMediaType { None, AudioCD, DataCD, DVDVideo, DVDData, Unknown };
struct DiscDrive { wchar_t letter; bool hasMedia; DriveMediaType type; std::wstring volumeLabel; };
std::vector<DiscDrive> EnumerateOpticalDrives();
DriveMediaType ProbeDiscKind(wchar_t letter);
```
Heuristique : `GetLogicalDriveStringsW` + `GetDriveTypeW == DRIVE_CDROM`, puis `VIDEO_TS\` détecté → DVDVideo, sinon `BASS_CD_GetTracks > 0` → AudioCD, sinon DataCD.

### Dialogues à créer
- `IDD_CD_BROWSER` : combo lecteurs + listbox pistes + Actualiser + Lecture + Annuler
- `IDD_DVD_BROWSER` : listbox lecteurs + bouton "Charger les titres" + listbox titres + listbox pistes audio + Lecture + Annuler (worker thread pour le load MPV silencieux + lecture `disc-titles` / `track-list`)
- `IDD_DVD_CSS_PROTECTED` : message clair + bouton "Ouvrir page d'aide" → docs/dvd_libdvdcss_fr.html

## Plan de test manuel (quand DVDs disponibles)

- [ ] **CD** : insérer CD audio commercial, File > Open CD, sélection piste, lecture, navigation N/B, éjection pendant lecture
- [ ] **CD multi-pistes longues** (audiolivre 80 min) : seek dans piste longue
- [ ] **CD resume** : éjecter / ré-insérer même disque → reprise position
- [ ] **DVD non protégé** : graver test, vérifier titres + chapitres + sous-titres + cycle audio
- [ ] **DVD protégé sans libdvdcss** : message d'erreur clair
- [ ] **DVD protégé avec libdvdcss** : installation manuelle, déchiffrement OK
- [ ] **DVD audiodécrit** : action JUMP_TO_AUDIODESCRIPTION, annonce + sélection auto
- [ ] **DVD-A et Blu-ray** : confirmer message "non supporté"
- [ ] **Aucun lecteur optique** : message clair
- [ ] **Lecteur externe USB** : énumération correcte

## Réponse honnête prévue à Isabelle (à envoyer une fois la fonctionnalité livrée)

> Bonjour Isabelle,
>
> Merci pour ce retour, et pour ce point très juste : c'est un manque réel pour les utilisateurs de DVDs audiodécrits, et c'est exactement le type d'usage que MediaAccess se doit de couvrir.
>
> Le support des CD audio et des DVDs vidéo arrivera dans la version 1.7X, avec une attention particulière à la sélection des pistes audio (pour pouvoir basculer facilement sur la piste audiodécrite) et à une annonce vocale claire des titres et chapitres.
>
> Un point d'honnêteté que je préfère vous mentionner dès maintenant : la majorité des DVDs commerciaux sont protégés par un système de chiffrement (CSS). Pour des raisons légales, MediaAccess ne pourra pas inclure d'office le composant qui le déchiffre — il vous suffira alors de placer un petit fichier supplémentaire dans le dossier d'installation (la procédure sera documentée dans le manuel et identique à celle utilisée par HandBrake ou d'autres outils). Sans cela, les DVDs non protégés (gravés, démos, etc.) fonctionneront immédiatement.
>
> Très bonne semaine,
