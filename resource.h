#pragma once

// ===== Video playback IDs =====
#define IDM_VIDEO_FULLSCREEN    1200
#define IDM_VIDEO_SUB_CYCLE     1201
#define IDM_VIDEO_SUB_LOAD      1202
#define IDM_VIDEO_SUB_OFF       1203
#define IDM_VIDEO_AUDIO_CYCLE   1240
#define IDM_VIDEO_ASPECT        1270
#define IDM_VIDEO_SCREENSHOT    1280
#define IDT_CURSOR_HIDE         410
#define IDC_YT_VIDEO_MODE       1304
#define IDC_LABEL_YT_VIDEO_MODE 1305

// Menu IDs
#define IDI_APPICON         99
#define IDM_MAIN_MENU       100
#define IDM_FILE_OPEN       101
#define IDM_FILE_EXIT       102
#define IDM_TOOLS_OPTIONS   103
#define IDM_FILE_OPEN_URL   104
#define IDM_FILE_HIDE_TRAY  106
#define IDM_FILE_ADD_FOLDER 110
#define IDM_FILE_PLAYLIST   111
#define IDM_FILE_PASTE      122
#define IDM_FILE_RECENT_BASE 6000  // Recent files use IDs 6000-6009

// Playlist manager dialog
#define IDD_PLAYLIST        930
#define IDC_PLAYLIST_LIST   931
#define IDC_PLAYLIST_SAVE   932

// URL dialog
#define IDD_URL             650
#define IDC_URL_EDIT        651

// Jump to time dialog
#define IDD_JUMPTOTIME      660
#define IDC_JUMPTIME_EDIT   661
#define IDM_PLAY_PLAYPAUSE  201
#define IDM_PLAY_STOP       202
#define IDM_PLAY_PREV       203
#define IDM_PLAY_NEXT       204
#define IDM_PLAY_PLAY       212
#define IDM_PLAY_PAUSE      213
#define IDM_PLAY_SEEKBACK   205
#define IDM_PLAY_SEEKFWD    206
#define IDM_PLAY_VOLUP      207
#define IDM_PLAY_VOLDOWN    208
#define IDM_PLAY_ELAPSED    209
#define IDM_PLAY_REMAINING  210
#define IDM_PLAY_TOTAL      211
#define IDM_PLAY_NOWPLAYING 214
#define IDM_PLAY_SHUFFLE    215
#define IDM_PLAY_BEGINNING  216
#define IDM_PLAY_JUMPTOTIME 217
#define IDM_PLAY_NEAR_END   222
#define IDM_PLAY_MUTE       218
#define IDM_PLAY_REPEAT_TOGGLE 219
#define IDM_EFFECT_PRESETS  238

// Accelerator table
#define IDA_ACCEL           300

// Timer
#define IDT_UPDATE_TITLE    400
#define IDT_BATCH_FILES     401

// Custom messages
#define WM_SPEAK            (WM_USER + 1)
#define WM_ADDFILE          (WM_USER + 2)
#define WM_META_CHANGED     (WM_USER + 3)
#define WM_PLAYLIST_TRACK_CHANGED (WM_USER + 4)
#define WM_YT_HYBRID_READY  (WM_USER + 5)  // YouTube background download finished — swap mpv→BASS
#define WM_YT_LOAD_MORE_DONE (WM_USER + 6) // Background "load more YouTube results" finished

// Playback tab controls
#define IDC_BRING_TO_FRONT  531
#define IDC_LOAD_FOLDER     532
#define IDC_MINIMIZE_TO_TRAY 539
#define IDC_REGISTER_FILE_TYPES 548

// Global hotkeys tab
#define IDC_HOTKEY_LIST     540
#define IDC_HOTKEY_ADD      541
#define IDC_HOTKEY_EDIT     542
#define IDC_HOTKEY_REMOVE   543
#define IDC_HOTKEY_ENABLED  544

// Hotkey assignment dialog
#define IDD_HOTKEY          600
#define IDC_HOTKEY_KEY      601
#define IDC_HOTKEY_ACTION   602

// Options dialog
#define IDD_OPTIONS         500
#define IDC_TAB             501
#define IDC_SOUNDCARD       502
#define IDC_ALLOW_AMPLIFY   503
#define IDC_REMEMBER_STATE  504
#define IDC_REMEMBER_POS    505
#define IDC_VOLUME_STEP     506
#define IDC_SHOW_TITLE      507

// Movement settings (checkboxes for seek amounts)
#define IDC_SEEK_1S         510
#define IDC_SEEK_5S         511
#define IDC_SEEK_10S        512
#define IDC_SEEK_30S        513
#define IDC_SEEK_1M         514
#define IDC_SEEK_5M         515
#define IDC_SEEK_10M        516
#define IDC_SEEK_30M        545
#define IDC_SEEK_1H         546
#define IDC_SEEK_1T         517
#define IDC_SEEK_5T         518
#define IDC_SEEK_10T        519

// Menu commands for seek amount
#define IDM_SEEK_DECREASE   220
#define IDM_SEEK_INCREASE   221

// Effect controls
#define IDM_EFFECT_PREV     230
#define IDM_EFFECT_NEXT     231
#define IDM_EFFECT_UP       232
#define IDM_EFFECT_DOWN     233
#define IDM_EFFECT_RESET    234
#define IDM_EFFECT_MIN      235
#define IDM_EFFECT_MAX      236

// Audio device menu
#define IDM_SHOW_AUDIO_DEVICES  237
#define IDM_AUDIO_DEVICE_BASE   5000  // Actual device IDs are IDM_AUDIO_DEVICE_BASE + device index

// Effect toggles (for hotkeys)
#define IDM_TOGGLE_VOLUME       240
#define IDM_TOGGLE_PITCH        241
#define IDM_TOGGLE_TEMPO        242
#define IDM_TOGGLE_RATE         243
#define IDM_TOGGLE_REVERB       244
#define IDM_TOGGLE_ECHO         245
#define IDM_TOGGLE_EQ           246
#define IDM_TOGGLE_COMPRESSOR   247
#define IDM_TOGGLE_STEREOWIDTH  248
#define IDM_TOGGLE_CENTERCANCEL 249
#define IDM_TOGGLE_SPATIAL      251
#define IDM_TOGGLE_CONVOLUTION  252

// Speak commands
#define IDM_SPEAK_SEEK          250

// Tag reading commands (keys 1-0)
#define IDM_READ_TAG_TITLE      261
#define IDM_READ_TAG_ARTIST     262
#define IDM_READ_TAG_ALBUM      263
#define IDM_READ_TAG_YEAR       264
#define IDM_READ_TAG_TRACK      265
#define IDM_READ_TAG_GENRE      266
#define IDM_READ_TAG_COMMENT    267
#define IDM_READ_TAG_BITRATE    268
#define IDM_READ_TAG_DURATION   269
#define IDM_READ_TAG_FILENAME   260

// Effect settings (checkboxes)
#define IDC_EFFECT_VOLUME   550
#define IDC_EFFECT_PITCH    551
#define IDC_EFFECT_TEMPO    552
#define IDC_EFFECT_RATE     553
#define IDC_RATE_STEP_MODE  554

// DSP effect settings (checkboxes)
#define IDC_DSP_REVERB      560
#define IDC_DSP_ECHO        561
#define IDC_DSP_EQ          562
#define IDC_DSP_COMPRESSOR  563
#define IDC_DSP_STEREOWIDTH 564
#define IDC_DSP_CENTERCANCEL 565
#define IDC_DSP_SPATIAL     566

// Advanced tab controls
#define IDC_BUFFER_SIZE     570
#define IDC_UPDATE_PERIOD   571
#define IDC_TEMPO_ALGORITHM 572
#define IDC_EQ_BASS_FREQ    573
#define IDC_EQ_MID_FREQ     574
#define IDC_EQ_TREBLE_FREQ  575
#define IDC_LEGACY_VOLUME   576
#define IDC_DISABLE_BATCH   577
#define IDC_RESET_LIST_ORDER 578

// SoundTouch settings (tab 7)
#define IDC_ST_AA_FILTER        580
#define IDC_ST_AA_LENGTH        581
#define IDC_ST_QUICK_ALGO       582
#define IDC_ST_SEQUENCE         583
#define IDC_ST_SEEKWINDOW       584
#define IDC_ST_OVERLAP          585
#define IDC_ST_PREVENT_CLICK    586
#define IDC_ST_ALGORITHM        587


// 3D Audio settings (tab 14)
#define IDC_SPATIAL_MODE        605
#define IDC_SPATIAL_REAR_CENTER 606

// Speedy settings (tab 9)
#define IDC_SPEEDY_NONLINEAR    610

// Signalsmith settings (tab 10)
#define IDC_SS_PRESET           615
#define IDC_SS_TONALITY         616

// MIDI settings (tab 11)
#define IDC_MIDI_SOUNDFONT      620
#define IDC_MIDI_SF_BROWSE      621
#define IDC_MIDI_VOICES         622
#define IDC_MIDI_SINC           623

// System tray
#define IDM_TRAY_RESTORE    700
#define IDM_TRAY_EXIT       701
#define WM_TRAYICON         (WM_USER + 10)
#define IDM_TOGGLE_WINDOW   702

// YouTube menu and dialog
#define IDM_FILE_YOUTUBE    105
#define IDD_YOUTUBE         750
#define IDC_YT_SEARCH       751
#define IDC_YT_RESULTS      752
#define IDC_YT_LOADMORE     753
#define IDC_YT_DOWNLOAD     754
#define IDM_HELP_CLEAR_YT_CACHE 119

// YouTube tab in Options
#define IDC_YTDLP_PATH      760
#define IDC_YTDLP_BROWSE    761
#define IDC_YT_APIKEY       762
#define IDC_YT_CLEAR_ON_EXIT 763
#define IDC_YT_CLEAR_NOW    764
#define IDC_YT_CACHE_LIMIT  765
#define IDC_LABEL_YT_LIMIT  766

// Bookmarks
#define IDM_BOOKMARK_ADD    800
#define IDM_BOOKMARK_LIST   801
#define IDD_BOOKMARKS       810
#define IDC_BOOKMARK_LIST   811
#define IDC_BOOKMARK_FILTER 812

// Recording
#define IDM_RECORD_TOGGLE   820
#define IDC_REC_PATH        830
#define IDC_REC_BROWSE      831
#define IDC_REC_TEMPLATE    832
#define IDC_REC_FORMAT      833
#define IDC_REC_BITRATE     834

// Speech tab
#define IDC_SPEECH_TRACKCHANGE  840
#define IDC_SPEECH_VOLUME       841
#define IDC_SPEECH_EFFECT       842
#define IDC_SPEECH_YT_HYBRID    843

// Radio dialog
#define IDM_FILE_RADIO      107
#define IDM_FILE_ADD_TO_FAVORITES 113
#define IDM_HELP_PLUGINS    109
#define IDM_HELP_UPDATES    112
#define IDM_HELP_README     114
#define IDM_HELP_TEST_YOUTUBE   118
#define IDM_HELP_AUDIT_LAYOUT   120
#define IDM_HELP_MANUAL         121
#define IDM_KEYBOARD_HELP_TOGGLE 123
#define IDM_HELP_SET_DEFAULT     124

// =========================================================================
// DAISY / EPUB book reader (v1.49 — Phase 1)
// =========================================================================
// Menu commands
#define IDM_FILE_OPEN_BOOK              1500
#define IDM_TOOLS_BOOK_LIBRARY          1501

// Book actions (bound via the keymap)
#define IDM_BOOK_NAV_LEVEL_UP           1510   // Shift+Up   — cycle nav level up
#define IDM_BOOK_NAV_LEVEL_DOWN         1511   // Shift+Down — cycle nav level down
#define IDM_BOOK_NAV_FORWARD            1512   // Shift+Right — jump to next nav point
#define IDM_BOOK_NAV_BACKWARD           1513   // Shift+Left  — jump to previous nav point
#define IDM_BOOK_ANNOUNCE_LOCATION      1514   // Announce current chapter / page
#define IDM_BOOK_ADD_BOOKMARK_WITH_NOTE 1515   // Bookmark with optional note
#define IDM_BOOK_GO_TO_PAGE             1516   // G — jump to a specific page

// Books library dialog
#define IDD_BOOK_LIBRARY                1520
#define IDC_BOOK_LIST                   1521
#define IDC_BOOK_OPEN_SELECTED          1522
#define IDC_BOOK_REMOVE_SELECTED        1523
#define IDC_BOOK_RESCAN                 1524
#define IDC_LABEL_BOOK_LIST             1525

// Bookmark-note input dialog
#define IDD_BOOK_BOOKMARK_NOTE          1530
#define IDC_BOOK_BOOKMARK_NOTE_EDIT     1531
#define IDC_LABEL_BOOK_BOOKMARK_PROMPT  1532

// Go-to-page dialog
#define IDD_BOOK_GO_TO_PAGE             1540
#define IDC_BOOK_GO_TO_PAGE_EDIT        1541
#define IDC_LABEL_BOOK_GO_TO_PAGE       1542

// Books preferences tab controls
#define IDC_BOOK_FOLDERS_LIST           1550
#define IDC_BOOK_FOLDER_ADD             1551
#define IDC_BOOK_FOLDER_REMOVE          1552
#define IDC_LABEL_BOOK_FOLDERS          1553
// v1.50 Phase 2 — extended Books prefs
#define IDC_BOOK_TTS_VOICE              1554
#define IDC_LABEL_BOOK_TTS_VOICE        1555
#define IDC_BOOK_TEXT_THEME             1556
#define IDC_LABEL_BOOK_TEXT_THEME       1557
#define IDC_BOOK_HIDE_TEXT_WINDOW       1558

// =========================================================================
// DAISY/EPUB reader — Phase 2 (v1.50): text window + F3 search + TTS
// =========================================================================
// Text display window (modeless)
#define IDD_BOOK_TEXT_WINDOW            1560
#define IDC_BOOK_TEXT_EDIT              1561

// Toggle / focus the text window
#define IDM_BOOK_TOGGLE_TEXT_WINDOW     1562

// F3 search dialog
#define IDD_BOOK_SEARCH                 1565
#define IDC_BOOK_SEARCH_EDIT            1566
#define IDC_BOOK_SEARCH_NEXT            1567
#define IDC_LABEL_BOOK_SEARCH_PROMPT    1568
#define IDM_BOOK_SEARCH                 1569
#define IDM_BOOK_SEARCH_NEXT            1570

// Progress dialog (for updates)
#define IDD_PROGRESS        995
#define IDC_PROGRESS_BAR    996
#define IDC_PROGRESS_TEXT   997
#define IDC_CHECK_UPDATES   998
#define IDC_MULTI_INSTANCE  999
#define IDD_RADIO           850
#define IDC_RADIO_TAB       851
#define IDC_RADIO_LIST      852
#define IDC_RADIO_ADD       853
#define IDC_RADIO_IMPORT    854
#define IDC_RADIO_SEARCH_EDIT   855
#define IDC_RADIO_SEARCH_BTN    856
#define IDC_RADIO_SEARCH_LIST   857
#define IDC_RADIO_SEARCH_ADD    858
#define IDC_RADIO_SEARCH_SOURCE 859
#define IDC_RADIO_SEARCH_COUNTRY       864
#define IDC_RADIO_SEARCH_COUNTRY_LABEL 865
#define IDC_RADIO_SEARCH_LIST_LABEL    866

// Add station dialog
#define IDD_RADIO_ADD       860
#define IDC_RADIO_NAME      861
#define IDC_RADIO_URL       862
#define IDC_RADIO_EXPORT    863

// Scheduler
#define IDM_FILE_SCHEDULE   108
#define IDD_SCHEDULER       870
#define IDC_SCHED_LIST      871
#define IDC_SCHED_ADD       872
#define IDC_SCHED_EDIT      873

// Add schedule dialog
#define IDD_SCHED_ADD       880
#define IDC_SCHED_NAME      881
#define IDC_SCHED_ACTION    882
#define IDC_SCHED_SOURCE    883
#define IDC_SCHED_FILE      884
#define IDC_SCHED_BROWSE    885
#define IDC_SCHED_RADIO     886
#define IDC_SCHED_DATE      887
#define IDC_SCHED_TIME      888
#define IDC_SCHED_REPEAT    889
#define IDC_SCHED_ENABLED   890
#define IDC_SCHED_DURATION  891
#define IDC_SCHED_STOP      892

// Scheduler timer
#define IDT_SCHEDULER       402
#define IDT_SCHED_DURATION  403

// Chapter seeking
#define IDC_CHAPTER_SEEK    900

// Auto-advance playlist
#define IDC_AUTO_ADVANCE    901
#define IDC_REWIND_ON_PAUSE 902
#define IDC_REWIND_LABEL    903
#define IDC_PLAYLIST_FOLLOW 904

// Tag view dialog
#define IDD_TAG_VIEW        910
#define IDC_TAG_TEXT        911

// Preset name input dialog
#define IDD_PRESET_NAME     915
#define IDC_PRESET_NAME     916

// Preset menu base IDs (dynamic)
#define IDM_PRESET_BASE     7000  // Apply preset: IDM_PRESET_BASE + index (up to 100)
#define IDM_PRESET_DELETE_BASE 7100  // Delete preset: IDM_PRESET_DELETE_BASE + index (up to 100)
#define IDM_PRESET_SAVE_NEW 7200

// Convolution reverb controls
#define IDC_DSP_CONVOLUTION     920
#define IDC_CONV_IR             921
#define IDC_CONV_BROWSE         922

// View tag commands (Shift+1-0)
#define IDM_VIEW_TAG_TITLE      270
#define IDM_VIEW_TAG_ARTIST     271
#define IDM_VIEW_TAG_ALBUM      272
#define IDM_VIEW_TAG_YEAR       273
#define IDM_VIEW_TAG_TRACK      274
#define IDM_VIEW_TAG_GENRE      275
#define IDM_VIEW_TAG_COMMENT    276
#define IDM_VIEW_TAG_BITRATE    277
#define IDM_VIEW_TAG_DURATION   278
#define IDM_VIEW_TAG_FILENAME   279

// Podcast dialog
#define IDM_FILE_PODCAST        940
#define IDD_PODCAST             950
#define IDC_PODCAST_TAB         951
#define IDC_PODCAST_SUBS_LIST   952
#define IDC_PODCAST_EPISODES    953
#define IDC_PODCAST_ADD_FEED    954
#define IDC_PODCAST_REFRESH     956
#define IDC_PODCAST_SEARCH_EDIT 960
#define IDC_PODCAST_SEARCH_BTN  961
#define IDC_PODCAST_SEARCH_LIST 962
#define IDC_PODCAST_SUBSCRIBE   963
#define IDC_PODCAST_ADD_URL     964
#define IDC_PODCAST_SUBS_LABEL  965
#define IDC_PODCAST_EP_LABEL    966
#define IDC_PODCAST_SUBS_HELP   967
#define IDC_PODCAST_SEARCH_LABEL 968
#define IDC_PODCAST_SEARCH_HELP 969
#define IDC_PODCAST_EP_DESC     955

// Add podcast subscription dialog
#define IDD_PODCAST_ADD         970
#define IDC_PODCAST_NAME        971
#define IDC_PODCAST_FEED_URL    972
#define IDC_PODCAST_USERNAME    973
#define IDC_PODCAST_PASSWORD    974
#define IDC_PODCAST_DOWNLOAD    975
#define IDC_PODCAST_IMPORT_OPML 976
#define IDC_PODCAST_DOWNLOAD_ALL 977
#define IDC_PODCAST_EXPORT_OPML 978

// Downloads tab
#define IDC_DOWNLOAD_PATH       980
#define IDC_DOWNLOAD_BROWSE     981
#define IDC_DOWNLOAD_ORGANIZE   982

// Song history dialog
#define IDM_VIEW_SONG_HISTORY   990
#define IDD_SONG_HISTORY        991
#define IDC_HISTORY_LIST        992
#define IDC_HISTORY_COPY        993
#define IDC_HISTORY_CLEAR       994


// Static label IDs for IDD_OPTIONS (so ShowTabControls can hide/show them per tab)
// Playback tab labels
#define IDC_LABEL_PLAYBACK_OUTPUT_DEVICE    1000
#define IDC_LABEL_PLAYBACK_REMEMBER_POS     1001
#define IDC_LABEL_PLAYBACK_VOLUME_STEP      1002

// Recording tab labels
#define IDC_LABEL_RECORDING_DESCRIPTION     1010
#define IDC_LABEL_RECORDING_OUTPUT_FOLDER   1011
#define IDC_LABEL_RECORDING_TEMPLATE        1012
#define IDC_LABEL_RECORDING_TEMPLATE_HELP   1013
#define IDC_LABEL_RECORDING_FORMAT          1014
#define IDC_LABEL_RECORDING_BITRATE         1015
#define IDC_LABEL_RECORDING_BITRATE_NOTE    1016

// Downloads tab labels
#define IDC_LABEL_DOWNLOADS_DESCRIPTION     1020
#define IDC_LABEL_DOWNLOADS_FOLDER          1021

// Speech tab labels
#define IDC_LABEL_SPEECH_DESCRIPTION        1030

// Movement tab labels
#define IDC_LABEL_MOVEMENT_DESCRIPTION      1040

// Effects tab labels
#define IDC_LABEL_EFFECTS_DESCRIPTION       1050
#define IDC_LABEL_EFFECTS_STEP              1051
#define IDC_LABEL_EFFECTS_DSP_DESCRIPTION   1052
#define IDC_LABEL_EFFECTS_REVERB            1053
#define IDC_LABEL_EFFECTS_IR_FILE           1054

// Advanced tab labels
#define IDC_LABEL_ADVANCED_BUFFER_DESC      1060
#define IDC_LABEL_ADVANCED_BUFFER_SIZE      1061
#define IDC_LABEL_ADVANCED_UPDATE_PERIOD    1062
#define IDC_LABEL_ADVANCED_LATENCY_NOTE     1063
#define IDC_LABEL_ADVANCED_TEMPO_ALGO       1064
#define IDC_LABEL_ADVANCED_EQ_FREQ          1065
#define IDC_LABEL_ADVANCED_EQ_BASS          1066
#define IDC_LABEL_ADVANCED_EQ_MID           1067
#define IDC_LABEL_ADVANCED_EQ_TREBLE        1068

// YouTube tab labels
#define IDC_LABEL_YOUTUBE_YTDLP_PATH        1070
#define IDC_LABEL_YOUTUBE_API_KEY           1071
#define IDC_LABEL_YOUTUBE_API_HELP          1072
#define IDC_LABEL_YOUTUBE_API_NOTE          1073

// SoundTouch tab labels
#define IDC_LABEL_SOUNDTOUCH_DESCRIPTION    1080
#define IDC_LABEL_SOUNDTOUCH_AA_LENGTH      1081
#define IDC_LABEL_SOUNDTOUCH_INTERPOLATION  1082
#define IDC_LABEL_SOUNDTOUCH_SEQUENCE       1083
#define IDC_LABEL_SOUNDTOUCH_SEEKWINDOW     1084
#define IDC_LABEL_SOUNDTOUCH_OVERLAP        1085
#define IDC_LABEL_SOUNDTOUCH_AUTO_NOTE      1086

// Speedy tab labels
#define IDC_LABEL_SPEEDY_DESCRIPTION        1090
#define IDC_LABEL_SPEEDY_INFO1              1091
#define IDC_LABEL_SPEEDY_INFO2              1092

// Signalsmith tab labels
#define IDC_LABEL_SIGNALSMITH_DESCRIPTION   1100
#define IDC_LABEL_SIGNALSMITH_QUALITY       1101
#define IDC_LABEL_SIGNALSMITH_TONALITY      1102
#define IDC_LABEL_SIGNALSMITH_HARMONICS     1103
#define IDC_LABEL_SIGNALSMITH_VALUES        1104

// MIDI tab labels
#define IDC_LABEL_MIDI_DESCRIPTION          1110
#define IDC_LABEL_MIDI_SOUNDFONT            1111
#define IDC_LABEL_MIDI_VOICES               1112
#define IDC_LABEL_MIDI_VOICES_RANGE         1113
#define IDC_LABEL_MIDI_SF_BUNDLED           1114

// Language selector (placed on Playback tab)
#define IDC_LANGUAGE_COMBO                  1310
#define IDC_LABEL_LANGUAGE                  1311

// =========================================================================
// Actions / Keymap system (REAPER-style)
// =========================================================================
// Menu command for Tools → Actions...
#define IDM_TOOLS_ACTIONS                   1400

// Actions dialog (the F4 window)
#define IDD_ACTIONS                         1410
#define IDC_ACTIONS_CATEGORY                1411
#define IDC_ACTIONS_SEARCH                  1412
#define IDC_ACTIONS_LIST                    1413
#define IDC_ACTIONS_SHORTCUTS               1414
#define IDC_ACTIONS_ADD                     1415
#define IDC_ACTIONS_DELETE                  1416
#define IDC_ACTIONS_EDIT                    1417
#define IDC_ACTIONS_LOAD                    1418
#define IDC_ACTIONS_SAVE_AS                 1419
#define IDC_ACTIONS_RESET                   1420
#define IDC_ACTIONS_GLOBAL_CHECK            1421
#define IDC_LABEL_ACTIONS_CATEGORY          1422
#define IDC_LABEL_ACTIONS_SEARCH            1423
#define IDC_LABEL_ACTIONS_LIST              1424
#define IDC_LABEL_ACTIONS_SHORTCUTS         1425
#define IDC_LABEL_ACTIONS_KEYMAP            1426
#define IDC_ACTIONS_KEYMAP_NAME             1427

// Shortcut assignment dialog (press a key to learn it)
#define IDD_SHORTCUT_ASSIGN                 1430
#define IDC_SHORTCUT_DISPLAY                1431
#define IDC_LABEL_SHORTCUT_PROMPT           1432
#define IDC_LABEL_SHORTCUT_CURRENT          1433

// Find-by-shortcut dialog (press a shortcut, jump to the matching action)
#define IDC_ACTIONS_FIND_SHORTCUT           1440
#define IDD_FIND_SHORTCUT                   1441
#define IDC_FIND_SHORTCUT_DISPLAY           1442
#define IDC_LABEL_FIND_SHORTCUT_PROMPT      1443
#define IDC_LABEL_FIND_SHORTCUT_HINT        1444
#define IDC_FIND_SHORTCUT_SEARCH            1445

#define IDOK                1
#define IDCANCEL            2
