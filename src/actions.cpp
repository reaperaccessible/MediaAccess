// =============================================================================
// actions.cpp — action registry implementation
// =============================================================================

#include "mediaaccess/actions.h"
#include "mediaaccess/translations.h"
#include "resource.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace mediaaccess {

// =============================================================================
// The catalog. Order matters — it controls list ordering in the Actions dialog.
// Stable string IDs MUST NEVER change after release.
// =============================================================================
//
// Default-shortcut convention: this column is the USA keymap default.
// FR-CA and FR-FR keymaps ship as separate text files in the KeyMaps/
// folder and can override any of these defaults.
//
// vk==0 means the action has no default shortcut on USA.
// -----------------------------------------------------------------------------

static const Action g_actions[] = {

    // ========================================================================
    // CATEGORY: Main — file ops
    // ========================================================================
    { "FILE_OPEN",                IDM_FILE_OPEN,              ActionCategory::Main,
      "Open file...",                 "Ouvrir un fichier...",
      { 'O', true,  false, false } },
    { "FILE_ADD_FOLDER",          IDM_FILE_ADD_FOLDER,        ActionCategory::Main,
      "Add folder...",                "Ajouter un dossier...",
      { 'O', true,  true,  false } },
    { "FILE_PLAYLIST",            IDM_FILE_PLAYLIST,          ActionCategory::Main,
      "Playlist manager...",          "Gestionnaire de listes de lecture...",
      { 'P', true,  false, false } },
    { "FILE_OPEN_URL",            IDM_FILE_OPEN_URL,          ActionCategory::Main,
      "Open URL...",                  "Ouvrir une URL...",
      { 'U', true,  false, false } },
    { "FILE_PASTE",               IDM_FILE_PASTE,             ActionCategory::Main,
      "Paste media from clipboard",   "Coller un média depuis le presse-papiers",
      { 'V', true,  false, false } },
    { "FILE_YOUTUBE",             IDM_FILE_YOUTUBE,           ActionCategory::Main,
      "YouTube search...",            "Recherche YouTube...",
      { 'Y', true,  false, false } },
    { "FILE_RADIO",               IDM_FILE_RADIO,             ActionCategory::Main,
      "Radio...",                     "Radio...",
      { 'R', true,  false, false } },
    { "FILE_ADD_TO_FAVORITES",    IDM_FILE_ADD_TO_FAVORITES,  ActionCategory::Main,
      "Add stream to favorites",      "Ajouter le flux aux favoris",
      { 'D', true,  false, false } },
    { "FILE_PODCAST",             IDM_FILE_PODCAST,           ActionCategory::Main,
      "Podcasts...",                  "Podcasts...",
      { 'P', true,  true,  false } },
    { "FILE_SCHEDULE",            IDM_FILE_SCHEDULE,          ActionCategory::Main,
      "Schedule...",                  "Planificateur...",
      { 'S', true,  false, false } },
    { "FILE_HIDE_TRAY",           IDM_FILE_HIDE_TRAY,         ActionCategory::Main,
      "Hide to tray",                 "Réduire dans la zone de notification",
      { 'H', true,  false, false } },
    { "VIEW_SONG_HISTORY",        IDM_VIEW_SONG_HISTORY,      ActionCategory::Main,
      "Song history",                 "Historique des chansons",
      { 'H', true,  true,  false } },
    { "TOOLS_OPTIONS",            IDM_TOOLS_OPTIONS,          ActionCategory::Main,
      "Options...",                   "Options...",
      { VK_OEM_COMMA, true,  false, false } },

    // ========================================================================
    // CATEGORY: Main — playback transport
    // ========================================================================
    { "PLAYER_PLAY_PAUSE",        IDM_PLAY_PLAYPAUSE,         ActionCategory::Main,
      "Play / Pause",                 "Lecture / Pause",
      { VK_SPACE, false, false, false } },
    { "PLAYER_PLAY",              IDM_PLAY_PLAY,              ActionCategory::Main,
      "Play",                         "Lecture",
      { 'X', false, false, false } },
    { "PLAYER_PAUSE",             IDM_PLAY_PAUSE,             ActionCategory::Main,
      "Pause",                        "Pause",
      { 'C', false, false, false } },
    { "PLAYER_STOP",              IDM_PLAY_STOP,              ActionCategory::Main,
      "Stop",                         "Arrêter",
      { 'V', false, false, false } },
    { "PLAYER_PREV",              IDM_PLAY_PREV,              ActionCategory::Main,
      "Previous track",               "Piste précédente",
      { 'Z', false, false, false } },
    { "PLAYER_NEXT",              IDM_PLAY_NEXT,              ActionCategory::Main,
      "Next track",                   "Piste suivante",
      { 'B', false, false, false } },
    { "PLAYER_SHUFFLE",           IDM_PLAY_SHUFFLE,           ActionCategory::Main,
      "Toggle shuffle",               "Basculer la lecture aléatoire",
      { 'H', false, false, false } },
    { "PLAYER_REPEAT_TOGGLE",     IDM_PLAY_REPEAT_TOGGLE,     ActionCategory::Main,
      "Cycle repeat mode",            "Changer le mode répétition",
      { 'E', false, false, false } },
    { "PLAYER_MUTE",              IDM_PLAY_MUTE,              ActionCategory::Main,
      "Toggle mute",                  "Basculer le mode muet",
      { 'U', false, false, false } },
    { "PLAYER_NOW_PLAYING",       IDM_PLAY_NOWPLAYING,        ActionCategory::Main,
      "Speak now playing",            "Annoncer ce qui joue",
      {} },

    // ========================================================================
    // CATEGORY: Main — seeking and navigation
    // ========================================================================
    { "SEEK_BACK",                IDM_PLAY_SEEKBACK,          ActionCategory::Main,
      "Seek backward",                "Reculer dans la piste",
      { VK_LEFT, false, false, false } },
    { "SEEK_FWD",                 IDM_PLAY_SEEKFWD,           ActionCategory::Main,
      "Seek forward",                 "Avancer dans la piste",
      { VK_RIGHT, false, false, false } },
    { "SEEK_BEGINNING",           IDM_PLAY_BEGINNING,         ActionCategory::Main,
      "Go to beginning",              "Aller au début",
      { VK_HOME, false, false, false } },
    { "SEEK_JUMP_TO_TIME",        IDM_PLAY_JUMPTOTIME,        ActionCategory::Main,
      "Jump to time...",              "Aller à une position...",
      { 'J', false, false, false } },
    { "SEEK_NEAR_END",            IDM_PLAY_NEAR_END,          ActionCategory::Main,
      "Jump 30 seconds before end",   "Aller à 30 secondes avant la fin",
      { VK_END, false, true,  false } },
    { "SEEK_UNIT_DECREASE",       IDM_SEEK_DECREASE,          ActionCategory::Main,
      "Smaller seek unit",            "Unité de saut plus petite",
      { VK_OEM_COMMA, false, false, false } },
    { "SEEK_UNIT_INCREASE",       IDM_SEEK_INCREASE,          ActionCategory::Main,
      "Larger seek unit",             "Unité de saut plus grande",
      { VK_OEM_PERIOD, false, false, false } },
    { "SEEK_UNIT_SPEAK",          IDM_SPEAK_SEEK,             ActionCategory::Main,
      "Speak current seek unit",      "Annoncer l'unité de saut actuelle",
      {} },

    // ========================================================================
    // CATEGORY: Main — volume
    // ========================================================================
    { "VOLUME_UP",                IDM_PLAY_VOLUP,             ActionCategory::Main,
      "Volume up",                    "Volume plus haut",
      { VK_UP,   true,  false, false } },
    { "VOLUME_DOWN",              IDM_PLAY_VOLDOWN,           ActionCategory::Main,
      "Volume down",                  "Volume plus bas",
      { VK_DOWN, true,  false, false } },

    // ========================================================================
    // CATEGORY: Main — speech feedback
    // ========================================================================
    { "SPEAK_ELAPSED",            IDM_PLAY_ELAPSED,           ActionCategory::Main,
      "Speak elapsed time",           "Annoncer le temps écoulé",
      { 'E', true,  true,  false } },
    { "SPEAK_REMAINING",          IDM_PLAY_REMAINING,         ActionCategory::Main,
      "Speak remaining time",         "Annoncer le temps restant",
      { 'R', true,  true,  false } },
    { "SPEAK_TOTAL",              IDM_PLAY_TOTAL,             ActionCategory::Main,
      "Speak total duration",         "Annoncer la durée totale",
      { 'T', true,  true,  false } },

    // ========================================================================
    // CATEGORY: Main — effects (selection + value control)
    // ========================================================================
    { "EFFECT_PREV",              IDM_EFFECT_PREV,            ActionCategory::Main,
      "Previous effect parameter",    "Paramètre d'effet précédent",
      { VK_OEM_4, false, false, false } }, // physical '['
    { "EFFECT_NEXT",              IDM_EFFECT_NEXT,            ActionCategory::Main,
      "Next effect parameter",        "Paramètre d'effet suivant",
      { VK_OEM_6, false, false, false } }, // physical ']'
    { "EFFECT_UP",                IDM_EFFECT_UP,              ActionCategory::Main,
      "Increase current parameter",   "Augmenter le paramètre actuel",
      { VK_UP,   false, false, false } },
    { "EFFECT_DOWN",              IDM_EFFECT_DOWN,            ActionCategory::Main,
      "Decrease current parameter",   "Diminuer le paramètre actuel",
      { VK_DOWN, false, false, false } },
    { "EFFECT_RESET",             IDM_EFFECT_RESET,           ActionCategory::Main,
      "Reset parameter to default",   "Réinitialiser le paramètre",
      { VK_BACK, false, false, false } },
    { "EFFECT_MIN",               IDM_EFFECT_MIN,             ActionCategory::Main,
      "Set parameter to minimum",     "Régler le paramètre au minimum",
      { VK_HOME, true,  false, false } },
    { "EFFECT_MAX",               IDM_EFFECT_MAX,             ActionCategory::Main,
      "Set parameter to maximum",     "Régler le paramètre au maximum",
      { VK_END,  true,  false, false } },
    { "EFFECT_PRESETS",           IDM_EFFECT_PRESETS,         ActionCategory::Main,
      "Effect presets menu",          "Menu des préréglages d'effets",
      { 'P', false, false, false } },

    // ========================================================================
    // CATEGORY: Main — effect toggles
    // ========================================================================
    { "TOGGLE_VOLUME",            IDM_TOGGLE_VOLUME,          ActionCategory::Main,
      "Toggle Volume effect",         "Basculer l'effet Volume",
      { '1', true,  false, false } },
    { "TOGGLE_PITCH",             IDM_TOGGLE_PITCH,           ActionCategory::Main,
      "Toggle Pitch effect",          "Basculer l'effet Tonalité",
      { '2', true,  false, false } },
    { "TOGGLE_TEMPO",             IDM_TOGGLE_TEMPO,           ActionCategory::Main,
      "Toggle Tempo effect",          "Basculer l'effet Tempo",
      { '3', true,  false, false } },
    { "TOGGLE_RATE",              IDM_TOGGLE_RATE,            ActionCategory::Main,
      "Toggle Rate effect",           "Basculer l'effet Vitesse",
      { '4', true,  false, false } },
    { "TOGGLE_REVERB",            IDM_TOGGLE_REVERB,          ActionCategory::Main,
      "Cycle Reverb algorithm",       "Changer l'algorithme de Réverbération",
      { '5', true,  false, false } },
    { "TOGGLE_ECHO",              IDM_TOGGLE_ECHO,            ActionCategory::Main,
      "Toggle Echo effect",           "Basculer l'effet Écho",
      { '6', true,  false, false } },
    { "TOGGLE_EQ",                IDM_TOGGLE_EQ,              ActionCategory::Main,
      "Toggle Equalizer",             "Basculer l'Égaliseur",
      { '7', true,  false, false } },
    { "TOGGLE_COMPRESSOR",        IDM_TOGGLE_COMPRESSOR,      ActionCategory::Main,
      "Toggle Compressor",            "Basculer le Compresseur",
      { '8', true,  false, false } },
    { "TOGGLE_STEREO_WIDTH",      IDM_TOGGLE_STEREOWIDTH,     ActionCategory::Main,
      "Toggle Stereo Width",          "Basculer la Largeur stéréo",
      { '9', true,  false, false } },
    { "TOGGLE_CENTER_CANCEL",     IDM_TOGGLE_CENTERCANCEL,    ActionCategory::Main,
      "Toggle Center Cancel",         "Basculer l'Annulation centrale",
      { '0', true,  false, false } },
    { "TOGGLE_CONVOLUTION",       IDM_TOGGLE_CONVOLUTION,     ActionCategory::Main,
      "Toggle Convolution Reverb",    "Basculer la Réverbération à convolution",
      { VK_OEM_MINUS, true,  false, false } },
    { "TOGGLE_SPATIAL",           IDM_TOGGLE_SPATIAL,         ActionCategory::Main,
      "Toggle 3D Audio",              "Basculer l'Audio 3D",
      { VK_OEM_PLUS,  true,  false, false } },

    // ========================================================================
    // CATEGORY: Main — tag reading (1-0)
    // ========================================================================
    { "READ_TAG_TITLE",           IDM_READ_TAG_TITLE,         ActionCategory::Main,
      "Speak title tag",              "Annoncer le titre",
      { '1', false, false, false } },
    { "READ_TAG_ARTIST",          IDM_READ_TAG_ARTIST,        ActionCategory::Main,
      "Speak artist tag",             "Annoncer l'artiste",
      { '2', false, false, false } },
    { "READ_TAG_ALBUM",           IDM_READ_TAG_ALBUM,         ActionCategory::Main,
      "Speak album tag",              "Annoncer l'album",
      { '3', false, false, false } },
    { "READ_TAG_YEAR",            IDM_READ_TAG_YEAR,          ActionCategory::Main,
      "Speak year tag",               "Annoncer l'année",
      { '4', false, false, false } },
    { "READ_TAG_TRACK",           IDM_READ_TAG_TRACK,         ActionCategory::Main,
      "Speak track number",           "Annoncer le numéro de piste",
      { '5', false, false, false } },
    { "READ_TAG_GENRE",           IDM_READ_TAG_GENRE,         ActionCategory::Main,
      "Speak genre tag",              "Annoncer le genre",
      { '6', false, false, false } },
    { "READ_TAG_COMMENT",         IDM_READ_TAG_COMMENT,       ActionCategory::Main,
      "Speak comment tag",            "Annoncer le commentaire",
      { '7', false, false, false } },
    { "READ_TAG_BITRATE",         IDM_READ_TAG_BITRATE,       ActionCategory::Main,
      "Speak bitrate",                "Annoncer le débit",
      { '8', false, false, false } },
    { "READ_TAG_DURATION",        IDM_READ_TAG_DURATION,      ActionCategory::Main,
      "Speak duration",               "Annoncer la durée",
      { '9', false, false, false } },
    { "READ_TAG_FILENAME",        IDM_READ_TAG_FILENAME,      ActionCategory::Main,
      "Speak filename",               "Annoncer le nom de fichier",
      { '0', false, false, false } },

    // ========================================================================
    // CATEGORY: Main — tag display in dialog (Shift+1-0)
    // ========================================================================
    { "VIEW_TAG_TITLE",            IDM_VIEW_TAG_TITLE,        ActionCategory::Main,
      "Show title in window",         "Afficher le titre dans une fenêtre",
      { '1', false, true,  false } },
    { "VIEW_TAG_ARTIST",           IDM_VIEW_TAG_ARTIST,       ActionCategory::Main,
      "Show artist in window",        "Afficher l'artiste dans une fenêtre",
      { '2', false, true,  false } },
    { "VIEW_TAG_ALBUM",            IDM_VIEW_TAG_ALBUM,        ActionCategory::Main,
      "Show album in window",         "Afficher l'album dans une fenêtre",
      { '3', false, true,  false } },
    { "VIEW_TAG_YEAR",             IDM_VIEW_TAG_YEAR,         ActionCategory::Main,
      "Show year in window",          "Afficher l'année dans une fenêtre",
      { '4', false, true,  false } },
    { "VIEW_TAG_TRACK",            IDM_VIEW_TAG_TRACK,        ActionCategory::Main,
      "Show track number in window",  "Afficher le numéro de piste dans une fenêtre",
      { '5', false, true,  false } },
    { "VIEW_TAG_GENRE",            IDM_VIEW_TAG_GENRE,        ActionCategory::Main,
      "Show genre in window",         "Afficher le genre dans une fenêtre",
      { '6', false, true,  false } },
    { "VIEW_TAG_COMMENT",          IDM_VIEW_TAG_COMMENT,      ActionCategory::Main,
      "Show comment in window",       "Afficher le commentaire dans une fenêtre",
      { '7', false, true,  false } },
    { "VIEW_TAG_BITRATE",          IDM_VIEW_TAG_BITRATE,      ActionCategory::Main,
      "Show bitrate in window",       "Afficher le débit dans une fenêtre",
      { '8', false, true,  false } },
    { "VIEW_TAG_DURATION",         IDM_VIEW_TAG_DURATION,     ActionCategory::Main,
      "Show duration in window",      "Afficher la durée dans une fenêtre",
      { '9', false, true,  false } },
    { "VIEW_TAG_FILENAME",         IDM_VIEW_TAG_FILENAME,     ActionCategory::Main,
      "Show filename in window",      "Afficher le nom de fichier dans une fenêtre",
      { '0', false, true,  false } },

    // ========================================================================
    // CATEGORY: Main — bookmarks, recording, audio device
    // ========================================================================
    { "BOOKMARK_ADD",             IDM_BOOKMARK_ADD,           ActionCategory::Main,
      "Add bookmark at current position", "Ajouter un signet à la position actuelle",
      { 'M', false, false, false } },
    { "BOOKMARK_LIST",            IDM_BOOKMARK_LIST,          ActionCategory::Main,
      "Bookmarks manager",            "Gestionnaire de signets",
      { 'M', true,  false, false } },
    { "RECORD_TOGGLE",            IDM_RECORD_TOGGLE,          ActionCategory::Main,
      "Toggle recording",             "Basculer l'enregistrement",
      { 'R', false, false, false } },
    { "AUDIO_DEVICE_MENU",        IDM_SHOW_AUDIO_DEVICES,     ActionCategory::Main,
      "Audio device menu",            "Menu de périphériques audio",
      { 'A', false, false, false } },

    // ========================================================================
    // CATEGORY: Main — video
    // ========================================================================
    { "VIDEO_FULLSCREEN",         IDM_VIDEO_FULLSCREEN,       ActionCategory::Main,
      "Toggle fullscreen",            "Basculer le plein écran",
      { VK_F11, false, false, false } },
    { "VIDEO_SUB_CYCLE",          IDM_VIDEO_SUB_CYCLE,        ActionCategory::Main,
      "Cycle subtitles",              "Changer les sous-titres",
      { 'T', true,  true,  false } },
    { "VIDEO_AUDIO_CYCLE",        IDM_VIDEO_AUDIO_CYCLE,      ActionCategory::Main,
      "Cycle audio tracks",           "Changer la piste audio",
      { 'A', true,  true,  false } },
    { "VIDEO_SCREENSHOT",         IDM_VIDEO_SCREENSHOT,       ActionCategory::Main,
      "Take screenshot",              "Capture d'écran",
      { 'S', true,  true,  false } },

    // ========================================================================
    // CATEGORY: Main — help and Actions window
    // ========================================================================
    { "HELP_MANUAL",              IDM_HELP_MANUAL,            ActionCategory::Main,
      "Open manual",                  "Ouvrir le manuel",
      { VK_F1,  false, false, false } },
    { "KEYBOARD_HELP_TOGGLE",     IDM_KEYBOARD_HELP_TOGGLE,   ActionCategory::Main,
      "Toggle keyboard help mode",    "Basculer le mode aide clavier",
      { VK_F12, false, false, false } },
    // The Actions window itself — IDM defined in resource.h.
    // (Forward-referenced; resource.h adds IDM_TOOLS_ACTIONS.)
    { "TOOLS_ACTIONS",            IDM_TOOLS_ACTIONS,          ActionCategory::Main,
      "Actions...",                   "Actions...",
      { VK_F4, false, false, false } },

    // ========================================================================
    // CATEGORY: Global — system-wide hotkeys (active without window focus)
    // ========================================================================
    // These are user-defined by default (g_hotkeys vector). The entries here
    // describe which actions CAN be assigned a global hotkey. Defaults are
    // empty — users assign their own.
    { "GLOBAL_PLAY_PAUSE",        IDM_PLAY_PLAYPAUSE,         ActionCategory::Global,
      "Play / Pause (global)",        "Lecture / Pause (global)", {} },
    { "GLOBAL_STOP",              IDM_PLAY_STOP,              ActionCategory::Global,
      "Stop (global)",                "Arrêter (global)", {} },
    { "GLOBAL_PREV",              IDM_PLAY_PREV,              ActionCategory::Global,
      "Previous track (global)",      "Piste précédente (global)", {} },
    { "GLOBAL_NEXT",              IDM_PLAY_NEXT,              ActionCategory::Global,
      "Next track (global)",          "Piste suivante (global)", {} },
    { "GLOBAL_VOLUP",             IDM_PLAY_VOLUP,             ActionCategory::Global,
      "Volume up (global)",           "Volume plus haut (global)", {} },
    { "GLOBAL_VOLDOWN",           IDM_PLAY_VOLDOWN,           ActionCategory::Global,
      "Volume down (global)",         "Volume plus bas (global)", {} },
    { "GLOBAL_MUTE",              IDM_PLAY_MUTE,              ActionCategory::Global,
      "Toggle mute (global)",         "Basculer le mode muet (global)", {} },
    { "GLOBAL_TOGGLE_WINDOW",     IDM_TOGGLE_WINDOW,          ActionCategory::Global,
      "Show / hide MediaAccess window","Afficher / cacher la fenêtre MediaAccess", {} },
    { "GLOBAL_RECORD_TOGGLE",     IDM_RECORD_TOGGLE,          ActionCategory::Global,
      "Toggle recording (global)",    "Basculer l'enregistrement (global)", {} },
    { "GLOBAL_SHUFFLE",           IDM_PLAY_SHUFFLE,           ActionCategory::Global,
      "Toggle shuffle (global)",      "Basculer la lecture aléatoire (global)", {} },
    { "GLOBAL_YOUTUBE",           IDM_FILE_YOUTUBE,           ActionCategory::Global,
      "YouTube search (global)",      "Recherche YouTube (global)", {} },

    // ========================================================================
    // CATEGORY: Books — DAISY / EPUB reader (v1.49 Phase 1)
    // ========================================================================
    { "BOOK_OPEN",                IDM_FILE_OPEN_BOOK,         ActionCategory::Books,
      "Open book...",                 "Ouvrir un livre...",
      { 'B', true, false, false } },           // Ctrl+B
    { "BOOK_LIBRARY",             IDM_TOOLS_BOOK_LIBRARY,     ActionCategory::Books,
      "Book library...",              "Bibliothèque de livres...",
      { 'B', true, true,  false } },           // Ctrl+Shift+B
    { "BOOK_NAV_LEVEL_UP",        IDM_BOOK_NAV_LEVEL_UP,      ActionCategory::Books,
      "Next navigation level",        "Niveau de navigation suivant",
      { VK_UP, false, true, false } },        // Shift+Up
    { "BOOK_NAV_LEVEL_DOWN",      IDM_BOOK_NAV_LEVEL_DOWN,    ActionCategory::Books,
      "Previous navigation level",    "Niveau de navigation précédent",
      { VK_DOWN, false, true, false } },      // Shift+Down
    { "BOOK_NAV_FORWARD",         IDM_BOOK_NAV_FORWARD,       ActionCategory::Books,
      "Jump to next nav point",       "Aller au point de navigation suivant",
      { VK_RIGHT, false, true, false } },     // Shift+Right
    { "BOOK_NAV_BACKWARD",        IDM_BOOK_NAV_BACKWARD,      ActionCategory::Books,
      "Jump to previous nav point",   "Aller au point de navigation précédent",
      { VK_LEFT, false, true, false } },      // Shift+Left
    { "BOOK_ANNOUNCE_LOCATION",   IDM_BOOK_ANNOUNCE_LOCATION, ActionCategory::Books,
      "Announce current location",    "Annoncer la position actuelle",
      { 'L', false, true, false } },          // Shift+L
    { "BOOK_ADD_BOOKMARK_NOTE",   IDM_BOOK_ADD_BOOKMARK_WITH_NOTE, ActionCategory::Books,
      "Add bookmark with note",       "Ajouter un signet avec note",
      { 'B', false, true, false } },          // Shift+B
    { "BOOK_GO_TO_PAGE",          IDM_BOOK_GO_TO_PAGE,        ActionCategory::Books,
      "Go to page...",                "Aller à la page...",
      { 'G', false, false, false } },         // G
    { "BOOK_TOGGLE_TEXT_WINDOW",  IDM_BOOK_TOGGLE_TEXT_WINDOW, ActionCategory::Books,
      "Show / hide text window",      "Afficher / cacher la fenêtre texte",
      { 'T', true, false, false } },          // Ctrl+T
    { "BOOK_SEARCH",              IDM_BOOK_SEARCH,            ActionCategory::Books,
      "Search in book...",            "Rechercher dans le livre...",
      { VK_F3, false, false, false } },       // F3
    { "BOOK_BOOKMARK_LIST",       IDM_BOOK_BOOKMARK_LIST,     ActionCategory::Books,
      "Bookmarks...",                 "Signets...",
      { 'M', false, false, false } },         // M — list bookmarks

    // ========================================================================
    // CATEGORY: Radio — placeholder for future actions
    // CATEGORY: YouTube — placeholder for future actions
    // (No entries yet; categories appear empty in the Actions dialog until
    // the Radio/YouTube dialogs expose their own action surface.)
    // ========================================================================
};

static const int kActionCount = sizeof(g_actions) / sizeof(g_actions[0]);

// =============================================================================
// Registry accessors
// =============================================================================

int ActionCount() { return kActionCount; }

const Action* ActionAt(int index)
{
    if (index < 0 || index >= kActionCount) return nullptr;
    return &g_actions[index];
}

const Action* ActionByStringId(const std::string& stringId)
{
    for (int i = 0; i < kActionCount; ++i) {
        if (stringId == g_actions[i].stringId) return &g_actions[i];
    }
    return nullptr;
}

const Action* ActionByCommandId(int commandId)
{
    for (int i = 0; i < kActionCount; ++i) {
        if (g_actions[i].commandId == commandId) return &g_actions[i];
    }
    return nullptr;
}

std::vector<const Action*> ActionsInCategory(ActionCategory cat)
{
    std::vector<const Action*> out;
    out.reserve(64);
    for (int i = 0; i < kActionCount; ++i) {
        if (g_actions[i].category == cat) out.push_back(&g_actions[i]);
    }
    return out;
}

static bool IsFrench()
{
    const char* lang = GetCurrentLanguage();
    return lang && lang[0] == 'f';
}

std::string ActionDisplayName(const Action& a)
{
    return IsFrench() ? a.nameFr : a.nameEn;
}

std::string CategoryDisplayName(ActionCategory cat)
{
    bool fr = IsFrench();
    switch (cat) {
        case ActionCategory::Main:    return fr ? "Principale" : "Main";
        case ActionCategory::Radio:   return fr ? "Radio"      : "Radio";
        case ActionCategory::YouTube: return fr ? "YouTube"    : "YouTube";
        case ActionCategory::Global:  return fr ? "Globale"    : "Global";
        case ActionCategory::Books:   return fr ? "Livres"     : "Books";
        default:                      return "";
    }
}

// =============================================================================
// Shortcut formatting helpers
// =============================================================================

static std::string KeyNameDisplay(UINT vk)
{
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    bool ext = (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
                vk == VK_HOME || vk == VK_END   || vk == VK_PRIOR || vk == VK_NEXT ||
                vk == VK_INSERT || vk == VK_DELETE);
    LONG lparam = (LONG)(scan << 16);
    if (ext) lparam |= (1L << 24);
    wchar_t buf[64] = {0};
    int n = GetKeyNameTextW(lparam, buf, 64);
    if (n <= 0) {
        char tmp[16];
        std::snprintf(tmp, sizeof(tmp), "VK0x%02X", vk);
        return tmp;
    }
    int sz = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (sz <= 1) return "";
    std::string out(sz - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, &out[0], sz, nullptr, nullptr);
    return out;
}

std::string ShortcutToDisplay(const Shortcut& s)
{
    if (!s.valid()) return "";
    std::string out;
    if (s.ctrl)  out += Ts("KEY_MOD_CTRL")  + "+";
    if (s.shift) out += Ts("KEY_MOD_SHIFT") + "+";
    if (s.alt)   out += Ts("KEY_MOD_ALT")   + "+";
    out += KeyNameDisplay(s.vk);
    return out;
}

// =============================================================================
// Keymap file text format
//
// Canonical form:  [Ctrl+][Shift+][Alt+]<Key>
// where <Key> is one of:
//   - "A".."Z" "0".."9"
//   - Named key: Space, Backspace, Tab, Enter, Escape, Left, Right, Up, Down,
//     Home, End, PageUp, PageDown, Insert, Delete, F1..F24, NumPad0..NumPad9,
//     Multiply, Add, Subtract, Decimal, Divide,
//     OEMComma, OEMPeriod, OEMMinus, OEMPlus, OEMSemicolon, OEMQuotes,
//     OEMOpenBracket, OEMCloseBracket, OEMBackslash, OEMTilde, OEMSlash
//   - "VK0x<hex>" as last-resort raw escape
// Examples: "Ctrl+Shift+O", "Space", "F12", "Ctrl+OEMComma", "VK0xBE"
// =============================================================================

struct NamedKey { UINT vk; const char* name; };
static const NamedKey kNamedKeys[] = {
    { VK_SPACE,     "Space" },     { VK_BACK,      "Backspace" }, { VK_TAB,    "Tab" },
    { VK_RETURN,    "Enter" },     { VK_ESCAPE,    "Escape" },
    { VK_LEFT,      "Left" },      { VK_RIGHT,     "Right" },     { VK_UP,     "Up" },
    { VK_DOWN,      "Down" },
    { VK_HOME,      "Home" },      { VK_END,       "End" },
    { VK_PRIOR,     "PageUp" },    { VK_NEXT,      "PageDown" },
    { VK_INSERT,    "Insert" },    { VK_DELETE,    "Delete" },
    { VK_F1,  "F1" }, { VK_F2,  "F2" }, { VK_F3,  "F3" }, { VK_F4,  "F4" },
    { VK_F5,  "F5" }, { VK_F6,  "F6" }, { VK_F7,  "F7" }, { VK_F8,  "F8" },
    { VK_F9,  "F9" }, { VK_F10, "F10"}, { VK_F11, "F11"}, { VK_F12, "F12"},
    { VK_F13, "F13"}, { VK_F14, "F14"}, { VK_F15, "F15"}, { VK_F16, "F16"},
    { VK_F17, "F17"}, { VK_F18, "F18"}, { VK_F19, "F19"}, { VK_F20, "F20"},
    { VK_NUMPAD0, "NumPad0" }, { VK_NUMPAD1, "NumPad1" }, { VK_NUMPAD2, "NumPad2" },
    { VK_NUMPAD3, "NumPad3" }, { VK_NUMPAD4, "NumPad4" }, { VK_NUMPAD5, "NumPad5" },
    { VK_NUMPAD6, "NumPad6" }, { VK_NUMPAD7, "NumPad7" }, { VK_NUMPAD8, "NumPad8" },
    { VK_NUMPAD9, "NumPad9" },
    { VK_MULTIPLY, "Multiply" }, { VK_ADD, "Add" }, { VK_SUBTRACT, "Subtract" },
    { VK_DECIMAL,  "Decimal" },  { VK_DIVIDE, "Divide" },
    { VK_OEM_COMMA,    "OEMComma" },    { VK_OEM_PERIOD,   "OEMPeriod" },
    { VK_OEM_MINUS,    "OEMMinus" },    { VK_OEM_PLUS,     "OEMPlus" },
    { VK_OEM_1,        "OEMSemicolon" },{ VK_OEM_2,        "OEMSlash" },
    { VK_OEM_3,        "OEMTilde" },    { VK_OEM_4,        "OEMOpenBracket" },
    { VK_OEM_5,        "OEMBackslash" },{ VK_OEM_6,        "OEMCloseBracket" },
    { VK_OEM_7,        "OEMQuotes" },
};
static const int kNamedKeyCount = sizeof(kNamedKeys) / sizeof(kNamedKeys[0]);

static const char* NameForVK(UINT vk)
{
    for (int i = 0; i < kNamedKeyCount; ++i)
        if (kNamedKeys[i].vk == vk) return kNamedKeys[i].name;
    return nullptr;
}

static UINT VKForName(const std::string& name)
{
    for (int i = 0; i < kNamedKeyCount; ++i)
        if (name == kNamedKeys[i].name) return kNamedKeys[i].vk;
    return 0;
}

std::string ShortcutToKeymapText(const Shortcut& s)
{
    if (!s.valid()) return "";
    std::string out;
    if (s.ctrl)  out += "Ctrl+";
    if (s.shift) out += "Shift+";
    if (s.alt)   out += "Alt+";
    if (const char* n = NameForVK(s.vk)) {
        out += n;
    } else if ((s.vk >= 'A' && s.vk <= 'Z') || (s.vk >= '0' && s.vk <= '9')) {
        out += (char)s.vk;
    } else {
        char tmp[16];
        std::snprintf(tmp, sizeof(tmp), "VK0x%02X", s.vk);
        out += tmp;
    }
    return out;
}

Shortcut ShortcutFromKeymapText(const std::string& text)
{
    Shortcut s;
    std::string rest = text;

    // Trim leading/trailing whitespace.
    auto trim = [](std::string& x) {
        while (!x.empty() && std::isspace((unsigned char)x.front())) x.erase(x.begin());
        while (!x.empty() && std::isspace((unsigned char)x.back()))  x.pop_back();
    };
    trim(rest);
    if (rest.empty()) return s;

    auto consume = [&](const char* prefix) -> bool {
        size_t plen = std::strlen(prefix);
        if (rest.size() >= plen) {
            // Case-insensitive compare for the prefix only.
            bool match = true;
            for (size_t i = 0; i < plen; ++i) {
                char a = (char)std::tolower((unsigned char)rest[i]);
                char b = (char)std::tolower((unsigned char)prefix[i]);
                if (a != b) { match = false; break; }
            }
            if (match) { rest.erase(0, plen); return true; }
        }
        return false;
    };

    // Modifier prefixes may appear in any order.
    bool changed = true;
    while (changed) {
        changed = false;
        if (consume("Ctrl+"))  { s.ctrl  = true; changed = true; continue; }
        if (consume("Shift+")) { s.shift = true; changed = true; continue; }
        if (consume("Alt+"))   { s.alt   = true; changed = true; continue; }
    }

    if (rest.empty()) return Shortcut{};

    // Raw VK escape: "VK0x<hex>"
    if (rest.size() >= 5 && (rest[0] == 'V' || rest[0] == 'v') &&
        (rest[1] == 'K' || rest[1] == 'k') && rest[2] == '0' &&
        (rest[3] == 'x' || rest[3] == 'X')) {
        unsigned int hex = 0;
        if (std::sscanf(rest.c_str() + 4, "%x", &hex) == 1) {
            s.vk = (UINT)hex;
            return s;
        }
        return Shortcut{};
    }

    // Single letter or digit
    if (rest.size() == 1) {
        char c = rest[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            s.vk = (UINT)c;
            return s;
        }
    }

    // Named key
    UINT vk = VKForName(rest);
    if (vk != 0) { s.vk = vk; return s; }

    // Unknown — return invalid.
    return Shortcut{};
}

} // namespace mediaaccess
