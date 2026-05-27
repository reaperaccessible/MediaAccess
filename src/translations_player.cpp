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

    AddTranslation("en", "Failed to initialize BASS audio library.", L"Failed to initialize BASS audio library.");
    AddTranslation("fr", "Failed to initialize BASS audio library.", L"Impossible d'initialiser la bibliothèque audio BASS.");

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

    AddTranslation("en", "Unsupported stream format. Check bass_aac.dll is in lib folder.", L"Unsupported stream format. Check bass_aac.dll is in lib folder.");
    AddTranslation("fr", "Unsupported stream format. Check bass_aac.dll is in lib folder.", L"Format de flux non pris en charge. Vérifiez que bass_aac.dll est dans le dossier lib.");

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

    // Help menu — Manual (F1) opens the bilingual HTML manual
    AddTranslation("en", "&Manual\tF1", L"&Manual\tF1");
    AddTranslation("fr", "&Manual\tF1", L"&Manuel\tF1");
    AddTranslation("en", "Manual", L"Manual");
    AddTranslation("fr", "Manual", L"Manuel");
    AddTranslation("en", "Could not open the manual. Make sure the docs folder is present alongside MediaAccess.exe.",
                   L"Could not open the manual. Make sure the docs folder is present alongside MediaAccess.exe.");
    AddTranslation("fr", "Could not open the manual. Make sure the docs folder is present alongside MediaAccess.exe.",
                   L"Impossible d'ouvrir le manuel. Vérifiez que le dossier docs est présent à côté de MediaAccess.exe.");

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

    AddTranslation("en", "Failed to start recording (error %d)", L"Failed to start recording (error %d)");
    AddTranslation("fr", "Failed to start recording (error %d)", L"Impossible de démarrer l'enregistrement (erreur %d)");

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
}
