// translations_player.cpp -- French translations for player/radio/podcast/youtube/video modules
//
// Keys are the English source text used at call sites (Ts("...") or T("...")).
// Translations should sound natural when spoken by a screen reader.

#include "mediaaccess/translations.h"

void RegisterPlayerTranslations() {
    // --------------------------------------------------------------
    // player.cpp - audio device, BASS init, errors
    // --------------------------------------------------------------
    AddTranslation("en", "No audio devices found", L"No audio devices found");
    AddTranslation("fr", "No audio devices found", L"Aucun périphérique audio trouvé");

    AddTranslation("en", "Switched to ", L"Switched to ");
    AddTranslation("fr", "Switched to ", L"Basculé vers ");

    AddTranslation("en", "Failed to switch audio device", L"Failed to switch audio device");
    AddTranslation("fr", "Failed to switch audio device", L"Impossible de changer de périphérique audio");

    AddTranslation("en", "Failed to initialize BASS audio library.", L"Audio could not be initialized. Please reinstall MediaAccess.");
    AddTranslation("fr", "Failed to initialize BASS audio library.", L"L'audio n'a pas pu être initialisé. Veuillez réinstaller MediaAccess.");

    AddTranslation("en", "Only http:// and https:// URLs are supported.", L"Only http:// and https:// URLs are supported.");
    AddTranslation("fr", "Only http:// and https:// URLs are supported.", L"Seules les URL http:// et https:// sont prises en charge.");

    AddTranslation("en", "Failed to create tempo processor.", L"Failed to create tempo processor.");
    AddTranslation("fr", "Failed to create tempo processor.", L"Impossible de créer le processeur de tempo.");

    AddTranslation("en", "Failed to create tempo stream for URL.", L"Failed to create tempo stream for URL.");
    AddTranslation("fr", "Failed to create tempo stream for URL.", L"Impossible de créer le flux de tempo pour l'URL.");

    AddTranslation("en", "Failed to create tempo stream.", L"Failed to create tempo stream.");
    AddTranslation("fr", "Failed to create tempo stream.", L"Impossible de créer le flux de tempo.");

    // URL load error messages
    AddTranslation("en", "No internet connection.", L"No internet connection.");
    AddTranslation("fr", "No internet connection.", L"Pas de connexion Internet.");

    AddTranslation("en", "Could not connect to URL.", L"Could not connect to URL.");
    AddTranslation("fr", "Could not connect to URL.", L"Impossible de se connecter à l'URL.");

    AddTranslation("en", "Unsupported stream format.", L"Unsupported stream format.");
    AddTranslation("fr", "Unsupported stream format.", L"Format de flux non pris en charge.");

    AddTranslation("en", "Audio format support:", L"Audio format support:");
    AddTranslation("fr", "Audio format support:", L"Formats audio pris en charge :");

    AddTranslation("en", "Some audio format modules failed to load. Please reinstall MediaAccess.",
                   L"Some audio format modules failed to load. Please reinstall MediaAccess.");
    AddTranslation("fr", "Some audio format modules failed to load. Please reinstall MediaAccess.",
                   L"Certains modules de format audio n'ont pas pu être chargés. Veuillez réinstaller MediaAccess.");

    AddTranslation("en", "Required codec is not available.", L"Required codec is not available.");
    AddTranslation("fr", "Required codec is not available.", L"Le codec requis n'est pas disponible.");

    AddTranslation("en", "Unsupported sample format.", L"Unsupported sample format.");
    AddTranslation("fr", "Unsupported sample format.", L"Format d'échantillonnage non pris en charge.");

    AddTranslation("en", "Connection timed out.", L"Connection timed out.");
    AddTranslation("fr", "Connection timed out.", L"Délai de connexion dépassé.");

    AddTranslation("en", "SSL/HTTPS not supported.", L"SSL/HTTPS not supported.");
    AddTranslation("fr", "SSL/HTTPS not supported.", L"SSL/HTTPS non pris en charge.");

    AddTranslation("en", "Could not open stream.", L"Could not open stream.");
    AddTranslation("fr", "Could not open stream.", L"Impossible d'ouvrir le flux.");

    AddTranslation("en", "Cannot play URL:", L"Cannot play URL:");
    AddTranslation("fr", "Cannot play URL:", L"Impossible de lire l'URL :");

    AddTranslation("en", "Error: ", L"Error: ");
    AddTranslation("fr", "Error: ", L"Erreur : ");

    AddTranslation("en", "code ", L"code ");
    AddTranslation("fr", "code ", L"code ");

    // File load error messages
    AddTranslation("en", "Could not open the file.", L"Could not open the file.");
    AddTranslation("fr", "Could not open the file.", L"Impossible d'ouvrir le fichier.");

    AddTranslation("en", "Unsupported file format.", L"Unsupported file format.");
    AddTranslation("fr", "Unsupported file format.", L"Format de fichier non pris en charge.");

    AddTranslation("en", "Out of memory.", L"Out of memory.");
    AddTranslation("fr", "Out of memory.", L"Mémoire insuffisante.");

    AddTranslation("en", "3D sound is not available.", L"3D sound is not available.");
    AddTranslation("fr", "3D sound is not available.", L"Le son 3D n'est pas disponible.");

    AddTranslation("en", "Unknown error.", L"Unknown error.");
    AddTranslation("fr", "Unknown error.", L"Erreur inconnue.");

    AddTranslation("en", "Cannot play file:", L"Cannot play file:");
    AddTranslation("fr", "Cannot play file:", L"Impossible de lire le fichier :");

    AddTranslation("en", "Cannot play video file:", L"Cannot play video file:");
    AddTranslation("fr", "Cannot play video file:", L"Impossible de lire le fichier vidéo :");

    AddTranslation("en", "MediaAccess needs libmpv to play video files.", L"MediaAccess needs libmpv to play video files.");
    AddTranslation("fr", "MediaAccess needs libmpv to play video files.", L"MediaAccess a besoin de libmpv pour lire les fichiers vidéo.");

    AddTranslation("en", "The video playback engine could not be loaded. Please reinstall MediaAccess.",
                   L"The video playback engine could not be loaded. Please reinstall MediaAccess.");
    AddTranslation("fr", "The video playback engine could not be loaded. Please reinstall MediaAccess.",
                   L"Le moteur de lecture vidéo n'a pas pu être chargé. Veuillez réinstaller MediaAccess.");

    // Updater dialog (updater.cpp) — every user-visible string
    AddTranslation("en", "Check for Updates", L"Check for Updates");
    AddTranslation("fr", "Check for Updates", L"Vérifier les mises à jour");

    AddTranslation("en", "Update Available", L"Update Available");
    AddTranslation("fr", "Update Available", L"Mise à jour disponible");

    AddTranslation("en", "Update Error", L"Update Error");
    AddTranslation("fr", "Update Error", L"Erreur de mise à jour");

    AddTranslation("en", "Update", L"Update");
    AddTranslation("fr", "Update", L"Mise à jour");

    AddTranslation("en", "Error", L"Error");
    AddTranslation("fr", "Error", L"Erreur");

    AddTranslation("en", "Failed to connect to GitHub. Please check your internet connection.",
                   L"Failed to connect to GitHub. Please check your internet connection.");
    AddTranslation("fr", "Failed to connect to GitHub. Please check your internet connection.",
                   L"Impossible de se connecter à GitHub. Vérifiez votre connexion Internet.");

    AddTranslation("en", "No releases found.", L"No releases found.");
    AddTranslation("fr", "No releases found.", L"Aucune version trouvée.");

    AddTranslation("en", "No Windows download available for this release.",
                   L"No Windows download available for this release.");
    AddTranslation("fr", "No Windows download available for this release.",
                   L"Aucun téléchargement Windows disponible pour cette version.");

    AddTranslation("en", "No updates available. You are running the latest version.",
                   L"No updates available. You are running the latest version.");
    AddTranslation("fr", "No updates available. You are running the latest version.",
                   L"Aucune mise à jour disponible. Vous utilisez la dernière version.");

    AddTranslation("en", "A new version of MediaAccess is available!",
                   L"A new version of MediaAccess is available!");
    AddTranslation("fr", "A new version of MediaAccess is available!",
                   L"Une nouvelle version de MediaAccess est disponible !");

    AddTranslation("en", "Current version: ", L"Current version: ");
    AddTranslation("fr", "Current version: ", L"Version actuelle : ");

    AddTranslation("en", "Latest version: ", L"Latest version: ");
    AddTranslation("fr", "Latest version: ", L"Dernière version : ");

    AddTranslation("en", "Do you want to download and install the update?",
                   L"Do you want to download and install the update?");
    AddTranslation("fr", "Do you want to download and install the update?",
                   L"Voulez-vous télécharger et installer la mise à jour ?");

    AddTranslation("en", "Update available. ", L"Update available. ");
    AddTranslation("fr", "Update available. ", L"Mise à jour disponible. ");

    AddTranslation("en", "Starting download...", L"Starting download...");
    AddTranslation("fr", "Starting download...", L"Démarrage du téléchargement...");

    AddTranslation("en", "Downloading: %.1f MB / %.1f MB (%d%%)",
                   L"Downloading: %.1f MB / %.1f MB (%d%%)");
    AddTranslation("fr", "Downloading: %.1f MB / %.1f MB (%d%%)",
                   L"Téléchargement : %.1f Mo / %.1f Mo (%d %%)");

    AddTranslation("en", "Failed to download update.", L"Failed to download update.");
    AddTranslation("fr", "Failed to download update.", L"Échec du téléchargement de la mise à jour.");

    AddTranslation("en", "Update file not found. The download may have failed.",
                   L"Update file not found. The download may have failed.");
    AddTranslation("fr", "Update file not found. The download may have failed.",
                   L"Fichier de mise à jour introuvable. Le téléchargement a peut-être échoué.");

    AddTranslation("en", "Failed to launch installer.", L"Failed to launch installer.");
    AddTranslation("fr", "Failed to launch installer.", L"Échec du lancement de l'installateur.");

    AddTranslation("en", "Failed to launch update script.", L"Failed to launch update script.");
    AddTranslation("fr", "Failed to launch update script.", L"Échec du lancement du script de mise à jour.");

    // System tray context menu (tray.cpp)
    AddTranslation("en", "&Restore",    L"&Restore");
    AddTranslation("fr", "&Restore",    L"&Restaurer");
    AddTranslation("en", "&Play/Pause", L"&Play/Pause");
    AddTranslation("fr", "&Play/Pause", L"&Lecture/Pause");
    AddTranslation("en", "&Stop",       L"&Stop");
    AddTranslation("fr", "&Stop",       L"&Arrêter");
    AddTranslation("en", "P&revious",   L"P&revious");
    AddTranslation("fr", "P&revious",   L"P&récédent");
    AddTranslation("en", "&Next",       L"&Next");
    AddTranslation("fr", "&Next",       L"&Suivant");
    AddTranslation("en", "E&xit",       L"E&xit");
    AddTranslation("fr", "E&xit",       L"&Quitter");

    // Ctrl+V clipboard paste on the main window (loads media files/URLs)
    AddTranslation("en", "No media in clipboard", L"No media in clipboard");
    AddTranslation("fr", "No media in clipboard", L"Aucun média dans le presse-papiers");
    AddTranslation("en", "Pasted 1 item", L"Pasted 1 item");
    AddTranslation("fr", "Pasted 1 item", L"1 élément collé");
    AddTranslation("en", "items pasted", L"items pasted");
    AddTranslation("fr", "items pasted", L"éléments collés");

    // Keyboard help mode (F12) — describe-key strings.
    // Each command name is translated; the keyboard_help.cpp formatter
    // composes "Modifier+Key: Action" automatically.
    AddTranslation("en", "Keyboard help on",  L"Keyboard help on");
    AddTranslation("fr", "Keyboard help on",  L"Aide clavier activée");
    AddTranslation("en", "Keyboard help off", L"Keyboard help off");
    AddTranslation("fr", "Keyboard help off", L"Aide clavier désactivée");

    AddTranslation("en", "KEY_MOD_CTRL",  L"Ctrl");
    AddTranslation("fr", "KEY_MOD_CTRL",  L"Ctrl");
    AddTranslation("en", "KEY_MOD_SHIFT", L"Shift");
    AddTranslation("fr", "KEY_MOD_SHIFT", L"Maj");
    AddTranslation("en", "KEY_MOD_ALT",   L"Alt");
    AddTranslation("fr", "KEY_MOD_ALT",   L"Alt");

    AddTranslation("en", "no action assigned", L"no action assigned");
    AddTranslation("fr", "no action assigned", L"aucune action assignée");

    // Command descriptions
    AddTranslation("en", "Previous track", L"Previous track");
    AddTranslation("fr", "Previous track", L"Piste précédente");
    AddTranslation("en", "Next track", L"Next track");
    AddTranslation("fr", "Next track", L"Piste suivante");
    AddTranslation("en", "Play", L"Play");
    AddTranslation("fr", "Play", L"Lecture");
    AddTranslation("en", "Pause", L"Pause");
    AddTranslation("fr", "Pause", L"Pause");
    AddTranslation("en", "Stop", L"Stop");
    AddTranslation("fr", "Stop", L"Arrêter");
    AddTranslation("en", "Play/Pause", L"Play/Pause");
    AddTranslation("fr", "Play/Pause", L"Lecture/Pause");
    AddTranslation("en", "Beginning of track", L"Beginning of track");
    AddTranslation("fr", "Beginning of track", L"Début de la piste");
    AddTranslation("en", "Seek backward", L"Seek backward");
    AddTranslation("fr", "Seek backward", L"Reculer");
    AddTranslation("en", "Seek forward", L"Seek forward");
    AddTranslation("fr", "Seek forward", L"Avancer");
    AddTranslation("en", "Decrease seek unit", L"Decrease seek unit");
    AddTranslation("fr", "Decrease seek unit", L"Unité de déplacement précédente");
    AddTranslation("fr", "Increase seek unit", L"Unité de déplacement suivante");
    AddTranslation("en", "Increase seek unit", L"Increase seek unit");
    AddTranslation("en", "Cycle repeat mode", L"Cycle repeat mode");
    AddTranslation("fr", "Cycle repeat mode", L"Changer le mode de répétition");
    AddTranslation("en", "Toggle shuffle", L"Toggle shuffle");
    AddTranslation("fr", "Toggle shuffle", L"Activer/désactiver la lecture aléatoire");
    AddTranslation("en", "Toggle mute", L"Toggle mute");
    AddTranslation("fr", "Toggle mute", L"Activer/désactiver le muet");
    AddTranslation("en", "Toggle recording", L"Toggle recording");
    AddTranslation("fr", "Toggle recording", L"Démarrer/arrêter l'enregistrement");
    AddTranslation("en", "Volume up", L"Volume up");
    AddTranslation("fr", "Volume up", L"Augmenter le volume");
    AddTranslation("en", "Volume down", L"Volume down");
    AddTranslation("fr", "Volume down", L"Diminuer le volume");
    AddTranslation("en", "Add bookmark", L"Add bookmark");
    AddTranslation("fr", "Add bookmark", L"Ajouter un signet");
    AddTranslation("en", "Bookmarks manager", L"Bookmarks manager");
    AddTranslation("fr", "Bookmarks manager", L"Gestionnaire de signets");
    AddTranslation("en", "Jump to time", L"Jump to time");
    AddTranslation("fr", "Jump to time", L"Aller à un instant");
    AddTranslation("en", "Effect presets menu", L"Effect presets menu");
    AddTranslation("fr", "Effect presets menu", L"Menu des préréglages d'effets");
    AddTranslation("en", "Audio device menu", L"Audio device menu");
    AddTranslation("fr", "Audio device menu", L"Menu des périphériques audio");
    AddTranslation("en", "Previous effect parameter", L"Previous effect parameter");
    AddTranslation("fr", "Previous effect parameter", L"Paramètre d'effet précédent");
    AddTranslation("en", "Next effect parameter", L"Next effect parameter");
    AddTranslation("fr", "Next effect parameter", L"Paramètre d'effet suivant");
    AddTranslation("en", "Increase current parameter", L"Increase current parameter");
    AddTranslation("fr", "Increase current parameter", L"Augmenter le paramètre courant");
    AddTranslation("en", "Decrease current parameter", L"Decrease current parameter");
    AddTranslation("fr", "Decrease current parameter", L"Diminuer le paramètre courant");
    AddTranslation("en", "Reset effect to default", L"Reset effect to default");
    AddTranslation("fr", "Reset effect to default", L"Réinitialiser l'effet à sa valeur par défaut");
    AddTranslation("en", "Set effect to minimum", L"Set effect to minimum");
    AddTranslation("fr", "Set effect to minimum", L"Régler l'effet au minimum");
    AddTranslation("en", "Set effect to maximum", L"Set effect to maximum");
    AddTranslation("fr", "Set effect to maximum", L"Régler l'effet au maximum");

    // File menu
    AddTranslation("en", "Open file", L"Open file");
    AddTranslation("fr", "Open file", L"Ouvrir un fichier");
    AddTranslation("en", "Add folder", L"Add folder");
    AddTranslation("fr", "Add folder", L"Ajouter un dossier");
    AddTranslation("en", "Playlist", L"Playlist");
    AddTranslation("fr", "Playlist", L"Liste de lecture");
    AddTranslation("en", "Open URL", L"Open URL");
    AddTranslation("fr", "Open URL", L"Ouvrir une URL");
    AddTranslation("en", "Add stream to favorites", L"Add stream to favorites");
    AddTranslation("fr", "Add stream to favorites", L"Ajouter le flux aux favoris");
    AddTranslation("en", "Podcasts", L"Podcasts");
    AddTranslation("fr", "Podcasts", L"Podcasts");
    AddTranslation("en", "Schedule", L"Schedule");
    AddTranslation("fr", "Schedule", L"Planificateur");
    AddTranslation("en", "Hide to tray", L"Hide to tray");
    AddTranslation("fr", "Hide to tray", L"Réduire dans la zone de notification");
    AddTranslation("en", "Song history", L"Song history");
    AddTranslation("fr", "Song history", L"Historique des morceaux");
    AddTranslation("en", "Options", L"Options");
    AddTranslation("fr", "Options", L"Options");
    AddTranslation("en", "Paste from clipboard", L"Paste from clipboard");
    AddTranslation("fr", "Paste from clipboard", L"Coller depuis le presse-papiers");
    AddTranslation("en", "Radio", L"Radio");
    AddTranslation("fr", "Radio", L"Radio");
    AddTranslation("en", "YouTube", L"YouTube");

    // Speech
    AddTranslation("en", "Speak elapsed time", L"Speak elapsed time");
    AddTranslation("fr", "Speak elapsed time", L"Énoncer le temps écoulé");
    AddTranslation("en", "Speak remaining time", L"Speak remaining time");
    AddTranslation("fr", "Speak remaining time", L"Énoncer le temps restant");
    AddTranslation("en", "Speak total duration", L"Speak total duration");
    AddTranslation("fr", "Speak total duration", L"Énoncer la durée totale");

    // Effect toggles (Ctrl+1..0, Ctrl+-, Ctrl+=)
    AddTranslation("en", "Toggle Volume", L"Toggle Volume");
    AddTranslation("fr", "Toggle Volume", L"Activer/désactiver le volume");
    AddTranslation("en", "Toggle Pitch", L"Toggle Pitch");
    AddTranslation("fr", "Toggle Pitch", L"Activer/désactiver la hauteur");
    AddTranslation("en", "Toggle Tempo", L"Toggle Tempo");
    AddTranslation("fr", "Toggle Tempo", L"Activer/désactiver le tempo");
    AddTranslation("en", "Toggle Rate", L"Toggle Rate");
    AddTranslation("fr", "Toggle Rate", L"Activer/désactiver la vitesse");
    AddTranslation("en", "Cycle Reverb algorithm", L"Cycle Reverb algorithm");
    AddTranslation("fr", "Cycle Reverb algorithm", L"Changer d'algorithme de réverbération");
    AddTranslation("en", "Toggle Echo", L"Toggle Echo");
    AddTranslation("fr", "Toggle Echo", L"Activer/désactiver l'écho");
    AddTranslation("en", "Toggle Equalizer", L"Toggle Equalizer");
    AddTranslation("fr", "Toggle Equalizer", L"Activer/désactiver l'égaliseur");
    AddTranslation("en", "Toggle Compressor", L"Toggle Compressor");
    AddTranslation("fr", "Toggle Compressor", L"Activer/désactiver le compresseur");
    AddTranslation("en", "Toggle Stereo Width", L"Toggle Stereo Width");
    AddTranslation("fr", "Toggle Stereo Width", L"Activer/désactiver la largeur stéréo");
    AddTranslation("en", "Toggle Center Cancel", L"Toggle Center Cancel");
    AddTranslation("fr", "Toggle Center Cancel", L"Activer/désactiver la suppression du centre");
    AddTranslation("en", "Toggle Convolution Reverb", L"Toggle Convolution Reverb");
    AddTranslation("fr", "Toggle Convolution Reverb", L"Activer/désactiver la réverbération à convolution");
    AddTranslation("en", "Toggle 3D Audio", L"Toggle 3D Audio");
    AddTranslation("fr", "Toggle 3D Audio", L"Activer/désactiver l'audio 3D");

    // Tag reading (1-0)
    AddTranslation("en", "Speak title tag", L"Speak title tag");
    AddTranslation("fr", "Speak title tag", L"Énoncer le titre");
    AddTranslation("en", "Speak artist tag", L"Speak artist tag");
    AddTranslation("fr", "Speak artist tag", L"Énoncer l'artiste");
    AddTranslation("en", "Speak album tag", L"Speak album tag");
    AddTranslation("fr", "Speak album tag", L"Énoncer l'album");
    AddTranslation("en", "Speak year tag", L"Speak year tag");
    AddTranslation("fr", "Speak year tag", L"Énoncer l'année");
    AddTranslation("en", "Speak track number tag", L"Speak track number tag");
    AddTranslation("fr", "Speak track number tag", L"Énoncer le numéro de piste");
    AddTranslation("en", "Speak genre tag", L"Speak genre tag");
    AddTranslation("fr", "Speak genre tag", L"Énoncer le genre");
    AddTranslation("en", "Speak comment tag", L"Speak comment tag");
    AddTranslation("fr", "Speak comment tag", L"Énoncer le commentaire");
    AddTranslation("en", "Speak bitrate tag", L"Speak bitrate tag");
    AddTranslation("fr", "Speak bitrate tag", L"Énoncer le débit");
    AddTranslation("en", "Speak duration tag", L"Speak duration tag");
    AddTranslation("fr", "Speak duration tag", L"Énoncer la durée");
    AddTranslation("en", "Speak filename", L"Speak filename");
    AddTranslation("fr", "Speak filename", L"Énoncer le nom de fichier");

    // Tag display (Shift+1-0)
    AddTranslation("en", "Show title in window", L"Show title in window");
    AddTranslation("fr", "Show title in window", L"Afficher le titre");
    AddTranslation("en", "Show artist in window", L"Show artist in window");
    AddTranslation("fr", "Show artist in window", L"Afficher l'artiste");
    AddTranslation("en", "Show album in window", L"Show album in window");
    AddTranslation("fr", "Show album in window", L"Afficher l'album");
    AddTranslation("en", "Show year in window", L"Show year in window");
    AddTranslation("fr", "Show year in window", L"Afficher l'année");
    AddTranslation("en", "Show track number in window", L"Show track number in window");
    AddTranslation("fr", "Show track number in window", L"Afficher le numéro de piste");
    AddTranslation("en", "Show genre in window", L"Show genre in window");
    AddTranslation("fr", "Show genre in window", L"Afficher le genre");
    AddTranslation("en", "Show comment in window", L"Show comment in window");
    AddTranslation("fr", "Show comment in window", L"Afficher le commentaire");
    AddTranslation("en", "Show bitrate in window", L"Show bitrate in window");
    AddTranslation("fr", "Show bitrate in window", L"Afficher le débit");
    AddTranslation("en", "Show duration in window", L"Show duration in window");
    AddTranslation("fr", "Show duration in window", L"Afficher la durée");
    AddTranslation("en", "Show filename in window", L"Show filename in window");
    AddTranslation("fr", "Show filename in window", L"Afficher le nom de fichier");

    // Video
    AddTranslation("en", "Toggle fullscreen", L"Toggle fullscreen");
    AddTranslation("fr", "Toggle fullscreen", L"Activer/désactiver le plein écran");
    AddTranslation("en", "Cycle subtitles", L"Cycle subtitles");
    AddTranslation("fr", "Cycle subtitles", L"Changer de sous-titres");
    AddTranslation("en", "Cycle audio track", L"Cycle audio track");
    AddTranslation("fr", "Cycle audio track", L"Changer de piste audio");
    AddTranslation("en", "Take screenshot", L"Take screenshot");
    AddTranslation("fr", "Take screenshot", L"Capture d'écran");

    // Help
    AddTranslation("en", "Open manual", L"Open manual");
    AddTranslation("fr", "Open manual", L"Ouvrir le manuel");
    AddTranslation("en", "Toggle keyboard help", L"Toggle keyboard help");
    AddTranslation("fr", "Toggle keyboard help", L"Activer/désactiver l'aide clavier");

    // Help menu — Manual (F1) opens the bilingual HTML manual
    AddTranslation("en", "&Manual\tF1", L"&Manual\tF1");
    AddTranslation("fr", "&Manual\tF1", L"&Manuel\tF1");
    AddTranslation("en", "Manual", L"Manual");
    AddTranslation("fr", "Manual", L"Manuel");
    AddTranslation("en", "Could not open the manual. Make sure the docs folder is present alongside MediaAccess.exe.",
                   L"The manual could not be opened. Please reinstall MediaAccess.");
    AddTranslation("fr", "Could not open the manual. Make sure the docs folder is present alongside MediaAccess.exe.",
                   L"Le manuel n'a pas pu être ouvert. Veuillez réinstaller MediaAccess.");

    // Video window right-click context menu (main.cpp WM_RBUTTONUP)
    AddTranslation("en", "Fullscreen\tF11",       L"Fullscreen\tF11");
    AddTranslation("fr", "Fullscreen\tF11",       L"Plein écran\tF11");
    AddTranslation("en", "Cycle Subtitles",       L"Cycle Subtitles");
    AddTranslation("fr", "Cycle Subtitles",       L"Changer de sous-titres");
    AddTranslation("en", "Load Subtitle File...", L"Load Subtitle File...");
    AddTranslation("fr", "Load Subtitle File...", L"Charger un fichier de sous-titres...");
    AddTranslation("en", "Cycle Audio Track",     L"Cycle Audio Track");
    AddTranslation("fr", "Cycle Audio Track",     L"Changer de piste audio");
    AddTranslation("en", "Cycle Aspect Ratio",    L"Cycle Aspect Ratio");
    AddTranslation("fr", "Cycle Aspect Ratio",    L"Changer le format d'image");
    AddTranslation("en", "Take Screenshot",       L"Take Screenshot");
    AddTranslation("fr", "Take Screenshot",       L"Capture d'écran");

    // Video engine messages (player.cpp routing to mpv)
    AddTranslation("en", "Failed to initialize video engine", L"Failed to initialize video engine");
    AddTranslation("fr", "Failed to initialize video engine", L"Échec de l'initialisation du moteur vidéo");

    AddTranslation("en", "Failed to load video", L"Failed to load video");
    AddTranslation("fr", "Failed to load video", L"Échec du chargement de la vidéo");

    AddTranslation("en", "Failed to load video URL", L"Failed to load video URL");
    AddTranslation("fr", "Failed to load video URL", L"Échec du chargement de l'URL vidéo");

    // Live stream / pause
    AddTranslation("en", "Cannot pause live stream", L"Cannot pause live stream");
    AddTranslation("fr", "Cannot pause live stream", L"Impossible de mettre en pause un flux en direct");

    // Chapters / beginning
    AddTranslation("en", "Chapter ", L"Chapter ");
    AddTranslation("fr", "Chapter ", L"Chapitre ");

    AddTranslation("en", "Beginning", L"Beginning");
    AddTranslation("fr", "Beginning", L"Début");

    // Volume / mute
    AddTranslation("en", "Volume %d%%", L"Volume %d%%");
    AddTranslation("fr", "Volume %d%%", L"Volume %d%%");

    AddTranslation("en", "Muted", L"Muted");
    AddTranslation("fr", "Muted", L"Muet");

    AddTranslation("en", "Unmuted", L"Unmuted");
    AddTranslation("fr", "Unmuted", L"Son rétabli");

    // Repeat modes
    AddTranslation("en", "Repeat off", L"Repeat off");
    AddTranslation("fr", "Repeat off", L"Répétition désactivée");

    AddTranslation("en", "Repeat track", L"Repeat track");
    AddTranslation("fr", "Repeat track", L"Répéter la piste");

    AddTranslation("en", "Repeat all", L"Repeat all");
    AddTranslation("fr", "Repeat all", L"Répéter tout");

    // Tags / metadata
    AddTranslation("en", "Nothing playing", L"Nothing playing");
    AddTranslation("fr", "Nothing playing", L"Aucune lecture en cours");

    AddTranslation("en", "No title", L"No title");
    AddTranslation("fr", "No title", L"Aucun titre");

    AddTranslation("en", "No artist", L"No artist");
    AddTranslation("fr", "No artist", L"Aucun artiste");

    AddTranslation("en", "No album", L"No album");
    AddTranslation("fr", "No album", L"Aucun album");

    AddTranslation("en", "No year", L"No year");
    AddTranslation("fr", "No year", L"Aucune année");

    AddTranslation("en", "No track number", L"No track number");
    AddTranslation("fr", "No track number", L"Aucun numéro de piste");

    AddTranslation("en", "No track", L"No track");
    AddTranslation("fr", "No track", L"Aucune piste");

    AddTranslation("en", "No genre", L"No genre");
    AddTranslation("fr", "No genre", L"Aucun genre");

    AddTranslation("en", "No comment", L"No comment");
    AddTranslation("fr", "No comment", L"Aucun commentaire");

    AddTranslation("en", "Artist: ", L"Artist: ");
    AddTranslation("fr", "Artist: ", L"Artiste : ");

    AddTranslation("en", "Album: ", L"Album: ");
    AddTranslation("fr", "Album: ", L"Album : ");

    AddTranslation("en", "Station: ", L"Station: ");
    AddTranslation("fr", "Station: ", L"Station : ");

    AddTranslation("en", "Year: ", L"Year: ");
    AddTranslation("fr", "Year: ", L"Année : ");

    AddTranslation("en", "Track: ", L"Track: ");
    AddTranslation("fr", "Track: ", L"Piste : ");

    AddTranslation("en", "Genre: ", L"Genre: ");
    AddTranslation("fr", "Genre: ", L"Genre : ");

    AddTranslation("en", "Comment: ", L"Comment: ");
    AddTranslation("fr", "Comment: ", L"Commentaire : ");

    AddTranslation("en", "Cannot get info", L"Cannot get info");
    AddTranslation("fr", "Cannot get info", L"Impossible d'obtenir les informations");

    AddTranslation("en", "mono", L"mono");
    AddTranslation("fr", "mono", L"mono");

    AddTranslation("en", "stereo", L"stereo");
    AddTranslation("fr", "stereo", L"stéréo");

    AddTranslation("en", "multi-channel", L"multi-channel");
    AddTranslation("fr", "multi-channel", L"multicanal");

    AddTranslation("en", "Mono", L"Mono");
    AddTranslation("fr", "Mono", L"Mono");

    AddTranslation("en", "Stereo", L"Stereo");
    AddTranslation("fr", "Stereo", L"Stéréo");

    AddTranslation("en", "Multi-channel", L"Multi-channel");
    AddTranslation("fr", "Multi-channel", L"Multicanal");

    AddTranslation("en", "%d kbps, %d Hz, %s", L"%d kbps, %d Hz, %s");
    AddTranslation("fr", "%d kbps, %d Hz, %s", L"%d kbps, %d Hz, %s");

    AddTranslation("en", "%d-bit, %d Hz, %s", L"%d-bit, %d Hz, %s");
    AddTranslation("fr", "%d-bit, %d Hz, %s", L"%d-bit, %d Hz, %s");

    AddTranslation("en", "%.0f kbps, %d Hz, %s", L"%.0f kbps, %d Hz, %s");
    AddTranslation("fr", "%.0f kbps, %d Hz, %s", L"%.0f kbps, %d Hz, %s");

    AddTranslation("en", "Unknown bitrate", L"Unknown bitrate");
    AddTranslation("fr", "Unknown bitrate", L"Débit binaire inconnu");

    AddTranslation("en", "Live stream", L"Live stream");
    AddTranslation("fr", "Live stream", L"Flux en direct");

    AddTranslation("en", "Unknown duration", L"Unknown duration");
    AddTranslation("fr", "Unknown duration", L"Durée inconnue");

    AddTranslation("en", "Duration: %d hours, %d minutes, %d seconds", L"Duration: %d hours, %d minutes, %d seconds");
    AddTranslation("fr", "Duration: %d hours, %d minutes, %d seconds", L"Durée : %d heures, %d minutes, %d secondes");

    AddTranslation("en", "Duration: %d minutes, %d seconds", L"Duration: %d minutes, %d seconds");
    AddTranslation("fr", "Duration: %d minutes, %d seconds", L"Durée : %d minutes, %d secondes");

    AddTranslation("en", "Duration: %d seconds", L"Duration: %d seconds");
    AddTranslation("fr", "Duration: %d seconds", L"Durée : %d secondes");

    AddTranslation("en", "URL: ", L"URL: ");
    AddTranslation("fr", "URL: ", L"URL : ");

    AddTranslation("en", "Filename: ", L"Filename: ");
    AddTranslation("fr", "Filename: ", L"Nom de fichier : ");

    // Recording
    AddTranslation("en", "Recording stopped", L"Recording stopped");
    AddTranslation("fr", "Recording stopped", L"Enregistrement arrêté");

    AddTranslation("en", "Recording started", L"Recording started");
    AddTranslation("fr", "Recording started", L"Enregistrement démarré");

    AddTranslation("en", "Nothing to record", L"Nothing to record");
    AddTranslation("fr", "Nothing to record", L"Rien à enregistrer");

    AddTranslation("en", "Cannot get stream info", L"Cannot get stream info");
    AddTranslation("fr", "Cannot get stream info", L"Impossible d'obtenir les informations du flux");

    AddTranslation("en", "MP3 encoding failed.\nFalling back to WAV format.",
                   L"MP3 encoding failed.\nFalling back to WAV format.");
    AddTranslation("fr", "MP3 encoding failed.\nFalling back to WAV format.",
                   L"L'encodage MP3 a échoué.\nRetour au format WAV.");

    AddTranslation("en", "OGG encoding failed.\nFalling back to WAV format.",
                   L"OGG encoding failed.\nFalling back to WAV format.");
    AddTranslation("fr", "OGG encoding failed.\nFalling back to WAV format.",
                   L"L'encodage OGG a échoué.\nRetour au format WAV.");

    AddTranslation("en", "FLAC encoding failed.\nFalling back to WAV format.",
                   L"FLAC encoding failed.\nFalling back to WAV format.");
    AddTranslation("fr", "FLAC encoding failed.\nFalling back to WAV format.",
                   L"L'encodage FLAC a échoué.\nRetour au format WAV.");

    AddTranslation("en", "Failed to start recording (error %d)", L"Recording could not be started.");
    AddTranslation("fr", "Failed to start recording (error %d)", L"L'enregistrement n'a pas pu démarrer.");

    // --------------------------------------------------------------
    // video_engine.cpp - mpv playback
    // --------------------------------------------------------------
    AddTranslation("en", "Fullscreen", L"Fullscreen");
    AddTranslation("fr", "Fullscreen", L"Plein écran");

    AddTranslation("en", "Windowed", L"Windowed");
    AddTranslation("fr", "Windowed", L"Mode fenêtré");

    AddTranslation("en", "Subtitles off", L"Subtitles off");
    AddTranslation("fr", "Subtitles off", L"Sous-titres désactivés");

    AddTranslation("en", "Subtitle: ", L"Subtitle: ");
    AddTranslation("fr", "Subtitle: ", L"Sous-titre : ");

    AddTranslation("en", "Subtitle loaded", L"Subtitle loaded");
    AddTranslation("fr", "Subtitle loaded", L"Sous-titre chargé");

    AddTranslation("en", "Failed to load subtitle", L"Failed to load subtitle");
    AddTranslation("fr", "Failed to load subtitle", L"Échec du chargement du sous-titre");

    AddTranslation("en", "Audio: ", L"Audio: ");
    AddTranslation("fr", "Audio: ", L"Audio : ");

    AddTranslation("en", "Aspect: ", L"Aspect: ");
    AddTranslation("fr", "Aspect: ", L"Format : ");

    AddTranslation("en", "Screenshot saved", L"Screenshot saved");
    AddTranslation("fr", "Screenshot saved", L"Capture d'écran sauvegardée");

    // --------------------------------------------------------------
    // ui_radio.cpp
    // --------------------------------------------------------------
    AddTranslation("en", "Please enter a station name.", L"Please enter a station name.");
    AddTranslation("fr", "Please enter a station name.", L"Veuillez saisir un nom de station.");

    AddTranslation("en", "Please enter a stream URL.", L"Please enter a stream URL.");
    AddTranslation("fr", "Please enter a stream URL.", L"Veuillez saisir une URL de flux.");

    AddTranslation("en", "Failed to add station.", L"Failed to add station.");
    AddTranslation("fr", "Failed to add station.", L"Impossible d'ajouter la station.");

    AddTranslation("en", "Add Station", L"Add Station");
    AddTranslation("fr", "Add Station", L"Ajouter une station");

    AddTranslation("en", "Edit Station", L"Edit Station");
    AddTranslation("fr", "Edit Station", L"Modifier la station");

    AddTranslation("en", "Could not get stream URL", L"Could not get stream URL");
    AddTranslation("fr", "Could not get stream URL", L"Impossible d'obtenir l'URL du flux");

    AddTranslation("en", "URL copied", L"URL copied");
    AddTranslation("fr", "URL copied", L"URL copiée");

    AddTranslation("en", "Station updated", L"Station updated");
    AddTranslation("fr", "Station updated", L"Station mise à jour");

    AddTranslation("en", "Station removed", L"Station removed");
    AddTranslation("fr", "Station removed", L"Station supprimée");

    AddTranslation("en", "Station added", L"Station added");
    AddTranslation("fr", "Station added", L"Station ajoutée");

    AddTranslation("en", " moved to top", L" moved to top");
    AddTranslation("fr", " moved to top", L" déplacée en haut");

    AddTranslation("en", " moved below ", L" moved below ");
    AddTranslation("fr", " moved below ", L" déplacée sous ");

    AddTranslation("en", "Imported %d stations", L"Imported %d stations");
    AddTranslation("fr", "Imported %d stations", L"%d stations importées");

    AddTranslation("en", "No stations found to import", L"No stations found to import");
    AddTranslation("fr", "No stations found to import", L"Aucune station trouvée à importer");

    AddTranslation("en", "Exported %d stations", L"Exported %d stations");
    AddTranslation("fr", "Exported %d stations", L"%d stations exportées");

    AddTranslation("en", "No favorites to export", L"No favorites to export");
    AddTranslation("fr", "No favorites to export", L"Aucun favori à exporter");

    AddTranslation("en", "Failed to write file", L"Failed to write file");
    AddTranslation("fr", "Failed to write file", L"Échec de l'écriture du fichier");

    AddTranslation("en", "Enter a search term", L"Enter a search term");
    AddTranslation("fr", "Enter a search term", L"Saisissez un terme de recherche");

    AddTranslation("en", "Searching", L"Searching");
    AddTranslation("fr", "Searching", L"Recherche en cours");

    AddTranslation("en", "Found %d stations", L"Found %d stations");
    AddTranslation("fr", "Found %d stations", L"%d stations trouvées");

    AddTranslation("en", "No stations found", L"No stations found");
    AddTranslation("fr", "No stations found", L"Aucune station trouvée");

    AddTranslation("en", "Added to favorites", L"Added to favorites");
    AddTranslation("fr", "Added to favorites", L"Ajouté aux favoris");

    AddTranslation("en", "Failed to add station", L"Failed to add station");
    AddTranslation("fr", "Failed to add station", L"Impossible d'ajouter la station");

    AddTranslation("en", "Select a station first", L"Select a station first");
    AddTranslation("fr", "Select a station first", L"Sélectionnez d'abord une station");

    AddTranslation("en", "No stream playing", L"No stream playing");
    AddTranslation("fr", "No stream playing", L"Aucun flux en cours de lecture");

    AddTranslation("en", "Not a stream", L"Not a stream");
    AddTranslation("fr", "Not a stream", L"Pas un flux");

    AddTranslation("en", "Stream already in favorites", L"Stream already in favorites");
    AddTranslation("fr", "Stream already in favorites", L"Le flux est déjà dans les favoris");

    AddTranslation("en", "Favorites", L"Favorites");
    AddTranslation("fr", "Favorites", L"Favoris");

    AddTranslation("en", "Search", L"Search");
    AddTranslation("fr", "Search", L"Rechercher");

    // --------------------------------------------------------------
    // ui_podcast.cpp
    // --------------------------------------------------------------
    AddTranslation("en", "Loading episodes", L"Loading episodes");
    AddTranslation("fr", "Loading episodes", L"Chargement des épisodes");

    AddTranslation("en", "%d episodes", L"%d episodes");
    AddTranslation("fr", "%d episodes", L"%d épisodes");

    AddTranslation("en", "Failed to load episodes", L"Failed to load episodes");
    AddTranslation("fr", "Failed to load episodes", L"Échec du chargement des épisodes");

    AddTranslation("en", "Podcast Load Failed", L"Podcast Load Failed");
    AddTranslation("fr", "Podcast Load Failed", L"Échec du chargement du podcast");

    AddTranslation("en", "Please enter a feed URL.", L"Please enter a feed URL.");
    AddTranslation("fr", "Please enter a feed URL.", L"Veuillez saisir une URL de flux.");

    AddTranslation("en", "Add Podcast", L"Add Podcast");
    AddTranslation("fr", "Add Podcast", L"Ajouter un podcast");

    AddTranslation("en", "Unsubscribe from \"", L"Unsubscribe from \"");
    AddTranslation("fr", "Unsubscribe from \"", L"Se désabonner de \"");

    AddTranslation("en", "Unsubscribe", L"Unsubscribe");
    AddTranslation("fr", "Unsubscribe", L"Se désabonner");

    AddTranslation("en", "Unsubscribed", L"Unsubscribed");
    AddTranslation("fr", "Unsubscribed", L"Désabonné");

    AddTranslation("en", "Subscriptions", L"Subscriptions");
    AddTranslation("fr", "Subscriptions", L"Abonnements");

    AddTranslation("en", "Playing", L"Playing");
    AddTranslation("fr", "Playing", L"Lecture en cours");

    AddTranslation("en", "Playing %d episodes", L"Playing %d episodes");
    AddTranslation("fr", "Playing %d episodes", L"Lecture de %d épisodes");

    AddTranslation("en", "Loading preview", L"Loading preview");
    AddTranslation("fr", "Loading preview", L"Chargement de l'aperçu");

    AddTranslation("en", "No episodes found", L"No episodes found");
    AddTranslation("fr", "No episodes found", L"Aucun épisode trouvé");

    AddTranslation("en", "No episode selected", L"No episode selected");
    AddTranslation("fr", "No episode selected", L"Aucun épisode sélectionné");

    AddTranslation("en", "Please set a downloads folder in Options", L"Please set a downloads folder in Options");
    AddTranslation("fr", "Please set a downloads folder in Options", L"Veuillez définir un dossier de téléchargements dans les Options");

    AddTranslation("en", "All selected episodes already downloaded", L"All selected episodes already downloaded");
    AddTranslation("fr", "All selected episodes already downloaded", L"Tous les épisodes sélectionnés sont déjà téléchargés");

    AddTranslation("en", "No episodes to download", L"No episodes to download");
    AddTranslation("fr", "No episodes to download", L"Aucun épisode à télécharger");

    AddTranslation("en", "Downloading", L"Downloading");
    AddTranslation("fr", "Downloading", L"Téléchargement en cours");

    AddTranslation("en", "Downloading %d episodes", L"Downloading %d episodes");
    AddTranslation("fr", "Downloading %d episodes", L"Téléchargement de %d épisodes");

    AddTranslation("en", "Downloading %d episodes, %d skipped", L"Downloading %d episodes, %d skipped");
    AddTranslation("fr", "Downloading %d episodes, %d skipped", L"Téléchargement de %d épisodes, %d ignorés");

    AddTranslation("en", "No episodes loaded", L"No episodes loaded");
    AddTranslation("fr", "No episodes loaded", L"Aucun épisode chargé");

    AddTranslation("en", "No subscriptions to export", L"No subscriptions to export");
    AddTranslation("fr", "No subscriptions to export", L"Aucun abonnement à exporter");

    AddTranslation("en", "Export OPML", L"Export OPML");
    AddTranslation("fr", "Export OPML", L"Exporter OPML");

    AddTranslation("en", "Import OPML", L"Import OPML");
    AddTranslation("fr", "Import OPML", L"Importer OPML");

    AddTranslation("en", "Exported %d subscriptions", L"Exported %d subscriptions");
    AddTranslation("fr", "Exported %d subscriptions", L"%d abonnements exportés");

    AddTranslation("en", "Failed to create file", L"Failed to create file");
    AddTranslation("fr", "Failed to create file", L"Impossible de créer le fichier");

    AddTranslation("en", "%d results", L"%d results");
    AddTranslation("fr", "%d results", L"%d résultats");

    AddTranslation("en", "No results", L"No results");
    AddTranslation("fr", "No results", L"Aucun résultat");

    AddTranslation("en", "Subscribed", L"Subscribed");
    AddTranslation("fr", "Subscribed", L"Abonné");

    AddTranslation("en", "Already subscribed or failed", L"Already subscribed or failed");
    AddTranslation("fr", "Already subscribed or failed", L"Déjà abonné ou échec");

    AddTranslation("en", "Select a podcast first", L"Select a podcast first");
    AddTranslation("fr", "Select a podcast first", L"Sélectionnez d'abord un podcast");

    AddTranslation("en", "Fetching feed", L"Fetching feed");
    AddTranslation("fr", "Fetching feed", L"Récupération du flux");

    AddTranslation("en", "Unknown Podcast", L"Unknown Podcast");
    AddTranslation("fr", "Unknown Podcast", L"Podcast inconnu");

    AddTranslation("en", "Podcast added", L"Podcast added");
    AddTranslation("fr", "Podcast added", L"Podcast ajouté");

    AddTranslation("en", "Failed to fetch feed", L"Failed to fetch feed");
    AddTranslation("fr", "Failed to fetch feed", L"Échec de la récupération du flux");

    AddTranslation("en", "Podcast Fetch Failed", L"Podcast Fetch Failed");
    AddTranslation("fr", "Podcast Fetch Failed", L"Échec de la récupération du podcast");

    AddTranslation("en", "No feeds found in file", L"No feeds found in file");
    AddTranslation("fr", "No feeds found in file", L"Aucun flux trouvé dans le fichier");

    AddTranslation("en", "Importing feeds", L"Importing feeds");
    AddTranslation("fr", "Importing feeds", L"Importation des flux");

    AddTranslation("en", "Imported %d feeds", L"Imported %d feeds");
    AddTranslation("fr", "Imported %d feeds", L"%d flux importés");

    AddTranslation("en", "Imported %d feeds, %d skipped", L"Imported %d feeds, %d skipped");
    AddTranslation("fr", "Imported %d feeds, %d skipped", L"%d flux importés, %d ignorés");

    // --------------------------------------------------------------
    // youtube.cpp
    // --------------------------------------------------------------
    AddTranslation("en", "Playlist loaded", L"Playlist loaded");
    AddTranslation("fr", "Playlist loaded", L"Liste de lecture chargée");

    AddTranslation("en", "Loading video", L"Loading video");
    AddTranslation("fr", "Loading video", L"Chargement de la vidéo");

    AddTranslation("en", "Playing video", L"Playing video");
    AddTranslation("fr", "Playing video", L"Lecture de la vidéo");

    AddTranslation("en", "Failed to get stream URL", L"Failed to get stream URL");
    AddTranslation("fr", "Failed to get stream URL", L"Impossible d'obtenir l'URL du flux");

    AddTranslation("en", "No results or search failed", L"No results or search failed");
    AddTranslation("fr", "No results or search failed", L"Aucun résultat ou échec de la recherche");

    AddTranslation("en", "Loading more", L"Loading more");
    AddTranslation("fr", "Loading more", L"Chargement de plus de résultats");

    AddTranslation("en", "%d more loaded", L"%d more loaded");
    AddTranslation("fr", "%d more loaded", L"%d résultats supplémentaires chargés");

    AddTranslation("en", "Loading", L"Loading");
    AddTranslation("fr", "Loading", L"Chargement");

    // Generic YouTube extractor messages (impl detail = yt-dlp, but user
    // never sees the tool name — autonomy rule).
    AddTranslation("en", "The YouTube extractor was not found. Please reinstall MediaAccess.",
                   L"The YouTube extractor was not found. Please reinstall MediaAccess.");
    AddTranslation("fr", "The YouTube extractor was not found. Please reinstall MediaAccess.",
                   L"L'extracteur YouTube est introuvable. Veuillez réinstaller MediaAccess.");

    AddTranslation("en", "The YouTube extractor was found but failed to run. Please reinstall MediaAccess.",
                   L"The YouTube extractor was found but failed to run. Please reinstall MediaAccess.");
    AddTranslation("fr", "The YouTube extractor was found but failed to run. Please reinstall MediaAccess.",
                   L"L'extracteur YouTube est présent mais n'a pas pu démarrer. Veuillez réinstaller MediaAccess.");

    AddTranslation("en", "The YouTube extractor is working.\n\n",
                   L"The YouTube extractor is working.\n\n");
    AddTranslation("fr", "The YouTube extractor is working.\n\n",
                   L"L'extracteur YouTube fonctionne.\n\n");

    AddTranslation("en", "Version: ", L"Version: ");
    AddTranslation("fr", "Version: ", L"Version : ");

    AddTranslation("en", "Video engine: ", L"Video engine: ");
    AddTranslation("fr", "Video engine: ", L"Moteur vidéo : ");

    AddTranslation("en", "available", L"available");
    AddTranslation("fr", "available", L"disponible");

    AddTranslation("en", "not available", L"not available");
    AddTranslation("fr", "not available", L"indisponible");

    AddTranslation("en", "If a YouTube video still fails, check the log file at:\n",
                   L"If a YouTube video still fails, check the log file at:\n");
    AddTranslation("fr", "If a YouTube video still fails, check the log file at:\n",
                   L"Si une vidéo YouTube échoue encore, consultez le fichier journal :\n");

    AddTranslation("en", "YouTube", L"YouTube");
    AddTranslation("fr", "YouTube", L"YouTube");

    AddTranslation("en", "Playing via video engine", L"Playing via video engine");
    AddTranslation("fr", "Playing via video engine", L"Lecture via le moteur vidéo");

    // YouTubePlayById (new in v1.0.7)
    AddTranslation("en", "Downloading audio", L"Downloading audio");
    AddTranslation("fr", "Downloading audio", L"Téléchargement de l'audio");

    AddTranslation("en", "Failed to play this video", L"Failed to play this video");
    AddTranslation("fr", "Failed to play this video", L"Impossible de lire cette vidéo");

    // YouTube cache + download button (v1.0.8)
    AddTranslation("en", "Playing from cache", L"Playing from cache");
    AddTranslation("fr", "Playing from cache", L"Lecture depuis le cache");

    AddTranslation("en", "Downloading", L"Downloading");
    AddTranslation("fr", "Downloading", L"Téléchargement");

    AddTranslation("en", "Downloaded: ", L"Downloaded: ");
    AddTranslation("fr", "Downloaded: ", L"Téléchargé : ");

    AddTranslation("en", "Download failed", L"Download failed");
    AddTranslation("fr", "Download failed", L"Échec du téléchargement");

    AddTranslation("en", "Cannot download this item", L"Cannot download this item");
    AddTranslation("fr", "Cannot download this item", L"Impossible de télécharger cet élément");

    // YouTube dialog button + Help menu cache item
    AddTranslation("en", "&Download", L"&Download");
    AddTranslation("fr", "&Download", L"&Télécharger");

    AddTranslation("en", "C&lear YouTube cache...", L"C&lear YouTube cache...");
    AddTranslation("fr", "C&lear YouTube cache...", L"Vider le cache &YouTube...");

    AddTranslation("en", "Clear YouTube cache", L"Clear YouTube cache");
    AddTranslation("fr", "Clear YouTube cache", L"Vider le cache YouTube");

    AddTranslation("en", "Clear all cached YouTube audio?", L"Clear all cached YouTube audio?");
    AddTranslation("fr", "Clear all cached YouTube audio?", L"Vider tout l'audio YouTube en cache ?");

    AddTranslation("en", "Removed %d cached files.", L"Removed %d cached files.");
    AddTranslation("fr", "Removed %d cached files.", L"%d fichiers en cache supprimés.");

    // Layout audit tool (v1.0.16)
    AddTranslation("en", "Audit dialog &layout...", L"Audit dialog &layout...");
    AddTranslation("fr", "Audit dialog &layout...", L"Audit de &mise en page...");

    AddTranslation("en", "Layout audit", L"Layout audit");
    AddTranslation("fr", "Layout audit", L"Audit de mise en page");

    AddTranslation("en", "No truncated controls found.", L"No truncated controls found.");
    AddTranslation("fr", "No truncated controls found.", L"Aucun contrôle tronqué détecté.");

    AddTranslation("en", "Truncated controls:", L"Truncated controls:");
    AddTranslation("fr", "Truncated controls:", L"Contrôles tronqués :");

    AddTranslation("en", "Report saved to:", L"Report saved to:");
    AddTranslation("fr", "Report saved to:", L"Rapport enregistré dans :");

    // Hybrid playback (v1.0.9) — stream now via mpv, swap to BASS when download lands
    AddTranslation("en", "Streaming, effects will activate shortly",
                         L"Streaming, effects will activate shortly");
    AddTranslation("fr", "Streaming, effects will activate shortly",
                         L"Lecture en streaming, les effets s'activeront sous peu");

    AddTranslation("en", "Effects activated", L"Effects activated");
    AddTranslation("fr", "Effects activated", L"Effets activés");

    // -------------------------------------------------------------------
    // Effect-toggle announcements (Ctrl+1..0 / Ctrl+- / Ctrl+=)
    // Used by effects.cpp ToggleStreamEffect / ToggleDSPEffect via Ts().
    // -------------------------------------------------------------------
    AddTranslation("en", "Volume", L"Volume");
    AddTranslation("fr", "Volume", L"Volume");

    AddTranslation("en", "Pitch", L"Pitch");
    AddTranslation("fr", "Pitch", L"Tonalité");

    AddTranslation("en", "Tempo", L"Tempo");
    AddTranslation("fr", "Tempo", L"Tempo");

    AddTranslation("en", "Rate", L"Rate");
    AddTranslation("fr", "Rate", L"Vitesse");

    AddTranslation("en", "Reverb", L"Reverb");
    AddTranslation("fr", "Reverb", L"Réverbération");

    AddTranslation("en", "Echo", L"Echo");
    AddTranslation("fr", "Echo", L"Écho");

    AddTranslation("en", "EQ", L"EQ");
    AddTranslation("fr", "EQ", L"Égaliseur");

    AddTranslation("en", "Compressor", L"Compressor");
    AddTranslation("fr", "Compressor", L"Compresseur");

    AddTranslation("en", "Stereo Width", L"Stereo Width");
    AddTranslation("fr", "Stereo Width", L"Largeur stéréo");

    AddTranslation("en", "Center Cancel", L"Center Cancel");
    AddTranslation("fr", "Center Cancel", L"Annulation centrale");

    AddTranslation("en", "Convolution", L"Convolution");
    AddTranslation("fr", "Convolution", L"Convolution");

    AddTranslation("en", "3D Audio", L"3D Audio");
    AddTranslation("fr", "3D Audio", L"Audio 3D");

    AddTranslation("en", "enabled", L"enabled");
    AddTranslation("fr", "enabled", L"activé");

    AddTranslation("en", "disabled", L"disabled");
    AddTranslation("fr", "disabled", L"désactivé");

    // Reverb algorithm names (Ctrl+5 cycles)
    AddTranslation("en", "Off", L"Off");
    AddTranslation("fr", "Off", L"Désactivé");

    AddTranslation("en", "Freeverb", L"Freeverb");
    AddTranslation("fr", "Freeverb", L"Freeverb");

    AddTranslation("en", "DX8 Reverb", L"DX8 Reverb");
    AddTranslation("fr", "DX8 Reverb", L"Réverbération DX8");

    AddTranslation("en", "I3DL2 Reverb", L"I3DL2 Reverb");
    AddTranslation("fr", "I3DL2 Reverb", L"Réverbération I3DL2");

    // -------------------------------------------------------------------
    // Effect parameter names — spoken by AnnounceCurrentParam() when the
    // user cycles with [ / ] or adjusts with Up / Down / Backspace /
    // Ctrl+Home / Ctrl+End. Volume / Pitch / Tempo / Rate / Stereo Width /
    // Center Cancel already registered above.
    // -------------------------------------------------------------------
    AddTranslation("en", "Reverb Mix",   L"Reverb Mix");
    AddTranslation("fr", "Reverb Mix",   L"Mixage de réverbération");

    AddTranslation("en", "Reverb Room",  L"Reverb Room");
    AddTranslation("fr", "Reverb Room",  L"Taille de la pièce");

    AddTranslation("en", "Reverb Damp",  L"Reverb Damp");
    AddTranslation("fr", "Reverb Damp",  L"Amortissement de la réverbération");

    AddTranslation("en", "DX8 Reverb Time", L"DX8 Reverb Time");
    AddTranslation("fr", "DX8 Reverb Time", L"DX8 Temps de réverbération");

    AddTranslation("en", "DX8 HF Ratio",    L"DX8 HF Ratio");
    AddTranslation("fr", "DX8 HF Ratio",    L"DX8 Ratio hautes fréquences");

    AddTranslation("en", "DX8 Reverb Mix",  L"DX8 Reverb Mix");
    AddTranslation("fr", "DX8 Reverb Mix",  L"DX8 Mixage de réverbération");

    AddTranslation("en", "I3DL2 Room",      L"I3DL2 Room");
    AddTranslation("fr", "I3DL2 Room",      L"I3DL2 Pièce");

    AddTranslation("en", "I3DL2 Decay",     L"I3DL2 Decay");
    AddTranslation("fr", "I3DL2 Decay",     L"I3DL2 Déclin");

    AddTranslation("en", "I3DL2 Diffusion", L"I3DL2 Diffusion");
    AddTranslation("fr", "I3DL2 Diffusion", L"I3DL2 Diffusion");

    AddTranslation("en", "I3DL2 Density",   L"I3DL2 Density");
    AddTranslation("fr", "I3DL2 Density",   L"I3DL2 Densité");

    AddTranslation("en", "Echo Delay",      L"Echo Delay");
    AddTranslation("fr", "Echo Delay",      L"Délai d'écho");

    AddTranslation("en", "Echo Feedback",   L"Echo Feedback");
    AddTranslation("fr", "Echo Feedback",   L"Retour d'écho");

    AddTranslation("en", "Echo Mix",        L"Echo Mix");
    AddTranslation("fr", "Echo Mix",        L"Mixage d'écho");

    AddTranslation("en", "EQ Preamp", L"EQ Preamp");
    AddTranslation("fr", "EQ Preamp", L"Préampli égaliseur");

    AddTranslation("en", "EQ Bass",   L"EQ Bass");
    AddTranslation("fr", "EQ Bass",   L"Égaliseur basses");

    AddTranslation("en", "EQ Mid",    L"EQ Mid");
    AddTranslation("fr", "EQ Mid",    L"Égaliseur médiums");

    AddTranslation("en", "EQ Treble", L"EQ Treble");
    AddTranslation("fr", "EQ Treble", L"Égaliseur aigus");

    AddTranslation("en", "Comp Threshold", L"Comp Threshold");
    AddTranslation("fr", "Comp Threshold", L"Compresseur seuil");

    AddTranslation("en", "Comp Ratio",     L"Comp Ratio");
    AddTranslation("fr", "Comp Ratio",     L"Compresseur ratio");

    AddTranslation("en", "Comp Attack",    L"Comp Attack");
    AddTranslation("fr", "Comp Attack",    L"Compresseur attaque");

    AddTranslation("en", "Comp Release",   L"Comp Release");
    AddTranslation("fr", "Comp Release",   L"Compresseur relâchement");

    AddTranslation("en", "Comp Gain",      L"Comp Gain");
    AddTranslation("fr", "Comp Gain",      L"Compresseur gain");

    AddTranslation("en", "Conv Mix",  L"Conv Mix");
    AddTranslation("fr", "Conv Mix",  L"Mixage de convolution");

    AddTranslation("en", "Conv Gain", L"Conv Gain");
    AddTranslation("fr", "Conv Gain", L"Gain de convolution");

    AddTranslation("en", "3D Blend",        L"3D Blend");
    AddTranslation("fr", "3D Blend",        L"Mélange 3D");

    AddTranslation("en", "3D Width",        L"3D Width");
    AddTranslation("fr", "3D Width",        L"Largeur 3D");

    AddTranslation("en", "3D Rotation",     L"3D Rotation");
    AddTranslation("fr", "3D Rotation",     L"Rotation 3D");

    AddTranslation("en", "3D Mode",         L"3D Mode");
    AddTranslation("fr", "3D Mode",         L"Mode 3D");

    AddTranslation("en", "3D Rear Speaker", L"3D Rear Speaker");
    AddTranslation("fr", "3D Rear Speaker", L"Haut-parleur arrière 3D");

    AddTranslation("en", "3D Listener X",   L"3D Listener X");
    AddTranslation("fr", "3D Listener X",   L"Position auditeur X");

    AddTranslation("en", "3D Listener Y",   L"3D Listener Y");
    AddTranslation("fr", "3D Listener Y",   L"Position auditeur Y");

    AddTranslation("en", "3D Listener Z",   L"3D Listener Z");
    AddTranslation("fr", "3D Listener Z",   L"Position auditeur Z");

    // 3D Mode / Rear Speaker value words
    AddTranslation("en", "5.1 Surround", L"5.1 Surround");
    AddTranslation("fr", "5.1 Surround", L"Surround 5.1");

    AddTranslation("en", "Binaural", L"Binaural");
    AddTranslation("fr", "Binaural", L"Binaural");

    AddTranslation("en", "On", L"On");
    AddTranslation("fr", "On", L"Activé");

    // Unit suffix (leading space preserved — the announce code appends it
    // straight after the number). "%", "x", ":1", "ms", "dB", "Hz", "mB",
    // "s" are universal abbreviations and don't need a French form (Ts()
    // falls back to the source string).
    AddTranslation("en", " semitones", L" semitones");
    AddTranslation("fr", " semitones", L" demi-tons");

    AddTranslation("en", " deg", L" deg");
    AddTranslation("fr", " deg", L" degrés");

    // Status messages already wrapped at the call site
    AddTranslation("en", "No parameters available", L"No parameters available");
    AddTranslation("fr", "No parameters available", L"Aucun paramètre disponible");

    AddTranslation("en", "Not available for live streams", L"Not available for live streams");
    AddTranslation("fr", "Not available for live streams", L"Indisponible pour les flux en direct");

    // -------------------------------------------------------------------
    // Download manager announcements (download_manager.cpp:306-313)
    // -------------------------------------------------------------------
    AddTranslation("en", "Download complete", L"Download complete");
    AddTranslation("fr", "Download complete", L"Téléchargement terminé");

    AddTranslation("en", "downloads complete", L"downloads complete");
    AddTranslation("fr", "downloads complete", L"téléchargements terminés");

    AddTranslation("en", "complete", L"complete");
    AddTranslation("fr", "complete", L"terminés");

    AddTranslation("en", "failed", L"failed");
    AddTranslation("fr", "failed", L"échoués");

    // -------------------------------------------------------------------
    // Effect preset apply / delete announcements (main.cpp)
    // -------------------------------------------------------------------
    AddTranslation("en", "Loaded preset ", L"Loaded preset ");
    AddTranslation("fr", "Loaded preset ", L"Préréglage chargé : ");

    AddTranslation("en", "Deleted preset ", L"Deleted preset ");
    AddTranslation("fr", "Deleted preset ", L"Préréglage supprimé : ");

    // -------------------------------------------------------------------
    // Status-bar bitrate text (ui.cpp UpdateStatusBar)
    // -------------------------------------------------------------------
    AddTranslation("en", "%d kbps",       L"%d kbps");
    AddTranslation("fr", "%d kbps",       L"%d kbps");

    AddTranslation("en", "~%d kbps VBR",  L"~%d kbps VBR");
    AddTranslation("fr", "~%d kbps VBR",  L"~%d kbps VBR");

    // -------------------------------------------------------------------
    // Layout-audit error (ui_options.cpp)
    // -------------------------------------------------------------------
    AddTranslation("en", "Could not create Options dialog for audit.",
                   L"Could not create Options dialog for audit.");
    AddTranslation("fr", "Could not create Options dialog for audit.",
                   L"Impossible de créer la fenêtre Options pour l'audit.");

    // -------------------------------------------------------------------
    // Help > Set as default media player...  (v1.46)
    // -------------------------------------------------------------------
    AddTranslation("en", "Set as &default media player...",
                   L"Set as &default media player...");
    AddTranslation("fr", "Set as &default media player...",
                   L"Définir comme lecteur par &défaut...");

    AddTranslation("en", "Opening Windows default-apps settings",
                   L"Opening Windows default-apps settings");
    AddTranslation("fr", "Opening Windows default-apps settings",
                   L"Ouverture des paramètres d'applications par défaut de Windows");

    AddTranslation("en", "Set as default media player",
                   L"Set as default media player");
    AddTranslation("fr", "Set as default media player",
                   L"Définir comme lecteur par défaut");

    AddTranslation("en",
        "Windows does not allow any application to set itself as the default "
        "automatically — this is a security restriction Microsoft added in "
        "Windows 8.\n\n"
        "When you click OK, Windows will open the Default apps page on the "
        "MediaAccess entry. From there, click each file type (.mp3, .mp4, "
        ".mkv, .flac, .mid, etc.) and choose MediaAccess to make it the "
        "default. On Windows 11 you can also use the \"Set default\" button "
        "near the top of the MediaAccess page to assign all supported types "
        "at once.",
        L"Windows does not allow any application to set itself as the default "
        L"automatically — this is a security restriction Microsoft added in "
        L"Windows 8.\n\n"
        L"When you click OK, Windows will open the Default apps page on the "
        L"MediaAccess entry. From there, click each file type (.mp3, .mp4, "
        L".mkv, .flac, .mid, etc.) and choose MediaAccess to make it the "
        L"default. On Windows 11 you can also use the \"Set default\" button "
        L"near the top of the MediaAccess page to assign all supported types "
        L"at once.");
    AddTranslation("fr",
        "Windows does not allow any application to set itself as the default "
        "automatically — this is a security restriction Microsoft added in "
        "Windows 8.\n\n"
        "When you click OK, Windows will open the Default apps page on the "
        "MediaAccess entry. From there, click each file type (.mp3, .mp4, "
        ".mkv, .flac, .mid, etc.) and choose MediaAccess to make it the "
        "default. On Windows 11 you can also use the \"Set default\" button "
        "near the top of the MediaAccess page to assign all supported types "
        "at once.",
        L"Windows n'autorise aucune application à se définir elle-même comme "
        L"lecteur par défaut automatiquement — c'est une restriction de "
        L"sécurité ajoutée par Microsoft depuis Windows 8.\n\n"
        L"Quand vous cliquerez sur OK, Windows ouvrira la page des "
        L"applications par défaut directement sur la fiche MediaAccess. "
        L"De là, cliquez sur chaque type de fichier (.mp3, .mp4, .mkv, "
        L".flac, .mid, etc.) et choisissez MediaAccess pour le rendre "
        L"par défaut. Sur Windows 11, vous pouvez aussi utiliser le "
        L"bouton « Définir par défaut » en haut de la page MediaAccess "
        L"pour assigner d'un coup tous les formats pris en charge.");

    // -------------------------------------------------------------------
    // DAISY / EPUB reader (v1.49 Phase 1)
    // -------------------------------------------------------------------
    AddTranslation("en", "Open &book...", L"Open &book...");
    AddTranslation("fr", "Open &book...", L"Ouvrir un &livre...");

    AddTranslation("en", "&Book library...", L"&Book library...");
    AddTranslation("fr", "&Book library...", L"&Bibliothèque de livres...");

    AddTranslation("en", "Book library", L"Book library");
    AddTranslation("fr", "Book library", L"Bibliothèque de livres");

    AddTranslation("en", "Books in your library:", L"Books in your library:");
    AddTranslation("fr", "Books in your library:", L"Livres dans votre bibliothèque :");

    AddTranslation("en", "&Open",   L"&Open");
    AddTranslation("fr", "&Open",   L"&Ouvrir");

    AddTranslation("en", "&Remove", L"&Remove");
    AddTranslation("fr", "&Remove", L"&Retirer");

    AddTranslation("en", "Re&scan", L"Re&scan");
    AddTranslation("fr", "Re&scan", L"Re&scanner");

    AddTranslation("en", "in progress", L"in progress");
    AddTranslation("fr", "in progress", L"en cours");

    AddTranslation("en", "Add bookmark", L"Add bookmark");
    AddTranslation("fr", "Add bookmark", L"Ajouter un signet");

    AddTranslation("en", "Optional note for this bookmark:",
                   L"Optional note for this bookmark:");
    AddTranslation("fr", "Optional note for this bookmark:",
                   L"Note optionnelle pour ce signet :");

    AddTranslation("en", "Go to page", L"Go to page");
    AddTranslation("fr", "Go to page", L"Aller à la page");

    AddTranslation("en", "Page number:", L"Page number:");
    AddTranslation("fr", "Page number:", L"Numéro de page :");

    AddTranslation("en", "Page not found:", L"Page not found:");
    AddTranslation("fr", "Page not found:", L"Page introuvable :");

    AddTranslation("en", "Could not open book.", L"Could not open book.");
    AddTranslation("fr", "Could not open book.", L"Impossible d'ouvrir le livre.");

    AddTranslation("en", "Could not register book in library.",
                   L"Could not register book in library.");
    AddTranslation("fr", "Could not register book in library.",
                   L"Impossible d'enregistrer le livre dans la bibliothèque.");

    AddTranslation("en", "Could not start book playback.",
                   L"Could not start book playback.");
    AddTranslation("fr", "Could not start book playback.",
                   L"Impossible de démarrer la lecture du livre.");

    AddTranslation("en", "Book opened", L"Book opened");
    AddTranslation("fr", "Book opened", L"Livre ouvert");

    AddTranslation("en", "Bookmark added", L"Bookmark added");
    AddTranslation("fr", "Bookmark added", L"Signet ajouté");

    AddTranslation("en", "End of book", L"End of book");
    AddTranslation("fr", "End of book", L"Fin du livre");

    AddTranslation("en", "Beginning of book", L"Beginning of book");
    AddTranslation("fr", "Beginning of book", L"Début du livre");

    AddTranslation("en", "Navigation level", L"Navigation level");
    AddTranslation("fr", "Navigation level", L"Niveau de navigation");

    AddTranslation("en", "Heading level 1", L"Heading level 1");
    AddTranslation("fr", "Heading level 1", L"Titre de niveau 1");
    AddTranslation("en", "Heading level 2", L"Heading level 2");
    AddTranslation("fr", "Heading level 2", L"Titre de niveau 2");
    AddTranslation("en", "Heading level 3", L"Heading level 3");
    AddTranslation("fr", "Heading level 3", L"Titre de niveau 3");
    AddTranslation("en", "Heading level 4", L"Heading level 4");
    AddTranslation("fr", "Heading level 4", L"Titre de niveau 4");
    AddTranslation("en", "Heading level 5", L"Heading level 5");
    AddTranslation("fr", "Heading level 5", L"Titre de niveau 5");
    AddTranslation("en", "Heading level 6", L"Heading level 6");
    AddTranslation("fr", "Heading level 6", L"Titre de niveau 6");

    AddTranslation("en", "Page", L"Page");
    AddTranslation("fr", "Page", L"Page");

    AddTranslation("en", "Phrase", L"Phrase");
    AddTranslation("fr", "Phrase", L"Phrase");

    AddTranslation("en", "item",  L"item");
    AddTranslation("fr", "item",  L"élément");
    AddTranslation("en", "items", L"items");
    AddTranslation("fr", "items", L"éléments");

    // Books preferences tab labels and tab title
    AddTranslation("en", "Books", L"Books");
    AddTranslation("fr", "Books", L"Livres");

    AddTranslation("en", "Library folders (scanned for DAISY and EPUB books):",
                   L"Library folders (scanned for DAISY and EPUB books):");
    AddTranslation("fr", "Library folders (scanned for DAISY and EPUB books):",
                   L"Dossiers de la bibliothèque (scannés pour livres DAISY et EPUB) :");

    AddTranslation("en", "&Add folder...",    L"&Add folder...");
    AddTranslation("fr", "&Add folder...",    L"&Ajouter un dossier...");

    AddTranslation("en", "&Remove folder",    L"&Remove folder");
    AddTranslation("fr", "&Remove folder",    L"&Retirer le dossier");

    AddTranslation("en", "Re&scan now",       L"Re&scan now");
    AddTranslation("fr", "Re&scan now",       L"Re&scanner maintenant");

    AddTranslation("en", "Select a folder containing DAISY or EPUB books",
                   L"Select a folder containing DAISY or EPUB books");
    AddTranslation("fr", "Select a folder containing DAISY or EPUB books",
                   L"Sélectionnez un dossier contenant des livres DAISY ou EPUB");

    AddTranslation("en", "Remove this book from the library? The file on disk is kept.",
                   L"Remove this book from the library? The file on disk is kept.");
    AddTranslation("fr", "Remove this book from the library? The file on disk is kept.",
                   L"Retirer ce livre de la bibliothèque ? Le fichier sur le disque est conservé.");

    AddTranslation("en", "Scan complete: %d book(s) added or updated.",
                   L"Scan complete: %d book(s) added or updated.");
    AddTranslation("fr", "Scan complete: %d book(s) added or updated.",
                   L"Scan terminé : %d livre(s) ajouté(s) ou mis à jour.");

    AddTranslation("en",
        "This book has no recorded audio and no synthesized speech support yet. "
        "It has been added to your library — full text-to-speech playback "
        "for EPUB and text-only DAISY books will arrive in a future update.",
        L"This book has no recorded audio and no synthesized speech support yet. "
        L"It has been added to your library — full text-to-speech playback "
        L"for EPUB and text-only DAISY books will arrive in a future update.");
    AddTranslation("fr",
        "This book has no recorded audio and no synthesized speech support yet. "
        "It has been added to your library — full text-to-speech playback "
        "for EPUB and text-only DAISY books will arrive in a future update.",
        L"Ce livre n'a pas d'audio enregistré et la synthèse vocale n'est pas "
        L"encore prise en charge. Il a été ajouté à votre bibliothèque — la "
        L"lecture vocale complète des livres EPUB et DAISY texte-seul "
        L"arrivera dans une prochaine mise à jour.");

    // ----- v1.50 Phase 2 — TTS, text window, search -----
    AddTranslation("en", "Book text", L"Book text");
    AddTranslation("fr", "Book text", L"Texte du livre");

    AddTranslation("en", "&Text-to-speech voice (for text-only books):",
                   L"&Text-to-speech voice (for text-only books):");
    AddTranslation("fr", "&Text-to-speech voice (for text-only books):",
                   L"&Voix de synthèse vocale (livres texte seul) :");

    AddTranslation("en", "(Windows default voice)", L"(Windows default voice)");
    AddTranslation("fr", "(Windows default voice)", L"(Voix Windows par défaut)");

    AddTranslation("en", "Text &window theme:", L"Text &window theme:");
    AddTranslation("fr", "Text &window theme:", L"Thème de la fenêtre de te&xte :");

    AddTranslation("en", "Standard",      L"Standard");
    AddTranslation("fr", "Standard",      L"Standard");
    AddTranslation("en", "High contrast", L"High contrast");
    AddTranslation("fr", "High contrast", L"Fort contraste");
    AddTranslation("en", "Large",         L"Large");
    AddTranslation("fr", "Large",         L"Agrandi");

    AddTranslation("en", "Al&ways hide text window (audio-only experience)",
                   L"Al&ways hide text window (audio-only experience)");
    AddTranslation("fr", "Al&ways hide text window (audio-only experience)",
                   L"Tou&jours cacher la fenêtre texte (expérience audio uniquement)");

    AddTranslation("en", "Show / hide text window", L"Show / hide text window");
    AddTranslation("fr", "Show / hide text window", L"Afficher / cacher la fenêtre texte");

    AddTranslation("en", "Find in book", L"Find in book");
    AddTranslation("fr", "Find in book", L"Rechercher dans le livre");

    AddTranslation("en", "Find:", L"Find:");
    AddTranslation("fr", "Find:", L"Rechercher :");

    AddTranslation("en", "Find &next", L"Find &next");
    AddTranslation("fr", "Find &next", L"&Suivant");

    AddTranslation("en", "Search in book...", L"Search in book...");
    AddTranslation("fr", "Search in book...", L"Rechercher dans le livre...");

    AddTranslation("en", "No matches found for:", L"No matches found for:");
    AddTranslation("fr", "No matches found for:", L"Aucun résultat pour :");

    AddTranslation("en", "Found", L"Found");
    AddTranslation("fr", "Found", L"Trouvé");

    AddTranslation("en", "This book has no extractable text — search is not available.",
                   L"This book has no extractable text — search is not available.");
    AddTranslation("fr", "This book has no extractable text — search is not available.",
                   L"Ce livre n'a pas de texte extractible — la recherche n'est pas disponible.");

    AddTranslation("en", "This book has no recorded audio and no extractable text. "
                         "It has been added to your library but cannot be played.",
                   L"This book has no recorded audio and no extractable text. "
                   L"It has been added to your library but cannot be played.");
    AddTranslation("fr", "This book has no recorded audio and no extractable text. "
                         "It has been added to your library but cannot be played.",
                   L"Ce livre n'a pas d'audio enregistré ni de texte extractible. "
                   L"Il a été ajouté à votre bibliothèque mais ne peut pas être lu.");
}
