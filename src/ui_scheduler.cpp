#include "ui_internal.h"
#include "mediaaccess/translations.h"

// Scheduler dialog

// Scheduled event duration tracking
static ScheduleStopAction g_pendingStopAction = ScheduleStopAction::StopBoth;
static bool g_schedulerMuted = false;  // Track if we muted for scheduled recording

static std::vector<ScheduledEvent> g_schedEvents;

static void RefreshScheduleList(HWND hwnd) {
    HWND hList = GetDlgItem(hwnd, IDC_SCHED_LIST);
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);

    g_schedEvents = GetAllScheduledEvents();
    for (const auto& ev : g_schedEvents) {
        SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(ev.displayName.c_str()));
    }
}

// After a scheduled event fires, compute when it should next fire based on
// its repeat policy and persist the new time. One-time events are simply
// disabled. Daily/Weekly add a fixed interval; Weekdays/Weekends advance
// day-by-day until the next matching weekday is found (handles
// week-crossings naturally); Monthly bumps the month field and lets
// mktime() normalise (so a Feb 31 schedule rolls into early March).
void CalculateNextScheduleTime(int id, int64_t lastRun, ScheduleRepeat repeat) {
    if (repeat == ScheduleRepeat::None) {
        // One-time event, just disable it
        UpdateScheduledEventEnabled(id, false);
        return;
    }

    time_t now = time(nullptr);
    struct tm tm;
    localtime_s(&tm, &now);

    // Start from the current scheduled time and add appropriate interval
    int64_t nextTime = lastRun;

    switch (repeat) {
        case ScheduleRepeat::Daily:
            nextTime += 24 * 60 * 60;  // Add 1 day
            break;

        case ScheduleRepeat::Weekly:
            nextTime += 7 * 24 * 60 * 60;  // Add 1 week
            break;

        case ScheduleRepeat::Weekdays: {
            // Find next weekday
            time_t t = static_cast<time_t>(nextTime);
            do {
                t += 24 * 60 * 60;
                localtime_s(&tm, &t);
            } while (tm.tm_wday == 0 || tm.tm_wday == 6);  // Skip Sun=0, Sat=6
            nextTime = static_cast<int64_t>(t);
            break;
        }

        case ScheduleRepeat::Weekends: {
            // Find next weekend day
            time_t t = static_cast<time_t>(nextTime);
            do {
                t += 24 * 60 * 60;
                localtime_s(&tm, &t);
            } while (tm.tm_wday != 0 && tm.tm_wday != 6);  // Find Sun=0 or Sat=6
            nextTime = static_cast<int64_t>(t);
            break;
        }

        case ScheduleRepeat::Monthly: {
            // Same day next month
            time_t t = static_cast<time_t>(nextTime);
            localtime_s(&tm, &t);
            tm.tm_mon += 1;
            if (tm.tm_mon > 11) {
                tm.tm_mon = 0;
                tm.tm_year += 1;
            }
            nextTime = static_cast<int64_t>(mktime(&tm));
            break;
        }

        default:
            break;
    }

    UpdateScheduledEventTime(id, nextTime);
}

// Handle scheduled duration timer expiry
void HandleScheduledDurationEnd() {
    // Restore mute state if we muted for scheduled recording
    if (g_schedulerMuted) {
        g_muted = false;
        g_schedulerMuted = false;
    }

    switch (g_pendingStopAction) {
        case ScheduleStopAction::StopBoth:
            Stop();
            if (g_isRecording) {
                StopRecording();
            }
            Speak(Ts("Scheduled event ended"));
            break;
        case ScheduleStopAction::StopPlayback:
            Stop();
            Speak(Ts("Scheduled playback ended"));
            break;
        case ScheduleStopAction::StopRecording:
            if (g_isRecording) {
                StopRecording();
                Speak(Ts("Scheduled recording ended"));
            }
            break;
    }
}

// Poll the database for events whose scheduledTime has elapsed and fire
// them. Marks each event "last run = now" BEFORE executing to prevent a
// re-trigger if the action itself blocks long enough for the next tick.
//
// Recording-only mode is a special case: BASS needs an active playback
// stream for the encoder to receive samples, so we still call PlayTrack()
// but force g_muted=true beforehand (saving the flag in g_schedulerMuted
// so HandleScheduledDurationEnd can restore it). The user hears nothing
// but the file streams through the encoder.
void CheckScheduledEvents() {
    std::vector<ScheduledEvent> pending = GetPendingScheduledEvents();

    for (const auto& ev : pending) {
        // Mark as run immediately to prevent re-triggering
        int64_t now = static_cast<int64_t>(time(nullptr));
        UpdateScheduledEventLastRun(ev.id, now);

        // Execute the action
        bool shouldPlay = (ev.action == ScheduleAction::Playback || ev.action == ScheduleAction::Both);
        bool shouldRecord = (ev.action == ScheduleAction::Recording || ev.action == ScheduleAction::Both);

        // Load the source
        if (shouldPlay || shouldRecord) {
            g_playlist.clear();
            g_playlist.push_back(ev.sourcePath);

            // For Recording-only mode, mute playback so user doesn't hear it
            // but the stream still plays (required for encoder to capture audio)
            if (shouldRecord && !shouldPlay) {
                g_muted = true;
                g_schedulerMuted = true;
            }

            // Always play the track first - recording requires audio to flow through the stream
            PlayTrack(0);

            // Start recording AFTER playback begins (requires active stream)
            if (shouldRecord && !g_isRecording) {
                ToggleRecording();
            }

            // Set up duration timer if specified
            if (ev.duration > 0) {
                g_pendingStopAction = ev.stopAction;
                // Set timer for duration (minutes to milliseconds)
                SetTimer(g_hwnd, IDT_SCHED_DURATION, ev.duration * 60 * 1000, nullptr);
            }
        }

        // Speak announcement
        std::wstring msg = std::wstring(T("Scheduled event:")) + L" " + ev.name;
        Speak(WideToUtf8(msg).c_str());

        // Calculate next run time for repeating events
        CalculateNextScheduleTime(ev.id, ev.scheduledTime, ev.repeat);
    }
}

// Scheduler list subclass proc
static WNDPROC g_origSchedListProc = nullptr;

static LRESULT CALLBACK SchedListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            // Toggle enabled state
            int sel = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(g_schedEvents.size())) {
                bool newState = !g_schedEvents[sel].enabled;
                if (UpdateScheduledEventEnabled(g_schedEvents[sel].id, newState)) {
                    Speak(newState ? Ts("Enabled") : Ts("Disabled"));
                    RefreshScheduleList(GetParent(hwnd));
                    SendMessageW(hwnd, LB_SETCURSEL, sel, 0);
                }
            }
            return 0;
        } else if (wParam == VK_DELETE) {
            // Remove selected event
            int sel = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(g_schedEvents.size())) {
                if (RemoveScheduledEvent(g_schedEvents[sel].id)) {
                    Speak(Ts("Schedule removed"));
                    RefreshScheduleList(GetParent(hwnd));
                    int count = static_cast<int>(SendMessageW(hwnd, LB_GETCOUNT, 0, 0));
                    if (count > 0) {
                        if (sel >= count) sel = count - 1;
                        SendMessageW(hwnd, LB_SETCURSEL, sel, 0);
                    }
                }
            }
            return 0;
        } else if (wParam == VK_ESCAPE) {
            SendMessageW(GetParent(hwnd), WM_COMMAND, IDCANCEL, 0);
            return 0;
        }
    } else if (msg == WM_GETDLGCODE) {
        MSG* pmsg = reinterpret_cast<MSG*>(lParam);
        if (pmsg && (pmsg->wParam == VK_RETURN || pmsg->wParam == VK_ESCAPE || pmsg->wParam == VK_DELETE)) {
            return DLGC_WANTMESSAGE;
        }
        return CallWindowProcW(g_origSchedListProc, hwnd, msg, wParam, lParam);
    }
    return CallWindowProcW(g_origSchedListProc, hwnd, msg, wParam, lParam);
}

// Show/hide file vs radio controls based on source selection
static void UpdateSchedSourceControls(HWND hwnd) {
    HWND hSource = GetDlgItem(hwnd, IDC_SCHED_SOURCE);
    int sel = static_cast<int>(SendMessageW(hSource, CB_GETCURSEL, 0, 0));
    bool isFile = (sel == 0);

    // Show/hide file controls
    ShowWindow(GetDlgItem(hwnd, IDC_SCHED_FILE), isFile ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, IDC_SCHED_BROWSE), isFile ? SW_SHOW : SW_HIDE);

    // Show/hide radio control
    ShowWindow(GetDlgItem(hwnd, IDC_SCHED_RADIO), isFile ? SW_HIDE : SW_SHOW);
}

// Event being edited (nullptr = adding new)
static ScheduledEvent* g_editingEvent = nullptr;

// Add/Edit schedule dialog proc
static INT_PTR CALLBACK SchedAddDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            // Set title based on add vs edit mode
            SetWindowTextW(hwnd, g_editingEvent ? T("Edit Scheduled Event") : T("Add Scheduled Event"));

            // Action combo
            HWND hAction = GetDlgItem(hwnd, IDC_SCHED_ACTION);
            SendMessageW(hAction, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T("Playback")));
            SendMessageW(hAction, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T("Recording")));
            SendMessageW(hAction, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T("Both")));
            SendMessageW(hAction, CB_SETCURSEL, 0, 0);

            // Source combo
            HWND hSource = GetDlgItem(hwnd, IDC_SCHED_SOURCE);
            SendMessageW(hSource, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T("File")));
            SendMessageW(hSource, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T("Radio")));
            SendMessageW(hSource, CB_SETCURSEL, 0, 0);

            // Radio stations combo
            HWND hRadio = GetDlgItem(hwnd, IDC_SCHED_RADIO);
            std::vector<RadioStation> stations = GetRadioFavorites();
            for (const auto& rs : stations) {
                int idx = static_cast<int>(SendMessageW(hRadio, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(rs.name.c_str())));
                SendMessageW(hRadio, CB_SETITEMDATA, idx, rs.id);
            }
            if (!stations.empty()) {
                SendMessageW(hRadio, CB_SETCURSEL, 0, 0);
            }

            // Repeat combo
            HWND hRepeat = GetDlgItem(hwnd, IDC_SCHED_REPEAT);
            SendMessageW(hRepeat, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T("Once")));
            SendMessageW(hRepeat, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T("Daily")));
            SendMessageW(hRepeat, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T("Weekly")));
            SendMessageW(hRepeat, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T("Weekdays")));
            SendMessageW(hRepeat, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T("Weekends")));
            SendMessageW(hRepeat, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T("Monthly")));
            SendMessageW(hRepeat, CB_SETCURSEL, 0, 0);

            // Enabled checkbox - default on
            CheckDlgButton(hwnd, IDC_SCHED_ENABLED, BST_CHECKED);

            // Duration - default 0 (no limit)
            SetDlgItemTextW(hwnd, IDC_SCHED_DURATION, L"0");

            // Stop action combo (only relevant when action is "Both")
            HWND hStop = GetDlgItem(hwnd, IDC_SCHED_STOP);
            SendMessageW(hStop, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T("Both")));
            SendMessageW(hStop, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T("Playback only")));
            SendMessageW(hStop, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T("Recording only")));
            SendMessageW(hStop, CB_SETCURSEL, 0, 0);

            // Pre-populate fields if editing
            if (g_editingEvent) {
                SetDlgItemTextW(hwnd, IDC_SCHED_NAME, g_editingEvent->name.c_str());
                SendMessageW(hAction, CB_SETCURSEL, static_cast<int>(g_editingEvent->action), 0);
                SendMessageW(hSource, CB_SETCURSEL, static_cast<int>(g_editingEvent->sourceType), 0);

                if (g_editingEvent->sourceType == ScheduleSource::File) {
                    SetDlgItemTextW(hwnd, IDC_SCHED_FILE, g_editingEvent->sourcePath.c_str());
                } else {
                    // Find and select the radio station
                    for (int i = 0; i < static_cast<int>(stations.size()); i++) {
                        if (stations[i].id == g_editingEvent->radioStationId) {
                            SendMessageW(hRadio, CB_SETCURSEL, i, 0);
                            break;
                        }
                    }
                }

                // Convert timestamp to SYSTEMTIME
                time_t schedTime = static_cast<time_t>(g_editingEvent->scheduledTime);
                struct tm* tm = localtime(&schedTime);
                SYSTEMTIME st = {0};
                st.wYear = static_cast<WORD>(tm->tm_year + 1900);
                st.wMonth = static_cast<WORD>(tm->tm_mon + 1);
                st.wDay = static_cast<WORD>(tm->tm_mday);
                st.wHour = static_cast<WORD>(tm->tm_hour);
                st.wMinute = static_cast<WORD>(tm->tm_min);

                SendDlgItemMessageW(hwnd, IDC_SCHED_DATE, DTM_SETSYSTEMTIME, GDT_VALID,
                    reinterpret_cast<LPARAM>(&st));
                SendDlgItemMessageW(hwnd, IDC_SCHED_TIME, DTM_SETSYSTEMTIME, GDT_VALID,
                    reinterpret_cast<LPARAM>(&st));

                SendMessageW(hRepeat, CB_SETCURSEL, static_cast<int>(g_editingEvent->repeat), 0);
                CheckDlgButton(hwnd, IDC_SCHED_ENABLED, g_editingEvent->enabled ? BST_CHECKED : BST_UNCHECKED);
                SetDlgItemInt(hwnd, IDC_SCHED_DURATION, g_editingEvent->duration, FALSE);
                SendMessageW(hStop, CB_SETCURSEL, static_cast<int>(g_editingEvent->stopAction), 0);
            } else {
                // Set default date/time to now + 1 hour
                SYSTEMTIME st;
                GetLocalTime(&st);
                // Add 1 hour
                FILETIME ft;
                SystemTimeToFileTime(&st, &ft);
                ULARGE_INTEGER uli;
                uli.LowPart = ft.dwLowDateTime;
                uli.HighPart = ft.dwHighDateTime;
                uli.QuadPart += 36000000000ULL;  // 1 hour in 100ns units
                ft.dwLowDateTime = uli.LowPart;
                ft.dwHighDateTime = uli.HighPart;
                FileTimeToSystemTime(&ft, &st);

                SendDlgItemMessageW(hwnd, IDC_SCHED_DATE, DTM_SETSYSTEMTIME, GDT_VALID,
                    reinterpret_cast<LPARAM>(&st));
                SendDlgItemMessageW(hwnd, IDC_SCHED_TIME, DTM_SETSYSTEMTIME, GDT_VALID,
                    reinterpret_cast<LPARAM>(&st));

                // If currently playing, prefill file
                if (g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
                    SetDlgItemTextW(hwnd, IDC_SCHED_FILE, g_playlist[g_currentTrack].c_str());
                }
            }

            UpdateSchedSourceControls(hwnd);
            LocalizeDialog(hwnd);
            SetFocus(GetDlgItem(hwnd, IDC_SCHED_NAME));
            return FALSE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_SCHED_SOURCE:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        UpdateSchedSourceControls(hwnd);
                    }
                    return TRUE;

                case IDC_SCHED_BROWSE: {
                    wchar_t filePath[MAX_PATH] = {0};
                    // Get current value
                    GetDlgItemTextW(hwnd, IDC_SCHED_FILE, filePath, MAX_PATH);

                    OPENFILENAMEW ofn = {sizeof(ofn)};
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFilter = L"All Supported\0*.mp3;*.wav;*.ogg;*.oga;*.flac;*.m4a;*.m4b;*.wma;*.aac;*.opus;*.aiff;*.ape;*.wv;*.mid;*.midi\0"
                                      L"All Files (*.*)\0*.*\0";
                    ofn.lpstrFile = filePath;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.lpstrTitle = T("Select Audio File");
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                    if (GetOpenFileNameW(&ofn)) {
                        SetDlgItemTextW(hwnd, IDC_SCHED_FILE, filePath);
                    }
                    return TRUE;
                }

                case IDOK: {
                    wchar_t name[256] = {0};
                    GetDlgItemTextW(hwnd, IDC_SCHED_NAME, name, 256);

                    if (wcslen(name) == 0) {
                        MessageBoxW(hwnd, T("Please enter a name."), T("Add Schedule"), MB_ICONWARNING);
                        SetFocus(GetDlgItem(hwnd, IDC_SCHED_NAME));
                        return TRUE;
                    }

                    ScheduleAction action = static_cast<ScheduleAction>(
                        SendDlgItemMessageW(hwnd, IDC_SCHED_ACTION, CB_GETCURSEL, 0, 0));

                    int sourceIdx = static_cast<int>(
                        SendDlgItemMessageW(hwnd, IDC_SCHED_SOURCE, CB_GETCURSEL, 0, 0));
                    ScheduleSource sourceType = static_cast<ScheduleSource>(sourceIdx);

                    std::wstring sourcePath;
                    int radioStationId = 0;

                    if (sourceType == ScheduleSource::File) {
                        wchar_t filePath[MAX_PATH] = {0};
                        GetDlgItemTextW(hwnd, IDC_SCHED_FILE, filePath, MAX_PATH);
                        if (wcslen(filePath) == 0) {
                            MessageBoxW(hwnd, T("Please select a file."), T("Add Schedule"), MB_ICONWARNING);
                            SetFocus(GetDlgItem(hwnd, IDC_SCHED_FILE));
                            return TRUE;
                        }
                        sourcePath = filePath;
                    } else {
                        int sel = static_cast<int>(
                            SendDlgItemMessageW(hwnd, IDC_SCHED_RADIO, CB_GETCURSEL, 0, 0));
                        if (sel < 0) {
                            MessageBoxW(hwnd, T("Please select a radio station."), T("Add Schedule"), MB_ICONWARNING);
                            SetFocus(GetDlgItem(hwnd, IDC_SCHED_RADIO));
                            return TRUE;
                        }
                        radioStationId = static_cast<int>(
                            SendDlgItemMessageW(hwnd, IDC_SCHED_RADIO, CB_GETITEMDATA, sel, 0));
                        // Get the URL from the radio stations
                        std::vector<RadioStation> stations = GetRadioFavorites();
                        for (const auto& rs : stations) {
                            if (rs.id == radioStationId) {
                                sourcePath = rs.url;
                                break;
                            }
                        }
                    }

                    // Get date and time
                    SYSTEMTIME stDate = {0}, stTime = {0};
                    SendDlgItemMessageW(hwnd, IDC_SCHED_DATE, DTM_GETSYSTEMTIME, 0,
                        reinterpret_cast<LPARAM>(&stDate));
                    SendDlgItemMessageW(hwnd, IDC_SCHED_TIME, DTM_GETSYSTEMTIME, 0,
                        reinterpret_cast<LPARAM>(&stTime));

                    // Combine date and time
                    SYSTEMTIME st = stDate;
                    st.wHour = stTime.wHour;
                    st.wMinute = stTime.wMinute;
                    st.wSecond = 0;
                    st.wMilliseconds = 0;

                    // Convert to timestamp
                    struct tm tm = {0};
                    tm.tm_year = st.wYear - 1900;
                    tm.tm_mon = st.wMonth - 1;
                    tm.tm_mday = st.wDay;
                    tm.tm_hour = st.wHour;
                    tm.tm_min = st.wMinute;
                    tm.tm_sec = 0;
                    tm.tm_isdst = -1;
                    int64_t scheduledTime = static_cast<int64_t>(mktime(&tm));

                    ScheduleRepeat repeat = static_cast<ScheduleRepeat>(
                        SendDlgItemMessageW(hwnd, IDC_SCHED_REPEAT, CB_GETCURSEL, 0, 0));

                    bool enabled = IsDlgButtonChecked(hwnd, IDC_SCHED_ENABLED) == BST_CHECKED;

                    // Get duration
                    int duration = GetDlgItemInt(hwnd, IDC_SCHED_DURATION, nullptr, FALSE);
                    if (duration < 0) duration = 0;

                    // Get stop action
                    ScheduleStopAction stopAction = static_cast<ScheduleStopAction>(
                        SendDlgItemMessageW(hwnd, IDC_SCHED_STOP, CB_GETCURSEL, 0, 0));

                    bool success = false;
                    if (g_editingEvent) {
                        // Update existing event
                        success = UpdateScheduledEvent(g_editingEvent->id, name, action, sourceType, sourcePath,
                                                       radioStationId, scheduledTime, repeat, enabled,
                                                       duration, stopAction);
                    } else {
                        // Add new event
                        int id = AddScheduledEvent(name, action, sourceType, sourcePath,
                                                   radioStationId, scheduledTime, repeat, enabled,
                                                   duration, stopAction);
                        success = (id >= 0);
                    }

                    if (success) {
                        EndDialog(hwnd, IDOK);
                    } else {
                        MessageBoxW(hwnd, g_editingEvent ? T("Failed to update scheduled event.") : T("Failed to add scheduled event."),
                                    g_editingEvent ? T("Edit Schedule") : T("Add Schedule"), MB_ICONERROR);
                    }
                    return TRUE;
                }

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

// Scheduler dialog proc
static INT_PTR CALLBACK SchedulerDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            // Subclass the listbox
            HWND hList = GetDlgItem(hwnd, IDC_SCHED_LIST);
            g_origSchedListProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SchedListSubclassProc)));

            RefreshScheduleList(hwnd);

            LocalizeDialog(hwnd);

            SetFocus(hList);
            if (SendMessageW(hList, LB_GETCOUNT, 0, 0) > 0) {
                SendMessageW(hList, LB_SETCURSEL, 0, 0);
            }
            return FALSE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_SCHED_ADD:
                    g_editingEvent = nullptr;
                    if (DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_SCHED_ADD),
                                   hwnd, SchedAddDlgProc) == IDOK) {
                        RefreshScheduleList(hwnd);
                        Speak(Ts("Schedule added"));
                    }
                    return TRUE;

                case IDC_SCHED_EDIT: {
                    HWND hList = GetDlgItem(hwnd, IDC_SCHED_LIST);
                    int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
                    if (sel < 0 || sel >= static_cast<int>(g_schedEvents.size())) {
                        Speak(Ts("No schedule selected"));
                        return TRUE;
                    }
                    g_editingEvent = &g_schedEvents[sel];
                    if (DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_SCHED_ADD),
                                   hwnd, SchedAddDlgProc) == IDOK) {
                        RefreshScheduleList(hwnd);
                        SendMessageW(hList, LB_SETCURSEL, sel, 0);
                        Speak(Ts("Schedule updated"));
                    }
                    g_editingEvent = nullptr;
                    return TRUE;
                }

                case IDC_SCHED_LIST:
                    if (HIWORD(wParam) == LBN_DBLCLK) {
                        // Double-click to toggle
                        HWND hList = GetDlgItem(hwnd, IDC_SCHED_LIST);
                        int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(g_schedEvents.size())) {
                            bool newState = !g_schedEvents[sel].enabled;
                            if (UpdateScheduledEventEnabled(g_schedEvents[sel].id, newState)) {
                                Speak(newState ? Ts("Enabled") : Ts("Disabled"));
                                RefreshScheduleList(hwnd);
                                SendMessageW(hList, LB_SETCURSEL, sel, 0);
                            }
                        }
                    }
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;

        case WM_SIZE: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;

            SetWindowPos(GetDlgItem(hwnd, IDC_SCHED_LIST), nullptr, 7, 20, w - 14, h - 60, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_SCHED_ADD), nullptr, 7, h - 32, 50, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_SCHED_EDIT), nullptr, 64, h - 32, 50, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDCANCEL), nullptr, w - 64, h - 22, 50, 14, SWP_NOZORDER);
            return TRUE;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 300;
            mmi->ptMinTrackSize.y = 200;
            return TRUE;
        }
    }
    return FALSE;
}

// Show scheduler dialog
void ShowSchedulerDialog() {
    DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_SCHEDULER), g_hwnd, SchedulerDlgProc);
}
