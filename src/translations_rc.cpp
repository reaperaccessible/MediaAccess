// translations_rc.cpp -- French translations for every string baked into
// MediaAccess.rc (menus + dialog controls).
//
// The English source text (exactly as it appears in MediaAccess.rc) doubles
// as the lookup key. LocalizeDialog() / LocalizeMenu() walk the live HMENU /
// HWND tree and swap each English string for the active-language string
// returned by T().
//
// Accelerator hygiene: a menu's "&" hotkey letter must be unique within
// that menu (Windows ignores duplicates). When adding or editing a
// translation, scan the surrounding entries in the same MENU block and
// pick an "&" letter that isn't already used. Mismatched accelerators
// (e.g. EN "&File" → FR "&Fichier", both using 'F') are fine because each
// language renders only its own bindings.
//
// Tab-width abbreviations: some entries here intentionally use shorter
// phrasings than translations_ui.cpp because the .rc dialog control they
// drive has fixed pixel width. RegisterRcTranslations() runs LAST in
// InitTranslations() (see translations.cpp) so these abbreviated forms
// override any longer duplicates registered earlier.

#include "translations.h"

void RegisterRcTranslations() {
    // ===================================================================
    // MENU: &File
    // ===================================================================
    AddTranslation("en", "&File", L"&File");
    AddTranslation("fr", "&File", L"&Fichier");

    AddTranslation("en", "&Open...\tCtrl+O", L"&Open...\tCtrl+O");
    AddTranslation("fr", "&Open...\tCtrl+O", L"&Ouvrir...\tCtrl+O");

    AddTranslation("en", "Add &Folder...\tCtrl+Shift+O", L"Add &Folder...\tCtrl+Shift+O");
    AddTranslation("fr", "Add &Folder...\tCtrl+Shift+O", L"Ajouter un &dossier...\tCtrl+Shift+O");

    AddTranslation("en", "&Playlist...\tCtrl+P", L"&Playlist...\tCtrl+P");
    AddTranslation("fr", "&Playlist...\tCtrl+P", L"&Liste de lecture...\tCtrl+P");

    AddTranslation("en", "Open &URL...\tCtrl+U", L"Open &URL...\tCtrl+U");
    AddTranslation("fr", "Open &URL...\tCtrl+U", L"Ouvrir une &URL...\tCtrl+U");

    AddTranslation("en", "Paste from clip&board\tCtrl+V", L"Paste from clip&board\tCtrl+V");
    AddTranslation("fr", "Paste from clip&board\tCtrl+V", L"Coller depuis le &presse-papiers\tCtrl+V");

    AddTranslation("en", "&YouTube...\tCtrl+Y", L"&YouTube...\tCtrl+Y");
    AddTranslation("fr", "&YouTube...\tCtrl+Y", L"&YouTube...\tCtrl+Y");

    AddTranslation("en", "&Radio...\tCtrl+R", L"&Radio...\tCtrl+R");
    AddTranslation("fr", "&Radio...\tCtrl+R", L"&Radio...\tCtrl+R");

    AddTranslation("en", "&Add Stream to Favorites...\tCtrl+D", L"&Add Stream to Favorites...\tCtrl+D");
    AddTranslation("fr", "&Add Stream to Favorites...\tCtrl+D", L"&Ajouter le flux aux favoris...\tCtrl+D");

    AddTranslation("en", "&Podcasts...\tCtrl+Shift+P", L"&Podcasts...\tCtrl+Shift+P");
    AddTranslation("fr", "&Podcasts...\tCtrl+Shift+P", L"&Podcasts...\tCtrl+Shift+P");

    AddTranslation("en", "&Schedule...\tCtrl+S", L"&Schedule...\tCtrl+S");
    AddTranslation("fr", "&Schedule...\tCtrl+S", L"&Planificateur...\tCtrl+S");

    AddTranslation("en", "Song &History...\tCtrl+Shift+H", L"Song &History...\tCtrl+Shift+H");
    AddTranslation("fr", "Song &History...\tCtrl+Shift+H", L"&Historique des morceaux...\tCtrl+Shift+H");

    AddTranslation("en", "Recent &Files", L"Recent &Files");
    AddTranslation("fr", "Recent &Files", L"&Fichiers récents");

    AddTranslation("en", "(Empty)", L"(Empty)");
    AddTranslation("fr", "(Empty)", L"(Vide)");

    AddTranslation("en", "&Hide to Tray\tCtrl+H", L"&Hide to Tray\tCtrl+H");
    AddTranslation("fr", "&Hide to Tray\tCtrl+H", L"Réduire dans la &barre d'état\tCtrl+H");

    AddTranslation("en", "O&ptions...\tCtrl+,", L"O&ptions...\tCtrl+,");
    AddTranslation("fr", "O&ptions...\tCtrl+,", L"O&ptions...\tCtrl+,");

    AddTranslation("en", "E&xit\tAlt+F4", L"E&xit\tAlt+F4");
    AddTranslation("fr", "E&xit\tAlt+F4", L"&Quitter\tAlt+F4");

    // ===================================================================
    // MENU: &Playback
    // ===================================================================
    AddTranslation("en", "&Playback", L"&Playback");
    AddTranslation("fr", "&Playback", L"&Lecture");

    AddTranslation("en", "&Play\tX", L"&Play\tX");
    AddTranslation("fr", "&Play\tX", L"&Lire\tX");

    AddTranslation("en", "Pa&use\tC", L"Pa&use\tC");
    AddTranslation("fr", "Pa&use\tC", L"Pa&use\tC");

    AddTranslation("en", "Play/Pause\tSpace", L"Play/Pause\tSpace");
    AddTranslation("fr", "Play/Pause\tSpace", L"Lecture/Pause\tEspace");

    AddTranslation("en", "&Stop\tV", L"&Stop\tV");
    AddTranslation("fr", "&Stop\tV", L"&Arrêter\tV");

    AddTranslation("en", "P&revious\tZ", L"P&revious\tZ");
    AddTranslation("fr", "P&revious\tZ", L"P&récédent\tZ");

    AddTranslation("en", "&Next\tB", L"&Next\tB");
    AddTranslation("fr", "&Next\tB", L"&Suivant\tB");

    AddTranslation("en", "S&huffle\tH", L"S&huffle\tH");
    AddTranslation("fr", "S&huffle\tH", L"Lecture a&léatoire\tH");

    AddTranslation("en", "R&epeat\tE", L"R&epeat\tE");
    AddTranslation("fr", "R&epeat\tE", L"R&épéter\tE");

    AddTranslation("en", "Seek &Backward\tLeft", L"Seek &Backward\tLeft");
    AddTranslation("fr", "Seek &Backward\tLeft", L"&Reculer\tGauche");

    AddTranslation("en", "Seek &Forward\tRight", L"Seek &Forward\tRight");
    AddTranslation("fr", "Seek &Forward\tRight", L"A&vancer\tDroite");

    AddTranslation("en", "&Beginning\tHome", L"&Beginning\tHome");
    AddTranslation("fr", "&Beginning\tHome", L"&Début\tHome");

    AddTranslation("en", "&Jump to Time...\tJ", L"&Jump to Time...\tJ");
    AddTranslation("fr", "&Jump to Time...\tJ", L"&Aller à un instant...\tJ");

    AddTranslation("en", "Volume &Up\tUp", L"Volume &Up\tUp");
    AddTranslation("fr", "Volume &Up\tUp", L"&Augmenter le volume\tHaut");

    AddTranslation("en", "Volume &Down\tDown", L"Volume &Down\tDown");
    AddTranslation("fr", "Volume &Down\tDown", L"&Diminuer le volume\tBas");

    AddTranslation("en", "Speak &Elapsed\tCtrl+Shift+E", L"Speak &Elapsed\tCtrl+Shift+E");
    AddTranslation("fr", "Speak &Elapsed\tCtrl+Shift+E", L"Annoncer le temps &écoulé\tCtrl+Shift+E");

    AddTranslation("en", "Speak &Remaining\tCtrl+Shift+R", L"Speak &Remaining\tCtrl+Shift+R");
    AddTranslation("fr", "Speak &Remaining\tCtrl+Shift+R", L"Annoncer le temps &restant\tCtrl+Shift+R");

    AddTranslation("en", "Speak &Total\tCtrl+Shift+T", L"Speak &Total\tCtrl+Shift+T");
    AddTranslation("fr", "Speak &Total\tCtrl+Shift+T", L"Annoncer la durée &totale\tCtrl+Shift+T");

    // ===================================================================
    // MENU: &Video
    // ===================================================================
    AddTranslation("en", "&Video", L"&Video");
    AddTranslation("fr", "&Video", L"&Vidéo");

    AddTranslation("en", "&Fullscreen\tF11", L"&Fullscreen\tF11");
    AddTranslation("fr", "&Fullscreen\tF11", L"&Plein écran\tF11");

    AddTranslation("en", "Cycle &Subtitles", L"Cycle &Subtitles");
    AddTranslation("fr", "Cycle &Subtitles", L"Changer de &sous-titres");

    AddTranslation("en", "&Load Subtitle File...", L"&Load Subtitle File...");
    AddTranslation("fr", "&Load Subtitle File...", L"&Charger un fichier de sous-titres...");

    AddTranslation("en", "Subtitles &Off", L"Subtitles &Off");
    AddTranslation("fr", "Subtitles &Off", L"Désactiver les sous-titres (&O)");

    AddTranslation("en", "Cycle &Audio Track", L"Cycle &Audio Track");
    AddTranslation("fr", "Cycle &Audio Track", L"Changer de piste &audio");

    AddTranslation("en", "Cycle Aspect &Ratio", L"Cycle Aspect &Ratio");
    AddTranslation("fr", "Cycle Aspect &Ratio", L"Changer le format d'&image");

    AddTranslation("en", "Take Scr&eenshot", L"Take Scr&eenshot");
    AddTranslation("fr", "Take Scr&eenshot", L"Prendre une &capture d'écran");

    // ===================================================================
    // MENU: &Help
    // ===================================================================
    AddTranslation("en", "&Help", L"&Help");
    AddTranslation("fr", "&Help", L"&Aide");

    AddTranslation("en", "&Readme", L"&Readme");
    AddTranslation("fr", "&Readme", L"Lis&ez-moi");

    AddTranslation("en", "Check for &Updates...", L"Check for &Updates...");
    AddTranslation("fr", "Check for &Updates...", L"Rechercher des &mises à jour...");

    AddTranslation("en", "&Audio formats...", L"&Audio formats...");
    AddTranslation("fr", "&Audio formats...", L"Formats &audio...");

    AddTranslation("en", "&Contact us...", L"&Contact us...");
    AddTranslation("fr", "&Contact us...", L"Nous &contacter...");

    AddTranslation("en", "Make a &donation...", L"Make a &donation...");
    AddTranslation("fr", "Make a &donation...", L"Faire un &don...");

    AddTranslation("en", "Contact us", L"Contact us");
    AddTranslation("fr", "Contact us", L"Nous contacter");

    AddTranslation("en", "Make a donation", L"Make a donation");
    AddTranslation("fr", "Make a donation", L"Faire un don");

    AddTranslation("en", "Could not open your email client. Please write to "
                         "reaperaccessible@gmail.com manually.",
                   L"Could not open your email client. Please write to "
                   L"reaperaccessible@gmail.com manually.");
    AddTranslation("fr", "Could not open your email client. Please write to "
                         "reaperaccessible@gmail.com manually.",
                   L"Impossible d'ouvrir votre logiciel de courriel. "
                   L"Veuillez écrire à reaperaccessible@gmail.com manuellement.");

    AddTranslation("en", "Could not open the donation page in your browser.",
                   L"Could not open the donation page in your browser.");
    AddTranslation("fr", "Could not open the donation page in your browser.",
                   L"Impossible d'ouvrir la page de don dans votre navigateur.");

    // v1.65 — Speech tab: position announcement after seek
    AddTranslation("en", "Announce &position after a seek",
                   L"Announce &position after a seek");
    AddTranslation("fr", "Announce &position after a seek",
                   L"Annoncer la &position après un déplacement");

    // v1.63 — Audio slots menu and dialog
    AddTranslation("en", "Audio &slots...", L"Audio &slots...");
    AddTranslation("fr", "Audio &slots...", L"&Slots audio...");
    AddTranslation("en", "Audio slots",     L"Audio slots");
    AddTranslation("fr", "Audio slots",     L"Slots audio");
    AddTranslation("en", "&Slot 1:",  L"&Slot 1 :");
    AddTranslation("fr", "&Slot 1:",  L"&Slot 1 :");
    AddTranslation("en", "S&lot 2:",  L"S&lot 2 :");
    AddTranslation("fr", "S&lot 2:",  L"S&lot 2 :");
    AddTranslation("en", "Sl&ot 3:",  L"Sl&ot 3 :");
    AddTranslation("fr", "Sl&ot 3:",  L"Sl&ot 3 :");
    AddTranslation("en", "Slo&t 4:",  L"Slo&t 4 :");
    AddTranslation("fr", "Slo&t 4:",  L"Slo&t 4 :");
    AddTranslation("en", "Slot &5:",  L"Slot &5 :");
    AddTranslation("fr", "Slot &5:",  L"Slot &5 :");
    AddTranslation("en", "Slot &6:",  L"Slot &6 :");
    AddTranslation("fr", "Slot &6:",  L"Slot &6 :");
    AddTranslation("en", "Slot &7:",  L"Slot &7 :");
    AddTranslation("fr", "Slot &7:",  L"Slot &7 :");
    AddTranslation("en", "Slot &8:",  L"Slot &8 :");
    AddTranslation("fr", "Slot &8:",  L"Slot &8 :");
    AddTranslation("en", "Slot &9:",  L"Slot &9 :");
    AddTranslation("fr", "Slot &9:",  L"Slot &9 :");
    AddTranslation("en", "Slot 1&0:", L"Slot 1&0 :");
    AddTranslation("fr", "Slot 1&0:", L"Slot 1&0 :");
    AddTranslation("en", "&Refresh device list", L"&Refresh device list");
    AddTranslation("fr", "&Refresh device list", L"&Actualiser la liste des périphériques");
    AddTranslation("en", "Assign keyboard shortcuts to slots from Tools > Actions.",
                   L"Assign keyboard shortcuts to slots from Tools > Actions.");
    AddTranslation("fr", "Assign keyboard shortcuts to slots from Tools > Actions.",
                   L"Assignez des raccourcis aux slots depuis Outils > Actions.");

    // v1.55 — sleep-timer menu cascade
    AddTranslation("en", "Sleep &timer", L"Sleep &timer");
    AddTranslation("fr", "Sleep &timer", L"Minuterie de &sommeil");
    AddTranslation("en", "&15 minutes", L"&15 minutes");
    AddTranslation("fr", "&15 minutes", L"&15 minutes");
    AddTranslation("en", "&30 minutes", L"&30 minutes");
    AddTranslation("fr", "&30 minutes", L"&30 minutes");
    AddTranslation("en", "&45 minutes", L"&45 minutes");
    AddTranslation("fr", "&45 minutes", L"&45 minutes");
    AddTranslation("en", "&1 hour",     L"&1 hour");
    AddTranslation("fr", "&1 hour",     L"&1 heure");
    AddTranslation("en", "1h &30",      L"1h &30");
    AddTranslation("fr", "1h &30",      L"1h &30");
    AddTranslation("en", "&2 hours",    L"&2 hours");
    AddTranslation("fr", "&2 hours",    L"&2 heures");
    AddTranslation("en", "&Custom...\tF8",            L"&Custom...\tF8");
    AddTranslation("fr", "&Custom...\tF8",            L"&Personnalisée...\tF8");
    AddTranslation("en", "C&ancel timer\tShift+F8",   L"C&ancel timer\tShift+F8");
    AddTranslation("fr", "C&ancel timer\tShift+F8",   L"&Annuler la minuterie\tMaj+F8");
    AddTranslation("en", "&Speak remaining\tCtrl+F8", L"&Speak remaining\tCtrl+F8");
    AddTranslation("fr", "&Speak remaining\tCtrl+F8", L"&Annoncer le temps restant\tCtrl+F8");

    AddTranslation("en", "A&nnounce track when MediaAccess gets focus",
                   L"A&nnounce track when MediaAccess gets focus");
    AddTranslation("fr", "A&nnounce track when MediaAccess gets focus",
                   L"A&nnoncer la piste quand MediaAccess reçoit le focus");

    // ===================================================================
    // DIALOG: IDD_OPTIONS - "Options"
    // ===================================================================
    AddTranslation("en", "Options", L"Options");
    AddTranslation("fr", "Options", L"Options");

    // Playback tab
    AddTranslation("en", "&Output device:", L"&Output device:");
    AddTranslation("fr", "&Output device:", L"Périphérique de &sortie :");

    AddTranslation("en", "&Allow volume above 100%", L"&Allow volume above 100%");
    AddTranslation("fr", "&Allow volume above 100%", L"&Autoriser le volume au-dessus de 100 %");

    AddTranslation("en", "&Remember playback state on exit", L"&Remember playback state on exit");
    AddTranslation("fr", "&Remember playback state on exit", L"Mémo&riser l'état de lecture à la fermeture");

    AddTranslation("en", "Remember p&osition if longer than:", L"Remember p&osition if longer than:");
    AddTranslation("fr", "Remember p&osition if longer than:", L"Mémoriser la p&osition si plus long que :");

    AddTranslation("en", "&Bring window to front when opening files", L"&Bring window to front when opening files");
    AddTranslation("fr", "&Bring window to front when opening files", L"&Mettre la fenêtre au premier plan à l'ouverture de fichiers");

    AddTranslation("en", "&Load all files in folder when opening single file", L"&Load all files in folder when opening single file");
    AddTranslation("fr", "&Load all files in folder when opening single file", L"&Charger tous les fichiers du dossier à l'ouverture d'un fichier");

    AddTranslation("en", "&Minimize to system tray", L"&Minimize to system tray");
    AddTranslation("fr", "&Minimize to system tray", L"Réduire dans la &barre d'état système");

    AddTranslation("en", "Show &track name in window title", L"Show &track name in window title");
    AddTranslation("fr", "Show &track name in window title", L"Afficher le nom de la pis&te dans le titre de la fenêtre");

    AddTranslation("en", "Auto-ad&vance to next playlist item", L"Auto-ad&vance to next playlist item");
    AddTranslation("fr", "Auto-ad&vance to next playlist item", L"Passer automati&quement à l'élément suivant");

    AddTranslation("en", "&Follow playback in playlist dialog", L"&Follow playback in playlist dialog");
    AddTranslation("fr", "&Follow playback in playlist dialog", L"&Suivre la lecture dans la liste de lecture");

    AddTranslation("en", "Check for &updates on startup", L"Check for &updates on startup");
    AddTranslation("fr", "Check for &updates on startup", L"Rechercher des mises à jo&ur au démarrage");

    AddTranslation("en", "Allow &multiple instances", L"Allow &multiple instances");
    AddTranslation("fr", "Allow &multiple instances", L"Autoriser les instances &multiples");

    AddTranslation("en", "Register all supported &file types", L"Register all supported &file types");
    AddTranslation("fr", "Register all supported &file types", L"Associer tous les types de &fichiers pris en charge");

    AddTranslation("en", "Re&wind on pause (ms):", L"Re&wind on pause (ms):");
    AddTranslation("fr", "Re&wind on pause (ms):", L"Re&culer à la pause (ms) :");

    AddTranslation("en", "Volu&me step:", L"Volu&me step:");
    AddTranslation("fr", "Volu&me step:", L"Pas de volu&me :");

    // Recording tab
    AddTranslation("en", "Record audio output to file. Press R to toggle recording.",
                   L"Record audio output to file. Press R to toggle recording.");
    AddTranslation("fr", "Record audio output to file. Press R to toggle recording.",
                   L"Enregistrer la sortie audio dans un fichier. Appuyez sur R pour activer l'enregistrement.");

    AddTranslation("en", "&Output folder:", L"&Output folder:");
    AddTranslation("fr", "&Output folder:", L"Dossier de s&ortie :");

    AddTranslation("en", "&Browse...", L"&Browse...");
    AddTranslation("fr", "&Browse...", L"&Parcourir...");

    AddTranslation("en", "Filename &template:", L"Filename &template:");
    AddTranslation("fr", "Filename &template:", L"&Modèle de nom de fichier :");

    AddTranslation("en", "(Uses strftime format: %Y=year, %m=month, %d=day, %H=hour, %M=min, %S=sec)",
                   L"(Available: {year}, {month}, {day}, {hour}, {minute}, {second})");
    AddTranslation("fr", "(Uses strftime format: %Y=year, %m=month, %d=day, %H=hour, %M=min, %S=sec)",
                   L"(Disponibles : {année}, {mois}, {jour}, {heure}, {minute}, {seconde})");

    AddTranslation("en", "&Format:", L"&Format:");
    AddTranslation("fr", "&Format:", L"&Format :");

    AddTranslation("en", "&Bitrate:", L"&Bitrate:");
    AddTranslation("fr", "&Bitrate:", L"&Débit :");

    AddTranslation("en", "(Bitrate only applies to MP3 and OGG formats)",
                   L"(Bitrate only applies to MP3 and OGG formats)");
    AddTranslation("fr", "(Bitrate only applies to MP3 and OGG formats)",
                   L"(Le débit ne s'applique qu'aux formats MP3 et OGG)");

    // YouTube tab — v1.71 download folder field + Browse button
    AddTranslation("en", "Do&wnload folder (empty = default):",
                   L"Do&wnload folder (empty = default):");
    AddTranslation("fr", "Do&wnload folder (empty = default):",
                   L"Dossier de télé&chargement (vide = par défaut) :");
    AddTranslation("en", "Bro&wse...",                              L"Bro&wse...");
    AddTranslation("fr", "Bro&wse...",                              L"Pa&rcourir...");

    // Downloads tab
    AddTranslation("en", "Configure podcast episode download settings.",
                   L"Configure podcast episode download settings.");
    AddTranslation("fr", "Configure podcast episode download settings.",
                   L"Configurer les paramètres de téléchargement des épisodes de podcast.");

    AddTranslation("en", "&Downloads folder:", L"&Downloads folder:");
    AddTranslation("fr", "&Downloads folder:", L"Dossier des &téléchargements :");

    AddTranslation("en", "&Organize downloads into folders by feed title",
                   L"&Organize downloads into folders by feed title");
    AddTranslation("fr", "&Organize downloads into folders by feed title",
                   L"&Organiser les téléchargements en dossiers par titre de flux");

    // Speech tab
    AddTranslation("en", "Configure speech feedback for various events.",
                   L"Configure speech feedback for various events.");
    AddTranslation("fr", "Configure speech feedback for various events.",
                   L"Configurer le retour vocal pour divers événements.");

    AddTranslation("en", "&Announce track changes", L"&Announce track changes");
    AddTranslation("fr", "&Announce track changes", L"&Annoncer les changements de piste");

    AddTranslation("en", "Speak &volume when adjusted", L"Speak &volume when adjusted");
    AddTranslation("fr", "Speak &volume when adjusted", L"Annoncer le &volume lors des ajustements");

    AddTranslation("en", "Speak &effect value when adjusted", L"Speak &effect value when adjusted");
    AddTranslation("fr", "Speak &effect value when adjusted", L"Annoncer la valeur d'&effet lors des ajustements");

    AddTranslation("en", "Announce YouTube effects activation", L"Announce YouTube effects activation");
    AddTranslation("fr", "Announce YouTube effects activation", L"Annoncer l'activation des effets YouTube");

    AddTranslation("en", "Clear cache when MediaAccess closes", L"Clear cache when MediaAccess closes");
    AddTranslation("fr", "Clear cache when MediaAccess closes", L"Vider le cache à la fermeture de MediaAccess");

    AddTranslation("en", "Clear cache now...", L"Clear cache now...");
    AddTranslation("fr", "Clear cache now...", L"Vider le cache maintenant...");

    AddTranslation("en", "Limit cache size (MB, 0 = unlimited):", L"Limit cache size (MB, 0 = unlimited):");
    AddTranslation("fr", "Limit cache size (MB, 0 = unlimited):", L"Limite du cache (Mo, 0 = illimité) :");

    // Movement tab
    AddTranslation("en", "Seek amounts (use , and . to cycle):", L"Seek amounts (use , and . to cycle):");
    AddTranslation("fr", "Seek amounts (use , and . to cycle):",
                   L"Pas de recherche (utilisez , et . pour les faire défiler) :");

    AddTranslation("en", "&1 second", L"&1 second");
    AddTranslation("fr", "&1 second", L"&1 seconde");

    AddTranslation("en", "&5 seconds", L"&5 seconds");
    AddTranslation("fr", "&5 seconds", L"&5 secondes");

    AddTranslation("en", "1&0 seconds", L"1&0 seconds");
    AddTranslation("fr", "1&0 seconds", L"1&0 secondes");

    AddTranslation("en", "&30 seconds", L"&30 seconds");
    AddTranslation("fr", "&30 seconds", L"&30 secondes");

    AddTranslation("en", "1 &minute", L"1 &minute");
    AddTranslation("fr", "1 &minute", L"1 &minute");

    AddTranslation("en", "5 m&inutes", L"5 m&inutes");
    AddTranslation("fr", "5 m&inutes", L"5 m&inutes");

    AddTranslation("en", "10 min&utes", L"10 min&utes");
    AddTranslation("fr", "10 min&utes", L"10 min&utes");

    AddTranslation("en", "3&0 minutes", L"3&0 minutes");
    AddTranslation("fr", "3&0 minutes", L"3&0 minutes");

    AddTranslation("en", "1 &hour", L"1 &hour");
    AddTranslation("fr", "1 &hour", L"1 &heure");

    AddTranslation("en", "1 &track", L"1 &track");
    AddTranslation("fr", "1 &track", L"1 pis&te");

    AddTranslation("en", "5 t&racks", L"5 t&racks");
    AddTranslation("fr", "5 t&racks", L"5 pist&es");

    AddTranslation("en", "10 trac&ks", L"10 trac&ks");
    AddTranslation("fr", "10 trac&ks", L"10 pist&es");

    AddTranslation("en", "1 c&hapter (if available)", L"1 c&hapter (if available)");
    AddTranslation("fr", "1 c&hapter (if available)", L"1 c&hapitre (si disponible)");

    AddTranslation("en", "&Add...", L"&Add...");
    AddTranslation("fr", "&Add...", L"&Ajouter...");

    AddTranslation("en", "&Edit...", L"&Edit...");
    AddTranslation("fr", "&Edit...", L"&Modifier...");

    AddTranslation("en", "&Remove", L"&Remove");
    AddTranslation("fr", "&Remove", L"&Retirer");

    // Effects tab
    AddTranslation("en", "Stream effects ([ ] to cycle, Up/Down to adjust):",
                   L"Stream effects ([ ] to cycle, Up/Down to adjust):");
    AddTranslation("fr", "Stream effects ([ ] to cycle, Up/Down to adjust):",
                   L"Effets de flux ([ ] pour les faire défiler, Haut/Bas pour ajuster) :");

    AddTranslation("en", "&Volume (0-400%)", L"&Volume (0-400%)");
    AddTranslation("fr", "&Volume (0-400%)", L"&Volume (0-400 %)");

    AddTranslation("en", "&Pitch (-12 to +12 semitones)", L"&Pitch (-12 to +12 semitones)");
    AddTranslation("fr", "&Pitch (-12 to +12 semitones)", L"&Hauteur (-12 à +12 demi-tons)");

    AddTranslation("en", "&Tempo (-50% to +100%)", L"&Tempo (-50% to +100%)");
    AddTranslation("fr", "&Tempo (-50% to +100%)", L"&Tempo (-50 % à +100 %)");

    AddTranslation("en", "Playback &Rate (0.5x - 2x)", L"Playback &Rate (0.5x - 2x)");
    AddTranslation("fr", "Playback &Rate (0.5x - 2x)", L"Vitesse de lectu&re (0,5x - 2x)");

    AddTranslation("en", "Step:", L"Step:");
    AddTranslation("fr", "Step:", L"Pas :");

    AddTranslation("en", "DSP effects (enable to add their parameters to cycle list):",
                   L"DSP effects (enable to add their parameters to cycle list):");
    AddTranslation("fr", "DSP effects (enable to add their parameters to cycle list):",
                   L"Effets DSP (activez pour ajouter leurs paramètres à la liste) :");

    AddTranslation("en", "Re&verb:", L"Re&verb:");
    AddTranslation("fr", "Re&verb:", L"Ré&verbération :");

    AddTranslation("en", "&Echo", L"&Echo");
    AddTranslation("fr", "&Echo", L"&Écho");

    AddTranslation("en", "E&Q (Bass/Mid/Treble)", L"E&Q (Bass/Mid/Treble)");
    AddTranslation("fr", "E&Q (Bass/Mid/Treble)", L"E&Q (Graves/Médiums/Aigus)");

    AddTranslation("en", "&Compressor", L"&Compressor");
    AddTranslation("fr", "&Compressor", L"&Compresseur");

    AddTranslation("en", "&Stereo Width (0-200%)", L"&Stereo Width (0-200%)");
    AddTranslation("fr", "&Stereo Width (0-200%)", L"Largeur &stéréo (0-200 %)");

    AddTranslation("en", "Ce&nter Cancel (-100 to +100%)", L"Ce&nter Cancel (-100 to +100%)");
    AddTranslation("fr", "Ce&nter Cancel (-100 to +100%)", L"Suppression du ce&ntre (-100 à +100 %)");

    AddTranslation("en", "&3D Audio (HRTF/Binaural)", L"&3D Audio (HRTF/Binaural)");
    AddTranslation("fr", "&3D Audio (HRTF/Binaural)", L"Audio &3D (HRTF/binaural)");

    AddTranslation("en", "Co&nvolution Reverb", L"Co&nvolution Reverb");
    AddTranslation("fr", "Co&nvolution Reverb", L"Réverbération conv.");

    AddTranslation("en", "IR File:", L"IR File:");
    AddTranslation("fr", "IR File:", L"Fichier IR :");

    AddTranslation("en", "...", L"...");
    AddTranslation("fr", "...", L"...");

    // Advanced tab
    AddTranslation("en", "Audio buffer settings (changes apply on next file load):",
                   L"Audio buffer settings (changes apply on next file load):");
    AddTranslation("fr", "Audio buffer settings (changes apply on next file load):",
                   L"Paramètres de tampon audio (s'applique au prochain chargement) :");

    AddTranslation("en", "&Buffer size:", L"&Buffer size:");
    AddTranslation("fr", "&Buffer size:", L"Taille de &tampon :");

    AddTranslation("en", "&Update period:", L"&Update period:");
    AddTranslation("fr", "&Update period:", L"Période de mise à jo&ur :");

    AddTranslation("en", "Lower values reduce latency but may cause audio glitches.",
                   L"Lower values reduce latency but may cause audio glitches.");
    AddTranslation("fr", "Lower values reduce latency but may cause audio glitches.",
                   L"Des valeurs faibles réduisent la latence mais peuvent causer des coupures.");

    AddTranslation("en", "Tempo/pitch &algorithm (changes apply on next file load):",
                   L"Tempo/pitch &algorithm (changes apply on next file load):");
    AddTranslation("fr", "Tempo/pitch &algorithm (changes apply on next file load):",
                   L"&Algorithme tempo/hauteur (s'applique au prochain chargement) :");

    AddTranslation("en", "EQ frequencies (Hz) - changes apply on next EQ enable:",
                   L"EQ frequencies (Hz) - changes apply on next EQ enable:");
    AddTranslation("fr", "EQ frequencies (Hz) - changes apply on next EQ enable:",
                   L"Fréquences EQ (Hz) - s'applique à la prochaine activation de l'EQ :");

    AddTranslation("en", "Bass (20-500):", L"Bass (20-500):");
    AddTranslation("fr", "Bass (20-500):", L"Graves (20-500) :");

    AddTranslation("en", "Mid (200-5k):", L"Mid (200-5k):");
    AddTranslation("fr", "Mid (200-5k):", L"Médiums (200-5k) :");

    AddTranslation("en", "Treble (2k-20k):", L"Treble (2k-20k):");
    AddTranslation("fr", "Treble (2k-20k):", L"Aigus (2k-20k) :");

    AddTranslation("en", "&Legacy volume (faster, but affects recordings)",
                   L"&Legacy volume (faster, but affects recordings)");
    AddTranslation("fr", "&Legacy volume (faster, but affects recordings)",
                   L"Vo&lume hérité (plus rapide, mais affecte les enregistrements)");

    AddTranslation("en", "Disable &batch delay (only catches one file at a time)",
                   L"Disable &batch delay (only catches one file at a time)");
    AddTranslation("fr", "Disable &batch delay (only catches one file at a time)",
                   L"Désactiver le délai de &lot (un seul fichier à la fois)");

    AddTranslation("en", "Reset station/podcast &order to alphabetical",
                   L"Reset station/podcast &order to alphabetical");
    AddTranslation("fr", "Reset station/podcast &order to alphabetical",
                   L"Réinitialiser l'&ordre des stations/podcasts (alphabétique)");

    // YouTube tab
    AddTranslation("en", "YouTube Data &API key (optional, enables search):",
                   L"YouTube Data &API key (optional, enables search):");
    AddTranslation("fr", "YouTube Data &API key (optional, enables search):",
                   L"Clé d'&API YouTube Data (facultatif, active la recherche) :");

    AddTranslation("en", "Get an API key from: console.cloud.google.com",
                   L"Get an API key from: console.cloud.google.com");
    AddTranslation("fr", "Get an API key from: console.cloud.google.com",
                   L"Obtenez une clé d'API sur : console.cloud.google.com");

    AddTranslation("en", "Without API key, yt-dlp will be used for search (slower).",
                   L"Without API key, yt-dlp will be used for search (slower).");
    AddTranslation("fr", "Without API key, yt-dlp will be used for search (slower).",
                   L"Sans clé d'API, yt-dlp sera utilisé pour la recherche (plus lent).");

    // SoundTouch tab
    AddTranslation("en", "SoundTouch settings (changes apply on next file load):",
                   L"SoundTouch settings (changes apply on next file load):");
    AddTranslation("fr", "SoundTouch settings (changes apply on next file load):",
                   L"Paramètres SoundTouch (s'applique au prochain chargement) :");

    AddTranslation("en", "&Anti-alias filter", L"&Anti-alias filter");
    AddTranslation("fr", "&Anti-alias filter", L"Filtre &anti-repliement");

    AddTranslation("en", "AA filter &length:", L"AA filter &length:");
    AddTranslation("fr", "AA filter &length:", L"&Longueur du filtre AA :");

    AddTranslation("en", "&Quick algorithm (lower quality, less CPU)",
                   L"&Quick algorithm (lower quality, less CPU)");
    AddTranslation("fr", "&Quick algorithm (lower quality, less CPU)",
                   L"Algorithme &rapide (qualité inférieure, moins de CPU)");

    AddTranslation("en", "&Prevent click (reduces artifacts)",
                   L"&Prevent click (reduces artifacts)");
    AddTranslation("fr", "&Prevent click (reduces artifacts)",
                   L"&Éviter les clics (réduit les artefacts)");

    AddTranslation("en", "&Interpolation:", L"&Interpolation:");
    AddTranslation("fr", "&Interpolation:", L"&Interpolation :");

    AddTranslation("en", "&Sequence (ms):", L"&Sequence (ms):");
    AddTranslation("fr", "&Sequence (ms):", L"&Séquence (ms) :");

    AddTranslation("en", "See&k window:", L"See&k window:");
    AddTranslation("fr", "See&k window:", L"F. recherche :");

    AddTranslation("en", "&Overlap:", L"&Overlap:");
    AddTranslation("fr", "&Overlap:", L"Recouvr. :");

    AddTranslation("en", "(0 = automatic for Sequence/Seek window)",
                   L"(0 = automatic for Sequence/Seek window)");
    AddTranslation("fr", "(0 = automatic for Sequence/Seek window)",
                   L"(0 = automatique pour Séquence/Fenêtre de recherche)");

    // Speedy tab
    AddTranslation("en", "Google Speedy algorithm settings:", L"Google Speedy algorithm settings:");
    AddTranslation("fr", "Google Speedy algorithm settings:",
                   L"Paramètres de l'algorithme Google Speedy :");

    AddTranslation("en", "Speedy uses nonlinear speedup optimized for speech.",
                   L"Speedy uses nonlinear speedup optimized for speech.");
    AddTranslation("fr", "Speedy uses nonlinear speedup optimized for speech.",
                   L"Speedy utilise une accélération non linéaire optimisée pour la parole.");

    AddTranslation("en", "It compresses vowels more than consonants for clarity.",
                   L"It compresses vowels more than consonants for clarity.");
    AddTranslation("fr", "It compresses vowels more than consonants for clarity.",
                   L"Il compresse les voyelles plus que les consonnes pour plus de clarté.");

    AddTranslation("en", "&Enable nonlinear speedup (recommended for speech)",
                   L"&Enable nonlinear speedup (recommended for speech)");
    AddTranslation("fr", "&Enable nonlinear speedup (recommended for speech)",
                   L"Activ&er l'accélération non linéaire (recommandée pour la parole)");

    // Signalsmith tab
    AddTranslation("en", "Signalsmith Stretch settings (changes apply on next file load):",
                   L"Signalsmith Stretch settings (changes apply on next file load):");
    AddTranslation("fr", "Signalsmith Stretch settings (changes apply on next file load):",
                   L"Paramètres Signalsmith Stretch (s'applique au prochain chargement) :");

    AddTranslation("en", "&Quality preset:", L"&Quality preset:");
    AddTranslation("fr", "&Quality preset:", L"Préréglage de &qualité :");

    AddTranslation("en", "&Tonality limit (Hz, 0 = auto):", L"&Tonality limit (Hz, 0 = auto):");
    AddTranslation("fr", "&Tonality limit (Hz, 0 = auto):", L"Limite de &tonalité (Hz, 0 = auto) :");

    AddTranslation("en", "Higher tonality limits preserve more harmonics during pitch shift.",
                   L"Higher tonality limits preserve more harmonics during pitch shift.");
    AddTranslation("fr", "Higher tonality limits preserve more harmonics during pitch shift.",
                   L"Une limite plus élevée préserve plus d'harmoniques lors du décalage de hauteur.");

    AddTranslation("en", "Use 0 for automatic, or 4000-8000 for speech, 8000-16000 for music.",
                   L"Use 0 for automatic, or 4000-8000 for speech, 8000-16000 for music.");
    AddTranslation("fr", "Use 0 for automatic, or 4000-8000 for speech, 8000-16000 for music.",
                   L"Utilisez 0 pour automatique, 4000-8000 pour la parole, 8000-16000 pour la musique.");

    // MIDI tab
    AddTranslation("en", "MIDI playback settings (BASSMIDI):", L"MIDI playback settings (BASSMIDI):");
    AddTranslation("fr", "MIDI playback settings (BASSMIDI):",
                   L"Paramètres de lecture MIDI (BASSMIDI) :");

    AddTranslation("en", "&SoundFont (.sf2/.sf3) — leave empty to use bundled FluidR3_GM:",
                   L"&SoundFont (.sf2/.sf3) — leave empty to use bundled FluidR3_GM:");
    AddTranslation("fr", "&SoundFont (.sf2/.sf3) — leave empty to use bundled FluidR3_GM:",
                   L"&SoundFont (.sf2/.sf3) — laisser vide pour utiliser FluidR3_GM inclus :");

    AddTranslation("en", "Using bundled FluidR3_GM (Frank Wen, MIT license).",
                   L"Using bundled FluidR3_GM (Frank Wen, MIT license).");
    AddTranslation("fr", "Using bundled FluidR3_GM (Frank Wen, MIT license).",
                   L"Utilisation de FluidR3_GM inclus (Frank Wen, licence MIT).");

    AddTranslation("en", "Using BASSMIDI built-in synth (basic sound).",
                   L"Using BASSMIDI built-in synth (basic sound).");
    AddTranslation("fr", "Using BASSMIDI built-in synth (basic sound).",
                   L"Utilisation du synthétiseur intégré BASSMIDI (son basique).");

    AddTranslation("en", "Ma&x voices (polyphony):", L"Ma&x voices (polyphony):");
    AddTranslation("fr", "Ma&x voices (polyphony):", L"Nb ma&x de voix (polyphonie) :");

    AddTranslation("en", "(1-1000, default 128)", L"(1-1000, default 128)");
    AddTranslation("fr", "(1-1000, default 128)", L"(1-1000, par défaut 128)");

    AddTranslation("en", "Use s&inc interpolation (higher quality, more CPU)",
                   L"Use s&inc interpolation (higher quality, more CPU)");
    AddTranslation("fr", "Use s&inc interpolation (higher quality, more CPU)",
                   L"Utiliser l'&interpolation sinc (qualité supérieure, plus de CPU)");

    // OK / Cancel
    AddTranslation("en", "OK", L"OK");
    AddTranslation("fr", "OK", L"OK");
    AddTranslation("en", "Cancel", L"Cancel");
    AddTranslation("fr", "Cancel", L"Annuler");
    AddTranslation("en", "Close", L"Close");
    AddTranslation("fr", "Close", L"Fermer");

    // ===================================================================
    // DIALOG: IDD_SCHED_ADD - "Add Scheduled Event"
    // ===================================================================
    AddTranslation("en", "&Action:", L"&Action:");
    AddTranslation("fr", "&Action:", L"&Action :");

    // ===================================================================
    // DIALOG: IDD_URL - "Open URL"
    // ===================================================================
    AddTranslation("en", "Open URL", L"Open URL");
    AddTranslation("fr", "Open URL", L"Ouvrir une URL");

    AddTranslation("en", "Enter stream &URL (http/https):", L"Enter stream &URL (http/https):");
    AddTranslation("fr", "Enter stream &URL (http/https):", L"Entrez l'&URL du flux (http/https) :");

    // ===================================================================
    // DIALOG: IDD_JUMPTOTIME - "Jump to Time"
    // ===================================================================
    AddTranslation("en", "Jump to Time", L"Jump to Time");
    AddTranslation("fr", "Jump to Time", L"Aller à un instant");

    AddTranslation("en", "Enter &time (mm:ss or hh:mm:ss):", L"Enter &time (mm:ss or hh:mm:ss):");
    AddTranslation("fr", "Enter &time (mm:ss or hh:mm:ss):",
                   L"Entrez l'&heure (mm:ss ou hh:mm:ss) :");

    // ===================================================================
    // DIALOG: IDD_PRESET_NAME - "Save Effect Preset"
    // ===================================================================
    AddTranslation("en", "Save Effect Preset", L"Save Effect Preset");
    AddTranslation("fr", "Save Effect Preset", L"Enregistrer un préréglage d'effet");

    AddTranslation("en", "Preset &name:", L"Preset &name:");
    AddTranslation("fr", "Preset &name:", L"&Nom du préréglage :");

    // ===================================================================
    // DIALOG: IDD_YOUTUBE - "YouTube"
    // ===================================================================
    AddTranslation("en", "YouTube", L"YouTube");
    AddTranslation("fr", "YouTube", L"YouTube");

    AddTranslation("en", "&Search or paste URL:", L"&Search or paste URL:");
    AddTranslation("fr", "&Search or paste URL:", L"&Rechercher ou coller une URL :");

    AddTranslation("en", "&Results:", L"&Results:");
    AddTranslation("fr", "&Results:", L"&Résultats :");

    AddTranslation("en", "&Load More", L"&Load More");
    AddTranslation("fr", "&Load More", L"&Charger plus");

    // ===================================================================
    // DIALOG: IDD_BOOKMARKS - "Bookmarks"
    // ===================================================================
    AddTranslation("en", "Bookmarks", L"Bookmarks");
    AddTranslation("fr", "Bookmarks", L"Signets");

    AddTranslation("en", "&Show:", L"&Show:");
    AddTranslation("fr", "&Show:", L"&Afficher :");

    AddTranslation("en", "Enter = jump, Delete = remove, Escape = close",
                   L"Enter = jump, Delete = remove, Escape = close");
    AddTranslation("fr", "Enter = jump, Delete = remove, Escape = close",
                   L"Entrée = aller, Suppr = retirer, Échap = fermer");

    // ===================================================================
    // DIALOG: IDD_SONG_HISTORY - "Song History"
    // ===================================================================
    AddTranslation("en", "Song History", L"Song History");
    AddTranslation("fr", "Song History", L"Historique des morceaux");

    AddTranslation("en", "Recent songs captured from stream metadata (Ctrl+C to copy):",
                   L"Recent songs captured from stream metadata (Ctrl+C to copy):");
    AddTranslation("fr", "Recent songs captured from stream metadata (Ctrl+C to copy):",
                   L"Morceaux récents extraits des métadonnées du flux (Ctrl+C pour copier) :");

    AddTranslation("en", "&Copy", L"&Copy");
    AddTranslation("fr", "&Copy", L"&Copier");

    AddTranslation("en", "C&lear", L"C&lear");
    AddTranslation("fr", "C&lear", L"Effa&cer");

    // ===================================================================
    // DIALOG: IDD_RADIO - "Internet Radio"
    // ===================================================================
    AddTranslation("en", "Internet Radio", L"Internet Radio");
    AddTranslation("fr", "Internet Radio", L"Radio Internet");

    AddTranslation("en", "Enter = play, Delete = remove, Escape = close",
                   L"Enter = play, Delete = remove, Escape = close");
    AddTranslation("fr", "Enter = play, Delete = remove, Escape = close",
                   L"Entrée = lire, Suppr = retirer, Échap = fermer");

    AddTranslation("en", "&Import...", L"&Import...");
    AddTranslation("fr", "&Import...", L"&Importer...");

    AddTranslation("en", "&Export...", L"&Export...");
    AddTranslation("fr", "&Export...", L"&Exporter...");

    AddTranslation("en", "&Source:", L"&Source:");
    AddTranslation("fr", "&Source:", L"&Source :");

    AddTranslation("en", "&Search", L"&Search");
    AddTranslation("fr", "&Search", L"&Rechercher");

    AddTranslation("en", "Co&untry:", L"Co&untry:");
    AddTranslation("fr", "Co&untry:", L"&Pays :");

    AddTranslation("en", "S&tations:", L"S&tations:");
    AddTranslation("fr", "S&tations:", L"S&tations :");

    AddTranslation("en", "Enter = play, Escape = close", L"Enter = play, Escape = close");
    AddTranslation("fr", "Enter = play, Escape = close", L"Entrée = lire, Échap = fermer");

    AddTranslation("en", "Add to &Favorites", L"Add to &Favorites");
    AddTranslation("fr", "Add to &Favorites", L"Ajouter aux &favoris");

    // ===================================================================
    // DIALOG: IDD_RADIO_ADD - "Add Radio Station"
    // ===================================================================
    AddTranslation("en", "Add Radio Station", L"Add Radio Station");
    AddTranslation("fr", "Add Radio Station", L"Ajouter une station radio");

    AddTranslation("en", "Station &name:", L"Station &name:");
    AddTranslation("fr", "Station &name:", L"&Nom de la station :");

    AddTranslation("en", "Stream &URL:", L"Stream &URL:");
    AddTranslation("fr", "Stream &URL:", L"&URL du flux :");

    AddTranslation("en", "(e.g., http://stream.example.com:8000/radio.mp3)",
                   L"(e.g., http://stream.example.com:8000/radio.mp3)");
    AddTranslation("fr", "(e.g., http://stream.example.com:8000/radio.mp3)",
                   L"(ex. http://stream.example.com:8000/radio.mp3)");

    // Edit Station (set dynamically in code)
    AddTranslation("en", "Edit Station", L"Edit Station");
    AddTranslation("fr", "Edit Station", L"Modifier la station");

    // ===================================================================
    // DIALOG: IDD_SCHEDULER - "Scheduler"
    // ===================================================================
    AddTranslation("en", "Scheduler", L"Scheduler");
    AddTranslation("fr", "Scheduler", L"Planificateur");

    AddTranslation("en", "Scheduled events (Enter = toggle, Delete = remove, Escape = close):",
                   L"Scheduled events (Enter = toggle, Delete = remove, Escape = close):");
    AddTranslation("fr", "Scheduled events (Enter = toggle, Delete = remove, Escape = close):",
                   L"Événements planifiés (Entrée = activer/désactiver, Suppr = retirer, Échap = fermer) :");

    // ===================================================================
    // DIALOG: IDD_SCHED_ADD - "Add Scheduled Event"
    // ===================================================================
    AddTranslation("en", "Add Scheduled Event", L"Add Scheduled Event");
    AddTranslation("fr", "Add Scheduled Event", L"Ajouter un événement planifié");

    AddTranslation("en", "Edit Scheduled Event", L"Edit Scheduled Event");
    AddTranslation("fr", "Edit Scheduled Event", L"Modifier l'événement planifié");

    AddTranslation("en", "&Name:", L"&Name:");
    AddTranslation("fr", "&Name:", L"&Nom :");

    AddTranslation("en", "&File:", L"&File:");
    AddTranslation("fr", "&File:", L"&Fichier :");

    AddTranslation("en", "Ra&dio:", L"Ra&dio:");
    AddTranslation("fr", "Ra&dio:", L"Ra&dio :");

    AddTranslation("en", "&Date:", L"&Date:");
    AddTranslation("fr", "&Date:", L"&Date :");

    AddTranslation("en", "&Time:", L"&Time:");
    AddTranslation("fr", "&Time:", L"&Heure :");

    AddTranslation("en", "&Repeat:", L"&Repeat:");
    AddTranslation("fr", "&Repeat:", L"&Répéter :");

    AddTranslation("en", "D&uration:", L"D&uration:");
    AddTranslation("fr", "D&uration:", L"D&urée :");

    AddTranslation("en", "minutes (0 = no limit)", L"minutes (0 = no limit)");
    AddTranslation("fr", "minutes (0 = no limit)", L"minutes (0 = pas de limite)");

    AddTranslation("en", "S&top:", L"S&top:");
    AddTranslation("fr", "S&top:", L"Arrê&t :");

    AddTranslation("en", "&Enabled", L"&Enabled");
    AddTranslation("fr", "&Enabled", L"&Activé");

    // Scheduler combo items (set in code via CB_ADDSTRING)
    AddTranslation("en", "Playback", L"Playback");
    AddTranslation("fr", "Playback", L"Lecture");

    AddTranslation("en", "Recording", L"Recording");
    AddTranslation("fr", "Recording", L"Enregistrement");

    AddTranslation("en", "Both", L"Both");
    AddTranslation("fr", "Both", L"Les deux");

    // Bookmarks filter combo
    AddTranslation("en", "Current file", L"Current file");
    AddTranslation("fr", "Current file", L"Fichier actuel");

    AddTranslation("en", "All bookmarks", L"All bookmarks");
    AddTranslation("fr", "All bookmarks", L"Tous les signets");

    // ===================================================================
    // DIALOG: IDD_TAG_VIEW - "Tag"
    // ===================================================================
    AddTranslation("en", "Tag", L"Tag");
    AddTranslation("fr", "Tag", L"Métadonnée");

    // ===================================================================
    // DIALOG: IDD_PLAYLIST - "Playlist Manager"
    // ===================================================================
    AddTranslation("en", "Playlist Manager", L"Playlist Manager");
    AddTranslation("fr", "Playlist Manager", L"Gestionnaire de liste de lecture");

    AddTranslation("en", "&Save...", L"&Save...");
    AddTranslation("fr", "&Save...", L"Enregi&strer...");

    AddTranslation("en", "Alt+Up/Down: Move  |  Delete: Remove  |  Enter: Play  |  Ctrl+V: Paste  |  Esc: Close",
                   L"Alt+Up/Down: Move  |  Delete: Remove  |  Enter: Play  |  Ctrl+V: Paste  |  Esc: Close");
    AddTranslation("fr", "Alt+Up/Down: Move  |  Delete: Remove  |  Enter: Play  |  Ctrl+V: Paste  |  Esc: Close",
                   L"Alt+Haut/Bas : Déplacer  |  Suppr : Retirer  |  Entrée : Lire  |  Ctrl+V : Coller  |  Échap : Fermer");

    // ===================================================================
    // DIALOG: IDD_PODCAST - "Podcasts"
    // ===================================================================
    AddTranslation("en", "Podcasts", L"Podcasts");
    AddTranslation("fr", "Podcasts", L"Podcasts");

    AddTranslation("en", "&Subscriptions:", L"&Subscriptions:");
    AddTranslation("fr", "&Subscriptions:", L"A&bonnements :");

    AddTranslation("en", "&Episodes:", L"&Episodes:");
    AddTranslation("fr", "&Episodes:", L"&Épisodes :");

    AddTranslation("en", "Enter = load/play, Delete = unsubscribe, F5 = refresh, Escape = close",
                   L"Enter = load/play, Delete = unsubscribe, F5 = refresh, Escape = close");
    AddTranslation("fr", "Enter = load/play, Delete = unsubscribe, F5 = refresh, Escape = close",
                   L"Entrée = charger/lire, Suppr = se désabonner, F5 = actualiser, Échap = fermer");

    AddTranslation("en", "&Download", L"&Download");
    AddTranslation("fr", "&Download", L"&Télécharger");

    AddTranslation("en", "Download &All", L"Download &All");
    AddTranslation("fr", "Download &All", L"Tout télé&charger");

    AddTranslation("en", "&Export OPML...", L"&Export OPML...");
    AddTranslation("fr", "&Export OPML...", L"&Exporter OPML...");

    AddTranslation("en", "&Refresh", L"&Refresh");
    AddTranslation("fr", "&Refresh", L"Actualise&r");

    AddTranslation("en", "&Search iTunes:", L"&Search iTunes:");
    AddTranslation("fr", "&Search iTunes:", L"&Rechercher sur iTunes :");

    AddTranslation("en", "Enter = preview, Escape = close", L"Enter = preview, Escape = close");
    AddTranslation("fr", "Enter = preview, Escape = close", L"Entrée = aperçu, Échap = fermer");

    AddTranslation("en", "S&ubscribe", L"S&ubscribe");
    AddTranslation("fr", "S&ubscribe", L"S'a&bonner");

    AddTranslation("en", "&Import OPML...", L"&Import OPML...");
    AddTranslation("fr", "&Import OPML...", L"&Importer OPML...");

    AddTranslation("en", "&Add URL...", L"&Add URL...");
    AddTranslation("fr", "&Add URL...", L"&Ajouter une URL...");

    // Tab labels (set in code via TabCtrl_InsertItem)
    AddTranslation("en", "Subscriptions", L"Subscriptions");
    AddTranslation("fr", "Subscriptions", L"Abonnements");

    AddTranslation("en", "Search", L"Search");
    AddTranslation("fr", "Search", L"Rechercher");

    AddTranslation("en", "Favorites", L"Favorites");
    AddTranslation("fr", "Favorites", L"Favoris");

    // Options tabs (set in code)
    AddTranslation("en", "Downloads", L"Downloads");
    AddTranslation("fr", "Downloads", L"Téléchargements");

    AddTranslation("en", "Speech", L"Speech");
    AddTranslation("fr", "Speech", L"Voix");

    AddTranslation("en", "Movement", L"Movement");
    AddTranslation("fr", "Movement", L"Déplacement");

    AddTranslation("en", "Effects", L"Effects");
    AddTranslation("fr", "Effects", L"Effets");

    AddTranslation("en", "Advanced", L"Advanced");
    AddTranslation("fr", "Advanced", L"Avancé");

    AddTranslation("en", "SoundTouch", L"SoundTouch");
    AddTranslation("fr", "SoundTouch", L"SoundTouch");

    AddTranslation("en", "Speedy", L"Speedy");
    AddTranslation("fr", "Speedy", L"Speedy");

    AddTranslation("en", "Signalsmith", L"Signalsmith");
    AddTranslation("fr", "Signalsmith", L"Signalsmith");

    AddTranslation("en", "MIDI", L"MIDI");
    AddTranslation("fr", "MIDI", L"MIDI");

    // ===================================================================
    // DIALOG: IDD_PODCAST_ADD - "Add Podcast"
    // ===================================================================
    AddTranslation("en", "Add Podcast", L"Add Podcast");
    AddTranslation("fr", "Add Podcast", L"Ajouter un podcast");

    AddTranslation("en", "Feed &URL:", L"Feed &URL:");
    AddTranslation("fr", "Feed &URL:", L"&URL du flux :");

    AddTranslation("en", "(RSS feed URL, e.g., https://example.com/feed.xml)",
                   L"(RSS feed URL, e.g., https://example.com/feed.xml)");
    AddTranslation("fr", "(RSS feed URL, e.g., https://example.com/feed.xml)",
                   L"(URL de flux RSS, ex. https://example.com/feed.xml)");

    AddTranslation("en", "&Username:", L"&Username:");
    AddTranslation("fr", "&Username:", L"Nom d'&utilisateur :");

    AddTranslation("en", "&Password:", L"&Password:");
    AddTranslation("fr", "&Password:", L"Mot de &passe :");

    AddTranslation("en", "(Leave blank if feed doesn't require authentication)",
                   L"(Leave blank if feed doesn't require authentication)");
    AddTranslation("fr", "(Leave blank if feed doesn't require authentication)",
                   L"(Laissez vide si le flux ne nécessite pas d'authentification)");

    // ===================================================================
    // DIALOG: IDD_PROGRESS - "Downloading Update"
    // ===================================================================
    AddTranslation("en", "Downloading Update", L"Downloading Update");
    AddTranslation("fr", "Downloading Update", L"Téléchargement de la mise à jour");

    AddTranslation("en", "Preparing download...", L"Preparing download...");
    AddTranslation("fr", "Preparing download...", L"Préparation du téléchargement...");

    // ===================================================================
    // MENU: &Tools (added in v1.41 for the Actions/Keymap system)
    // ===================================================================
    AddTranslation("en", "&Tools", L"&Tools");
    AddTranslation("fr", "&Tools", L"&Outils");

    AddTranslation("en", "&Actions...\tF4", L"&Actions...\tF4");
    AddTranslation("fr", "&Actions...\tF4", L"&Actions...\tF4");

    // ===================================================================
    // DIALOG: IDD_ACTIONS - "Actions" (the F4 keymap editor)
    // ===================================================================
    AddTranslation("en", "Actions", L"Actions");
    AddTranslation("fr", "Actions", L"Actions");

    AddTranslation("en", "Section:", L"Section:");
    AddTranslation("fr", "Section:", L"Section :");

    AddTranslation("en", "Filter:", L"Filter:");
    AddTranslation("fr", "Filter:", L"Filtre :");

    AddTranslation("en", "Actions:", L"Actions:");
    AddTranslation("fr", "Actions:", L"Actions :");

    AddTranslation("en", "Shortcuts for selected action:", L"Shortcuts for selected action:");
    AddTranslation("fr", "Shortcuts for selected action:", L"Raccourcis pour l'action sélectionnée :");

    AddTranslation("en", "&Add...", L"&Add...");
    AddTranslation("fr", "&Add...", L"&Ajouter...");

    AddTranslation("en", "&Edit...", L"&Edit...");
    AddTranslation("fr", "&Edit...", L"&Modifier...");

    AddTranslation("en", "&Delete", L"&Delete");
    AddTranslation("fr", "&Delete", L"&Supprimer");

    AddTranslation("en", "Current keymap:", L"Current keymap:");
    AddTranslation("fr", "Current keymap:", L"Keymap actuel :");

    AddTranslation("en", "&Load...", L"&Load...");
    AddTranslation("fr", "&Load...", L"&Charger...");

    AddTranslation("en", "&Save as...", L"&Save as...");
    AddTranslation("fr", "&Save as...", L"&Enregistrer sous...");

    AddTranslation("en", "&Reset to defaults", L"&Reset to defaults");
    AddTranslation("fr", "&Reset to defaults", L"&Réinitialiser aux défauts");

    AddTranslation("en", "Close", L"Close");
    AddTranslation("fr", "Close", L"Fermer");

    // ===================================================================
    // DIALOG: IDD_SHORTCUT_ASSIGN - "Assign shortcut"
    // ===================================================================
    AddTranslation("en", "Assign shortcut", L"Assign shortcut");
    AddTranslation("fr", "Assign shortcut", L"Assigner un raccourci");

    AddTranslation("en", "Press the key combination you want to assign:",
                   L"Press the key combination you want to assign:");
    AddTranslation("fr", "Press the key combination you want to assign:",
                   L"Appuyez sur la combinaison de touches à assigner :");

    AddTranslation("en", "Press Backspace alone to clear.",
                   L"Press Backspace alone to clear.");
    AddTranslation("fr", "Press Backspace alone to clear.",
                   L"Appuyez sur Retour arrière seul pour effacer.");

    // ===================================================================
    // Ts() strings used in code (src/actions_window.cpp + src/keymap.cpp +
    // src/keyboard_help.cpp). The English source IS the lookup key.
    // ===================================================================
    AddTranslation("en", "This shortcut is already assigned to",
                   L"This shortcut is already assigned to");
    AddTranslation("fr", "This shortcut is already assigned to",
                   L"Ce raccourci est déjà assigné à");

    AddTranslation("en", "Replace the existing assignment?",
                   L"Replace the existing assignment?");
    AddTranslation("fr", "Replace the existing assignment?",
                   L"Remplacer l'assignation existante ?");

    AddTranslation("en", "This shortcut is already assigned. Replace it?",
                   L"This shortcut is already assigned. Replace it?");
    AddTranslation("fr", "This shortcut is already assigned. Replace it?",
                   L"Ce raccourci est déjà assigné. Le remplacer ?");

    AddTranslation("en", "Shortcut conflict", L"Shortcut conflict");
    AddTranslation("fr", "Shortcut conflict", L"Conflit de raccourci");

    AddTranslation("en", "Could not load keymap", L"Could not load keymap");
    AddTranslation("fr", "Could not load keymap", L"Impossible de charger le keymap");

    AddTranslation("en", "Could not save keymap", L"Could not save keymap");
    AddTranslation("fr", "Could not save keymap", L"Impossible d'enregistrer le keymap");

    AddTranslation("en", "Reset all shortcuts to the regional default for this keyboard layout?",
                   L"Reset all shortcuts to the regional default for this keyboard layout?");
    AddTranslation("fr", "Reset all shortcuts to the regional default for this keyboard layout?",
                   L"Réinitialiser tous les raccourcis au défaut régional pour votre clavier ?");

    AddTranslation("en", "no action assigned", L"no action assigned");
    AddTranslation("fr", "no action assigned", L"aucune action assignée");

    // -------------------------------------------------------------------
    // Find-by-shortcut dialog (IDD_FIND_SHORTCUT + the trigger button)
    // -------------------------------------------------------------------
    AddTranslation("en", "Find &shortcut...", L"Find &shortcut...");
    AddTranslation("fr", "Find &shortcut...", L"Rechercher un &raccourci...");

    AddTranslation("en", "Find shortcut", L"Find shortcut");
    AddTranslation("fr", "Find shortcut", L"Rechercher un raccourci");

    AddTranslation("en", "Press the shortcut you want to find:",
                   L"Press the shortcut you want to find:");
    AddTranslation("fr", "Press the shortcut you want to find:",
                   L"Appuyez sur le raccourci à rechercher :");

    AddTranslation("en", "Then press Tab to the Search button.",
                   L"Then press Tab to the Search button.");
    AddTranslation("fr", "Then press Tab to the Search button.",
                   L"Puis appuyez sur Tab pour atteindre le bouton Rechercher.");

    AddTranslation("en", "&Search", L"&Search");
    AddTranslation("fr", "&Search", L"&Rechercher");

    AddTranslation("en", "No action is assigned to this shortcut in this section.",
                   L"No action is assigned to this shortcut in this section.");
    AddTranslation("fr", "No action is assigned to this shortcut in this section.",
                   L"Aucune action n'est assignée à ce raccourci dans cette section.");
}
