// translations_ui.cpp — French translations for user-visible UI strings.
//
// All keys are the original English source text exactly as it appears in
// the C++ source files (the T()/Ts() lookup uses the English string as the
// key). The English entries are mostly identity mappings; they exist so
// LocalizeDialog() / LocalizeMenu() find a translation and replace text
// in-place (which also keeps any stale legacy text in the .rc in sync).

#include "mediaaccess/translations.h"

void RegisterUiTranslations() {
    // ============================================================
    // Options dialog — tab names
    // ============================================================
    AddTranslation("en", "Playback",        L"Playback");
    AddTranslation("fr", "Playback",        L"Lecture");
    AddTranslation("en", "Recording",       L"Recording");
    AddTranslation("fr", "Recording",       L"Enregistrement");
    AddTranslation("en", "Downloads",       L"Downloads");
    AddTranslation("fr", "Downloads",       L"Téléchargements");
    AddTranslation("en", "Speech",          L"Speech");
    AddTranslation("fr", "Speech",          L"Parole");
    AddTranslation("en", "Movement",        L"Movement");
    AddTranslation("fr", "Movement",        L"Déplacement");
    AddTranslation("en", "Global Hotkeys",  L"Global Hotkeys");
    AddTranslation("fr", "Global Hotkeys",  L"Raccourcis globaux");
    AddTranslation("en", "Effects",         L"Effects");
    AddTranslation("fr", "Effects",         L"Effets");
    AddTranslation("en", "Advanced",        L"Advanced");
    AddTranslation("fr", "Advanced",        L"Avancé");
    AddTranslation("en", "YouTube",         L"YouTube");
    AddTranslation("fr", "YouTube",         L"YouTube");
    AddTranslation("en", "SoundTouch",      L"SoundTouch");
    AddTranslation("fr", "SoundTouch",      L"SoundTouch");
    AddTranslation("en", "Speedy",          L"Speedy");
    AddTranslation("fr", "Speedy",          L"Speedy");
    AddTranslation("en", "Signalsmith",     L"Signalsmith");
    AddTranslation("fr", "Signalsmith",     L"Signalsmith");
    AddTranslation("en", "MIDI",            L"MIDI");
    AddTranslation("fr", "MIDI",            L"MIDI");

    // ============================================================
    // Options dialog — labels and controls (matched to .rc text so
    // LocalizeDialog() finds them when scanning child windows).
    // ============================================================
    AddTranslation("en", "Options",                                 L"Options");
    AddTranslation("fr", "Options",                                 L"Options");
    AddTranslation("en", "&Output device:",                         L"&Output device:");
    AddTranslation("fr", "&Output device:",                         L"Sortie audio :");
    AddTranslation("en", "&Allow volume above 100%",                L"&Allow volume above 100%");
    AddTranslation("fr", "&Allow volume above 100%",                L"&Autoriser le volume au-dessus de 100 %");
    AddTranslation("en", "&Remember playback state on exit",        L"&Remember playback state on exit");
    AddTranslation("fr", "&Remember playback state on exit",        L"&Mémoriser l'état de lecture à la sortie");
    AddTranslation("en", "Remember p&osition if longer than:",      L"Remember p&osition if longer than:");
    AddTranslation("fr", "Remember p&osition if longer than:",      L"Reprendre si plus long que :");
    AddTranslation("en", "&Bring window to front when opening files",   L"&Bring window to front when opening files");
    AddTranslation("fr", "&Bring window to front when opening files",   L"&Mettre la fenêtre au premier plan à l'ouverture");
    AddTranslation("en", "&Load all files in folder when opening single file", L"&Load all files in folder when opening single file");
    AddTranslation("fr", "&Load all files in folder when opening single file", L"&Charger tous les fichiers du dossier à l'ouverture d'un seul");
    AddTranslation("en", "&Minimize to system tray",                L"&Minimize to system tray");
    AddTranslation("fr", "&Minimize to system tray",                L"&Réduire dans la zone de notification");
    AddTranslation("en", "Show &track name in window title",        L"Show &track name in window title");
    AddTranslation("fr", "Show &track name in window title",        L"Afficher le nom de la &piste dans le titre");
    AddTranslation("en", "Auto-ad&vance to next playlist item",     L"Auto-ad&vance to next playlist item");
    AddTranslation("fr", "Auto-ad&vance to next playlist item",     L"Lecture auto de l'élément suivant");
    AddTranslation("en", "&Follow playback in playlist dialog",     L"&Follow playback in playlist dialog");
    AddTranslation("fr", "&Follow playback in playlist dialog",     L"&Suivre la lecture dans la liste");
    AddTranslation("en", "Check for &updates on startup",           L"Check for &updates on startup");
    AddTranslation("fr", "Check for &updates on startup",           L"Vérifier les mises à jo&ur au démarrage");
    AddTranslation("en", "Allow &multiple instances",               L"Allow &multiple instances");
    AddTranslation("fr", "Allow &multiple instances",               L"Autoriser plusieurs &instances");
    AddTranslation("en", "Register all supported &file types",      L"Register all supported &file types");
    AddTranslation("fr", "Register all supported &file types",      L"Associer tous les types de &fichiers pris en charge");
    AddTranslation("en", "Re&wind on pause (ms):",                  L"Re&wind on pause (ms):");
    AddTranslation("fr", "Re&wind on pause (ms):",                  L"Re&tour en arrière à la pause (ms) :");
    AddTranslation("en", "Volu&me step:",                           L"Volu&me step:");
    AddTranslation("fr", "Volu&me step:",                           L"Pas de volu&me :");
    AddTranslation("en", "Lan&guage:",                              L"Lan&guage:");
    AddTranslation("fr", "Lan&guage:",                              L"Lan&gue :");

    // Recording tab
    AddTranslation("en", "Record audio output to file. Press R to toggle recording.",
                         L"Record audio output to file. Press R to toggle recording.");
    AddTranslation("fr", "Record audio output to file. Press R to toggle recording.",
                         L"Enregistrer la sortie audio dans un fichier. Appuyez sur R pour activer.");
    AddTranslation("en", "&Output folder:",                         L"&Output folder:");
    AddTranslation("fr", "&Output folder:",                         L"D&ossier de sortie :");
    AddTranslation("en", "&Browse...",                              L"&Browse...");
    AddTranslation("fr", "&Browse...",                              L"&Parcourir...");
    AddTranslation("en", "Filename &template:",                     L"Filename &template:");
    AddTranslation("fr", "Filename &template:",                     L"Modèle de nom de fichier :");
    AddTranslation("en", "(Uses strftime format: %Y=year, %m=month, %d=day, %H=hour, %M=min, %S=sec)",
                         L"(Available: {year}, {month}, {day}, {hour}, {minute}, {second})");
    AddTranslation("fr", "(Uses strftime format: %Y=year, %m=month, %d=day, %H=hour, %M=min, %S=sec)",
                         L"(Disponibles : {année}, {mois}, {jour}, {heure}, {minute}, {seconde})");
    AddTranslation("en", "&Format:",                                L"&Format:");
    AddTranslation("fr", "&Format:",                                L"&Format :");
    AddTranslation("en", "&Bitrate:",                               L"&Bitrate:");
    AddTranslation("fr", "&Bitrate:",                               L"Dé&bit :");
    AddTranslation("en", "(Bitrate only applies to MP3 and OGG formats)",
                         L"(Bitrate only applies to MP3 and OGG formats)");
    AddTranslation("fr", "(Bitrate only applies to MP3 and OGG formats)",
                         L"(Le débit s'applique uniquement aux formats MP3 et OGG)");

    // Downloads tab
    AddTranslation("en", "Configure podcast episode download settings.",
                         L"Configure podcast episode download settings.");
    AddTranslation("fr", "Configure podcast episode download settings.",
                         L"Configurer les paramètres de téléchargement des épisodes.");
    AddTranslation("en", "&Downloads folder:",                      L"&Downloads folder:");
    AddTranslation("fr", "&Downloads folder:",                      L"Dossier de télé&chargement :");
    AddTranslation("en", "&Organize downloads into folders by feed title",
                         L"&Organize downloads into folders by feed title");
    AddTranslation("fr", "&Organize downloads into folders by feed title",
                         L"&Organiser les téléchargements en dossiers par flux");

    // Speech tab
    AddTranslation("en", "Configure speech feedback for various events.",
                         L"Configure speech feedback for various events.");
    AddTranslation("fr", "Configure speech feedback for various events.",
                         L"Configurer la voix pour divers événements.");
    AddTranslation("en", "&Announce track changes",                 L"&Announce track changes");
    AddTranslation("fr", "&Announce track changes",                 L"&Annoncer les changements de piste");
    AddTranslation("en", "Speak &volume when adjusted",             L"Speak &volume when adjusted");
    AddTranslation("fr", "Speak &volume when adjusted",             L"Annoncer le &volume lors du réglage");
    AddTranslation("en", "Speak &effect value when adjusted",       L"Speak &effect value when adjusted");
    AddTranslation("fr", "Speak &effect value when adjusted",       L"Annoncer la valeur d'&effet lors du réglage");

    // Movement tab
    AddTranslation("en", "Seek amounts (use , and . to cycle):",    L"Seek amounts (use , and . to cycle):");
    AddTranslation("fr", "Seek amounts (use , and . to cycle):",    L"Pas de recherche (utilisez , et . pour parcourir) :");
    AddTranslation("en", "&1 second",                               L"&1 second");
    AddTranslation("fr", "&1 second",                               L"&1 seconde");
    AddTranslation("en", "&5 seconds",                              L"&5 seconds");
    AddTranslation("fr", "&5 seconds",                              L"&5 secondes");
    AddTranslation("en", "1&0 seconds",                             L"1&0 seconds");
    AddTranslation("fr", "1&0 seconds",                             L"1&0 secondes");
    AddTranslation("en", "&30 seconds",                             L"&30 seconds");
    AddTranslation("fr", "&30 seconds",                             L"&30 secondes");
    AddTranslation("en", "1 &minute",                               L"1 &minute");
    AddTranslation("fr", "1 &minute",                               L"1 &minute");
    AddTranslation("en", "5 m&inutes",                              L"5 m&inutes");
    AddTranslation("fr", "5 m&inutes",                              L"5 m&inutes");
    AddTranslation("en", "10 min&utes",                             L"10 min&utes");
    AddTranslation("fr", "10 min&utes",                             L"10 min&utes");
    AddTranslation("en", "3&0 minutes",                             L"3&0 minutes");
    AddTranslation("fr", "3&0 minutes",                             L"3&0 minutes");
    AddTranslation("en", "1 &hour",                                 L"1 &hour");
    AddTranslation("fr", "1 &hour",                                 L"1 &heure");
    AddTranslation("en", "1 &track",                                L"1 &track");
    AddTranslation("fr", "1 &track",                                L"1 pis&te");
    AddTranslation("en", "5 t&racks",                               L"5 t&racks");
    AddTranslation("fr", "5 t&racks",                               L"5 pist&es");
    AddTranslation("en", "10 trac&ks",                              L"10 trac&ks");
    AddTranslation("fr", "10 trac&ks",                              L"10 pistes");
    AddTranslation("en", "1 c&hapter (if available)",               L"1 c&hapter (if available)");
    AddTranslation("fr", "1 c&hapter (if available)",               L"1 c&hapitre (si disponible)");

    // Global hotkeys tab
    AddTranslation("en", "&Enable global hotkeys",                  L"&Enable global hotkeys");
    AddTranslation("fr", "&Enable global hotkeys",                  L"&Activer les raccourcis globaux");
    AddTranslation("en", "&Add...",                                 L"&Add...");
    AddTranslation("fr", "&Add...",                                 L"&Ajouter...");
    AddTranslation("en", "&Edit...",                                L"&Edit...");
    AddTranslation("fr", "&Edit...",                                L"&Modifier...");
    AddTranslation("en", "&Remove",                                 L"&Remove");
    AddTranslation("fr", "&Remove",                                 L"&Retirer");

    // Effects tab
    AddTranslation("en", "Stream effects ([ ] to cycle, Up/Down to adjust):",
                         L"Stream effects ([ ] to cycle, Up/Down to adjust):");
    AddTranslation("fr", "Stream effects ([ ] to cycle, Up/Down to adjust):",
                         L"Effets de flux ([ ] pour parcourir, Haut/Bas pour ajuster) :");
    AddTranslation("en", "&Volume (0-400%)",                        L"&Volume (0-400%)");
    AddTranslation("fr", "&Volume (0-400%)",                        L"&Volume (0-400 %)");
    AddTranslation("en", "&Pitch (-12 to +12 semitones)",           L"&Pitch (-12 to +12 semitones)");
    AddTranslation("fr", "&Pitch (-12 to +12 semitones)",           L"&Hauteur (-12 à +12 demi-tons)");
    AddTranslation("en", "&Tempo (-50% to +100%)",                  L"&Tempo (-50% to +100%)");
    AddTranslation("fr", "&Tempo (-50% to +100%)",                  L"&Tempo (-50 % à +100 %)");
    AddTranslation("en", "Playback &Rate (0.5x - 2x)",              L"Playback &Rate (0.5x - 2x)");
    AddTranslation("fr", "Playback &Rate (0.5x - 2x)",              L"&Vitesse de lecture (0.5x - 2x)");
    AddTranslation("en", "Step:",                                   L"Step:");
    AddTranslation("fr", "Step:",                                   L"Pas :");
    AddTranslation("en", "DSP effects (enable to add their parameters to cycle list):",
                         L"DSP effects (enable to add their parameters to cycle list):");
    AddTranslation("fr", "DSP effects (enable to add their parameters to cycle list):",
                         L"Effets DSP (activez pour ajouter leurs paramètres) :");
    AddTranslation("en", "Re&verb:",                                L"Re&verb:");
    AddTranslation("fr", "Re&verb:",                                L"Ré&verbération :");
    AddTranslation("en", "&Echo",                                   L"&Echo");
    AddTranslation("fr", "&Echo",                                   L"&Écho");
    AddTranslation("en", "E&Q (Bass/Mid/Treble)",                   L"E&Q (Bass/Mid/Treble)");
    AddTranslation("fr", "E&Q (Bass/Mid/Treble)",                   L"É&galiseur (Graves/Médiums/Aigus)");
    AddTranslation("en", "&Compressor",                             L"&Compressor");
    AddTranslation("fr", "&Compressor",                             L"&Compresseur");
    AddTranslation("en", "&Stereo Width (0-200%)",                  L"&Stereo Width (0-200%)");
    AddTranslation("fr", "&Stereo Width (0-200%)",                  L"&Largeur stéréo (0-200 %)");
    AddTranslation("en", "Ce&nter Cancel (-100 to +100%)",          L"Ce&nter Cancel (-100 to +100%)");
    AddTranslation("fr", "Ce&nter Cancel (-100 to +100%)",          L"A&nnulation du centre (-100 à +100 %)");
    AddTranslation("en", "&3D Audio (HRTF/Binaural)",               L"&3D Audio (HRTF/Binaural)");
    AddTranslation("fr", "&3D Audio (HRTF/Binaural)",               L"Audio &3D (HRTF/Binaural)");
    AddTranslation("en", "Co&nvolution Reverb",                     L"Co&nvolution Reverb");
    AddTranslation("fr", "Co&nvolution Reverb",                     L"Réverbération par co&nvolution");
    AddTranslation("en", "IR File:",                                L"IR File:");
    AddTranslation("fr", "IR File:",                                L"Fichier IR :");

    // Advanced tab
    AddTranslation("en", "Audio buffer settings (changes apply on next file load):",
                         L"Audio buffer settings (changes apply on next file load):");
    AddTranslation("fr", "Audio buffer settings (changes apply on next file load):",
                         L"Paramètres de mémoire tampon (effectifs au prochain chargement) :");
    AddTranslation("en", "&Buffer size:",                           L"&Buffer size:");
    AddTranslation("fr", "&Buffer size:",                           L"Taille de la mémoire tam&pon :");
    AddTranslation("en", "&Update period:",                         L"&Update period:");
    AddTranslation("fr", "&Update period:",                         L"Période de mise à jo&ur :");
    AddTranslation("en", "Lower values reduce latency but may cause audio glitches.",
                         L"Lower values reduce latency but may cause audio glitches.");
    AddTranslation("fr", "Lower values reduce latency but may cause audio glitches.",
                         L"Des valeurs plus basses réduisent la latence mais peuvent causer des coupures.");
    AddTranslation("en", "Tempo/pitch &algorithm (changes apply on next file load):",
                         L"Tempo/pitch &algorithm (changes apply on next file load):");
    AddTranslation("fr", "Tempo/pitch &algorithm (changes apply on next file load):",
                         L"&Algorithme tempo/hauteur (effectif au prochain chargement) :");
    AddTranslation("en", "EQ frequencies (Hz) - changes apply on next EQ enable:",
                         L"EQ frequencies (Hz) - changes apply on next EQ enable:");
    AddTranslation("fr", "EQ frequencies (Hz) - changes apply on next EQ enable:",
                         L"Fréquences EQ (Hz) — effectives à la prochaine activation :");
    AddTranslation("en", "Bass (20-500):",                          L"Bass (20-500):");
    AddTranslation("fr", "Bass (20-500):",                          L"Graves (20-500) :");
    AddTranslation("en", "Mid (200-5k):",                           L"Mid (200-5k):");
    AddTranslation("fr", "Mid (200-5k):",                           L"Médiums (200-5k) :");
    AddTranslation("en", "Treble (2k-20k):",                        L"Treble (2k-20k):");
    AddTranslation("fr", "Treble (2k-20k):",                        L"Aigus (2k-20k) :");
    AddTranslation("en", "&Legacy volume (faster, but affects recordings)",
                         L"&Legacy volume (faster, but affects recordings)");
    AddTranslation("fr", "&Legacy volume (faster, but affects recordings)",
                         L"Vo&lume hérité (plus rapide, mais affecte les enregistrements)");
    AddTranslation("en", "Disable &batch delay (only catches one file at a time)",
                         L"Disable &batch delay (only catches one file at a time)");
    AddTranslation("fr", "Disable &batch delay (only catches one file at a time)",
                         L"Désactiver le délai de regroupement (un fichier à la fois)");
    AddTranslation("en", "Reset station/podcast &order to alphabetical",
                         L"Reset station/podcast &order to alphabetical");
    AddTranslation("fr", "Reset station/podcast &order to alphabetical",
                         L"Réinitialiser l'&ordre des stations/podcasts (alphabétique)");

    // YouTube tab
    // (No hardcoded user-facing strings beyond the labels managed via .rc)

    // Status bar / window state
    AddTranslation("en", "Playing",          L"Playing");
    AddTranslation("fr", "Playing",          L"Lecture");
    AddTranslation("en", "Paused",           L"Paused");
    AddTranslation("fr", "Paused",           L"En pause");
    AddTranslation("en", "Stopped",          L"Stopped");
    AddTranslation("fr", "Stopped",          L"Arrêté");
    AddTranslation("en", "Video",            L"Video");
    AddTranslation("fr", "Video",            L"Vidéo");
    AddTranslation("en", "Vol",              L"Vol");
    AddTranslation("fr", "Vol",              L"Vol");
    AddTranslation("en", "REC",              L"REC");
    AddTranslation("fr", "REC",              L"ENR");

    // ============================================================
    // ui.cpp — Speech and dialog strings
    // ============================================================
    AddTranslation("en", "files loaded",     L"files loaded");
    AddTranslation("fr", "files loaded",     L"fichiers chargés");
    AddTranslation("en", "No audio files found", L"No audio files found");
    AddTranslation("fr", "No audio files found", L"Aucun fichier audio trouvé");
    AddTranslation("en", "Saved preset",     L"Saved preset");
    AddTranslation("fr", "Saved preset",     L"Préréglage enregistré");
    AddTranslation("en", "(No presets saved)", L"(No presets saved)");
    AddTranslation("fr", "(No presets saved)", L"(Aucun préréglage enregistré)");
    AddTranslation("en", "&Delete preset",   L"&Delete preset");
    AddTranslation("fr", "&Delete preset",   L"&Supprimer le préréglage");
    AddTranslation("en", "&Save current as new preset...",
                         L"&Save current as new preset...");
    AddTranslation("fr", "&Save current as new preset...",
                         L"&Enregistrer comme nouveau préréglage...");
    AddTranslation("en", "Song copied",      L"Song copied");
    AddTranslation("fr", "Song copied",      L"Chanson copiée");
    AddTranslation("en", "Clear all song history?", L"Clear all song history?");
    AddTranslation("fr", "Clear all song history?", L"Effacer tout l'historique des chansons ?");
    AddTranslation("en", "Song History",     L"Song History");
    AddTranslation("fr", "Song History",     L"Historique des chansons");
    AddTranslation("en", "Select folder to add to playlist",
                         L"Select folder to add to playlist");
    AddTranslation("fr", "Select folder to add to playlist",
                         L"Sélectionner un dossier à ajouter à la liste");

    // ============================================================
    // ui_options.cpp specific
    // ============================================================
    AddTranslation("en", "Order reset to alphabetical",
                         L"Order reset to alphabetical");
    AddTranslation("fr", "Order reset to alphabetical",
                         L"Ordre réinitialisé (alphabétique)");
    AddTranslation("en", "Language change will fully apply after restart.",
                         L"Language change will fully apply after restart.");
    AddTranslation("fr", "Language change will fully apply after restart.",
                         L"Le changement de langue sera pleinement appliqué après redémarrage.");
    AddTranslation("en", "Select recording output folder",
                         L"Select recording output folder");
    AddTranslation("fr", "Select recording output folder",
                         L"Sélectionner le dossier d'enregistrement");
    AddTranslation("en", "Select downloads folder",
                         L"Select downloads folder");
    AddTranslation("fr", "Select downloads folder",
                         L"Sélectionner le dossier de téléchargement");
    AddTranslation("en", "Select yt-dlp executable",
                         L"Select yt-dlp executable");
    AddTranslation("fr", "Select yt-dlp executable",
                         L"Sélectionner l'exécutable yt-dlp");
    AddTranslation("en", "Select SoundFont file",
                         L"Select SoundFont file");
    AddTranslation("fr", "Select SoundFont file",
                         L"Sélectionner un fichier SoundFont");
    AddTranslation("en", "Select Impulse Response file",
                         L"Select Impulse Response file");
    AddTranslation("fr", "Select Impulse Response file",
                         L"Sélectionner un fichier de réponse impulsionnelle");

    // ============================================================
    // ui_scheduler.cpp
    // ============================================================
    AddTranslation("en", "Scheduled event ended",       L"Scheduled event ended");
    AddTranslation("fr", "Scheduled event ended",       L"Événement planifié terminé");
    AddTranslation("en", "Scheduled playback ended",    L"Scheduled playback ended");
    AddTranslation("fr", "Scheduled playback ended",    L"Lecture planifiée terminée");
    AddTranslation("en", "Scheduled recording ended",   L"Scheduled recording ended");
    AddTranslation("fr", "Scheduled recording ended",   L"Enregistrement planifié terminé");
    AddTranslation("en", "Scheduled event:",            L"Scheduled event:");
    AddTranslation("fr", "Scheduled event:",            L"Événement planifié :");
    AddTranslation("en", "Enabled",                     L"Enabled");
    AddTranslation("fr", "Enabled",                     L"Activé");
    AddTranslation("en", "Disabled",                    L"Disabled");
    AddTranslation("fr", "Disabled",                    L"Désactivé");
    AddTranslation("en", "Schedule removed",            L"Schedule removed");
    AddTranslation("fr", "Schedule removed",            L"Planification supprimée");
    AddTranslation("en", "Schedule added",              L"Schedule added");
    AddTranslation("fr", "Schedule added",              L"Planification ajoutée");
    AddTranslation("en", "Schedule updated",            L"Schedule updated");
    AddTranslation("fr", "Schedule updated",            L"Planification mise à jour");
    AddTranslation("en", "No schedule selected",        L"No schedule selected");
    AddTranslation("fr", "No schedule selected",        L"Aucune planification sélectionnée");
    AddTranslation("en", "Edit Scheduled Event",        L"Edit Scheduled Event");
    AddTranslation("fr", "Edit Scheduled Event",        L"Modifier l'événement planifié");
    AddTranslation("en", "Add Scheduled Event",         L"Add Scheduled Event");
    AddTranslation("fr", "Add Scheduled Event",         L"Ajouter un événement planifié");
    AddTranslation("en", "File",                        L"File");
    AddTranslation("fr", "File",                        L"Fichier");
    AddTranslation("en", "Radio",                       L"Radio");
    AddTranslation("fr", "Radio",                       L"Radio");
    AddTranslation("en", "Both",                        L"Both");
    AddTranslation("fr", "Both",                        L"Les deux");
    AddTranslation("en", "Once",                        L"Once");
    AddTranslation("fr", "Once",                        L"Une fois");
    AddTranslation("en", "Daily",                       L"Daily");
    AddTranslation("fr", "Daily",                       L"Quotidien");
    AddTranslation("en", "Weekly",                      L"Weekly");
    AddTranslation("fr", "Weekly",                      L"Hebdomadaire");
    AddTranslation("en", "Weekdays",                    L"Weekdays");
    AddTranslation("fr", "Weekdays",                    L"Jours de semaine");
    AddTranslation("en", "Weekends",                    L"Weekends");
    AddTranslation("fr", "Weekends",                    L"Week-ends");
    AddTranslation("en", "Monthly",                     L"Monthly");
    AddTranslation("fr", "Monthly",                     L"Mensuel");
    AddTranslation("en", "Playback only",               L"Playback only");
    AddTranslation("fr", "Playback only",               L"Lecture seule");
    AddTranslation("en", "Recording only",              L"Recording only");
    AddTranslation("fr", "Recording only",              L"Enregistrement seul");
    AddTranslation("en", "Select Audio File",           L"Select Audio File");
    AddTranslation("fr", "Select Audio File",           L"Sélectionner un fichier audio");
    AddTranslation("en", "Please enter a name.",        L"Please enter a name.");
    AddTranslation("fr", "Please enter a name.",        L"Veuillez entrer un nom.");
    AddTranslation("en", "Please select a file.",       L"Please select a file.");
    AddTranslation("fr", "Please select a file.",       L"Veuillez sélectionner un fichier.");
    AddTranslation("en", "Please select a radio station.", L"Please select a radio station.");
    AddTranslation("fr", "Please select a radio station.", L"Veuillez sélectionner une station radio.");
    AddTranslation("en", "Add Schedule",                L"Add Schedule");
    AddTranslation("fr", "Add Schedule",                L"Ajouter une planification");
    AddTranslation("en", "Edit Schedule",               L"Edit Schedule");
    AddTranslation("fr", "Edit Schedule",               L"Modifier la planification");
    AddTranslation("en", "Failed to update scheduled event.", L"Failed to update scheduled event.");
    AddTranslation("fr", "Failed to update scheduled event.", L"Échec de la mise à jour de l'événement planifié.");
    AddTranslation("en", "Failed to add scheduled event.",    L"Failed to add scheduled event.");
    AddTranslation("fr", "Failed to add scheduled event.",    L"Échec de l'ajout de l'événement planifié.");

    // ============================================================
    // ui_bookmarks.cpp
    // ============================================================
    AddTranslation("en", "Bookmark removed",            L"Bookmark removed");
    AddTranslation("fr", "Bookmark removed",            L"Signet supprimé");
    AddTranslation("en", "Current file",                L"Current file");
    AddTranslation("fr", "Current file",                L"Fichier courant");
    AddTranslation("en", "All bookmarks",               L"All bookmarks");
    AddTranslation("fr", "All bookmarks",               L"Tous les signets");

    // ============================================================
    // ui_playlist.cpp
    // ============================================================
    AddTranslation("en", "removed",                     L"removed");
    AddTranslation("fr", "removed",                     L"retirés");
    AddTranslation("en", "files pasted",                L"files pasted");
    AddTranslation("fr", "files pasted",                L"fichiers collés");
    AddTranslation("en", "Playlist is empty.",          L"Playlist is empty.");
    AddTranslation("fr", "Playlist is empty.",          L"La liste de lecture est vide.");
    AddTranslation("en", "Save Playlist",               L"Save Playlist");
    AddTranslation("fr", "Save Playlist",               L"Enregistrer la liste");
    AddTranslation("en", "Playlist saved",              L"Playlist saved");
    AddTranslation("fr", "Playlist saved",              L"Liste enregistrée");
    AddTranslation("en", "Failed to save playlist.",    L"Failed to save playlist.");
    AddTranslation("fr", "Failed to save playlist.",    L"Échec de l'enregistrement de la liste.");
    AddTranslation("en", "Error",                       L"Error");
    AddTranslation("fr", "Error",                       L"Erreur");

    // ============================================================
    // effects.cpp
    // ============================================================
    AddTranslation("en", "3D Audio Error",              L"3D Audio Error");
    AddTranslation("fr", "3D Audio Error",              L"Erreur audio 3D");
    AddTranslation("en", "No parameters available",     L"No parameters available");
    AddTranslation("fr", "No parameters available",     L"Aucun paramètre disponible");
    AddTranslation("en", "Not available for live streams", L"Not available for live streams");
    AddTranslation("fr", "Not available for live streams", L"Indisponible pour les flux en direct");

    // ============================================================
    // settings.cpp — seek announcements
    // ============================================================
    AddTranslation("en", "1 chapter",                   L"1 chapter");
    AddTranslation("fr", "1 chapter",                   L"1 chapitre");
    AddTranslation("en", "1 second",                    L"1 second");
    AddTranslation("fr", "1 second",                    L"1 seconde");
    AddTranslation("en", "5 seconds",                   L"5 seconds");
    AddTranslation("fr", "5 seconds",                   L"5 secondes");
    AddTranslation("en", "10 seconds",                  L"10 seconds");
    AddTranslation("fr", "10 seconds",                  L"10 secondes");
    AddTranslation("en", "30 seconds",                  L"30 seconds");
    AddTranslation("fr", "30 seconds",                  L"30 secondes");
    AddTranslation("en", "1 minute",                    L"1 minute");
    AddTranslation("fr", "1 minute",                    L"1 minute");
    AddTranslation("en", "5 minutes",                   L"5 minutes");
    AddTranslation("fr", "5 minutes",                   L"5 minutes");
    AddTranslation("en", "10 minutes",                  L"10 minutes");
    AddTranslation("fr", "10 minutes",                  L"10 minutes");
    AddTranslation("en", "20 minutes",                  L"20 minutes");
    AddTranslation("fr", "20 minutes",                  L"20 minutes");
    AddTranslation("en", "30 minutes",                  L"30 minutes");
    AddTranslation("fr", "30 minutes",                  L"30 minutes");
    AddTranslation("en", "45 minutes",                  L"45 minutes");
    AddTranslation("fr", "45 minutes",                  L"45 minutes");
    AddTranslation("en", "60 minutes",                  L"60 minutes");
    AddTranslation("fr", "60 minutes",                  L"60 minutes");
    AddTranslation("en", "1 hour",                      L"1 hour");
    AddTranslation("fr", "1 hour",                      L"1 heure");
    AddTranslation("en", "1 track",                     L"1 track");
    AddTranslation("fr", "1 track",                     L"1 piste");
    AddTranslation("en", "5 tracks",                    L"5 tracks");
    AddTranslation("fr", "5 tracks",                    L"5 pistes");
    AddTranslation("en", "10 tracks",                   L"10 tracks");
    AddTranslation("fr", "10 tracks",                   L"10 pistes");

    // ============================================================
    // main.cpp
    // ============================================================
    AddTranslation("en", "Loaded Plugins",                          L"Loaded Plugins");
    AddTranslation("fr", "Loaded Plugins",                          L"Modules chargés");
    AddTranslation("en", "Could not open readme.txt. Make sure the docs folder is present alongside MediaAccess.exe.",
                         L"Could not open readme.txt. Make sure the docs folder is present alongside MediaAccess.exe.");
    AddTranslation("fr", "Could not open readme.txt. Make sure the docs folder is present alongside MediaAccess.exe.",
                         L"Impossible d'ouvrir readme.txt. Vérifiez que le dossier docs est présent à côté de MediaAccess.exe.");
    AddTranslation("en", "Readme",                                  L"Readme");
    AddTranslation("fr", "Readme",                                  L"Lisez-moi");
    AddTranslation("en", "Bookmark added",                          L"Bookmark added");
    AddTranslation("fr", "Bookmark added",                          L"Signet ajouté");
    AddTranslation("en", "Shuffle on",                              L"Shuffle on");
    AddTranslation("fr", "Shuffle on",                              L"Aléatoire activé");
    AddTranslation("en", "Shuffle off",                             L"Shuffle off");
    AddTranslation("fr", "Shuffle off",                             L"Aléatoire désactivé");
    AddTranslation("en", "Failed to register window class.",        L"Failed to register window class.");
    AddTranslation("fr", "Failed to register window class.",        L"Échec de l'enregistrement de la classe de fenêtre.");
    AddTranslation("en", "Failed to create window.",                L"Failed to create window.");
    AddTranslation("fr", "Failed to create window.",                L"Échec de la création de la fenêtre.");

    // ============================================================
    // Language combo entries
    // ============================================================
    AddTranslation("en", "Language",                                L"Language");
    AddTranslation("fr", "Language",                                L"Langue");

    // ============================================================
    // "Test YouTube playback" diagnostic dialog (new in v1.0.7)
    // ============================================================
    AddTranslation("en", "Test YouTube playback", L"Test YouTube playback");
    AddTranslation("fr", "Test YouTube playback", L"Tester la lecture YouTube");

    AddTranslation("en",
        "yt-dlp not found.\n\nMediaAccess looks for it in:\n  - %LOCALAPPDATA%\\MediaAccess\\yt-dlp.exe\n  - <install>\\lib\\yt-dlp.exe\n  - system PATH",
        L"yt-dlp not found.\n\nMediaAccess looks for it in:\n  - %LOCALAPPDATA%\\MediaAccess\\yt-dlp.exe\n  - <install>\\lib\\yt-dlp.exe\n  - system PATH");
    AddTranslation("fr",
        "yt-dlp not found.\n\nMediaAccess looks for it in:\n  - %LOCALAPPDATA%\\MediaAccess\\yt-dlp.exe\n  - <install>\\lib\\yt-dlp.exe\n  - system PATH",
        L"yt-dlp introuvable.\n\nMediaAccess le cherche dans :\n  - %LOCALAPPDATA%\\MediaAccess\\yt-dlp.exe\n  - <installation>\\lib\\yt-dlp.exe\n  - le PATH système");

    AddTranslation("en", "yt-dlp is working.\n\nPath: ", L"yt-dlp is working.\n\nPath: ");
    AddTranslation("fr", "yt-dlp is working.\n\nPath: ", L"yt-dlp fonctionne.\n\nChemin : ");

    AddTranslation("en", "Version: ", L"Version: ");
    AddTranslation("fr", "Version: ", L"Version : ");

    AddTranslation("en", "libmpv (for video / fallback): ", L"libmpv (vidéo / repli) : ");
    AddTranslation("fr", "libmpv (for video / fallback): ", L"libmpv (vidéo / repli) : ");

    AddTranslation("en", "available", L"available");
    AddTranslation("fr", "available", L"disponible");

    AddTranslation("en", "not available", L"not available");
    AddTranslation("fr", "not available", L"non disponible");

    AddTranslation("en", "If a YouTube video still fails, check the log file at:\n",
                         L"If a YouTube video still fails, check the log file at:\n");
    AddTranslation("fr", "If a YouTube video still fails, check the log file at:\n",
                         L"Si une vidéo YouTube échoue encore, consultez le fichier journal :\n");

    AddTranslation("en", "yt-dlp is present but failed to run.\n\nPath: ",
                         L"yt-dlp is present but failed to run.\n\nPath: ");
    AddTranslation("fr", "yt-dlp is present but failed to run.\n\nPath: ",
                         L"yt-dlp est présent mais ne s'est pas exécuté.\n\nChemin : ");
}
