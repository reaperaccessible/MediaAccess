# MediaAccess

**Lecteur audio et vidéo accessible** — Tempo, pitch, effets, sous-titres, plein écran, YouTube intégré.

**Accessible audio and video player** — Tempo, pitch, effects, subtitles, fullscreen, YouTube integration.

---

## 🇫🇷 Français

MediaAccess est un lecteur multimédia conçu pour être pleinement accessible aux utilisateurs de lecteurs d'écran (NVDA, JAWS, etc.) tout en offrant des fonctionnalités avancées.

### Fonctionnalités
- **Lecture audio** : MP3, FLAC, WAV, OGG, AAC, M4A, WMA, Opus, et bien d'autres (via BASS)
- **Lecture vidéo** : MP4, MKV, AVI, MOV, WebM, FLV et autres (via libmpv)
- **Contrôles audio avancés** : tempo, pitch, vitesse de lecture indépendants
- **Effets DSP** : réverbération, écho, égaliseur 3 bandes, compresseur, élargissement stéréo, suppression vocale, audio spatial 3D
- **YouTube intégré** : recherche et lecture (audio ou vidéo) via yt-dlp
- **Radio internet** : recherche RadioBrowser, iHeartRadio, TuneIn + favoris
- **Podcasts** : abonnements RSS, téléchargement d'épisodes
- **Planificateur** : programmer des lectures à heures fixes
- **Mode plein écran** vidéo avec sous-titres, sélection piste audio, captures d'écran
- **Bilingue** : français / anglais (auto-détection ou choix manuel)
- **Accessibilité complète** : annonces vocales, navigation clavier, raccourcis personnalisables

### Installation
Téléchargez le dernier installateur depuis [Releases](https://github.com/reaperaccessible/MediaAccess/releases/latest) et exécutez-le.

### Site web
https://reaperaccessible.fr

---

## 🇬🇧 English

MediaAccess is a multimedia player designed for full accessibility with screen readers (NVDA, JAWS, etc.) while offering advanced features.

### Features
- **Audio playback**: MP3, FLAC, WAV, OGG, AAC, M4A, WMA, Opus and many more (via BASS)
- **Video playback**: MP4, MKV, AVI, MOV, WebM, FLV and others (via libmpv)
- **Advanced audio controls**: independent tempo, pitch, and playback speed
- **DSP effects**: reverb, echo, 3-band EQ, compressor, stereo widening, vocal removal, 3D spatial audio
- **Integrated YouTube**: search and playback (audio or video) via yt-dlp
- **Internet radio**: RadioBrowser, iHeartRadio, TuneIn search + favorites
- **Podcasts**: RSS subscriptions, episode downloads
- **Scheduler**: schedule playback at specific times
- **Video fullscreen** with subtitles, audio track selection, screenshots
- **Bilingual**: French / English (auto-detect or manual choice)
- **Full accessibility**: speech announcements, keyboard navigation, customizable shortcuts

### Installation
Download the latest installer from [Releases](https://github.com/reaperaccessible/MediaAccess/releases/latest) and run it.

### Website
https://reaperaccessible.fr

---

## License

MediaAccess is licensed under the **GNU General Public License v3.0**. See [LICENSE](LICENSE) for the full text.

### Third-party libraries
- **BASS** (un4seen.com) — free for non-commercial use
- **libmpv** (mpv.io) — GPL/LGPL
- **Rubber Band Library** — GPL
- **SQLite** — public domain
- **Universal Speech** — MIT
- **yt-dlp** — Unlicense / Public Domain

## Build from source

Run `download-deps.bat` (downloads BASS, libmpv, dependencies) then `build_new.bat` (requires MSVC 2017+).
