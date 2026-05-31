# MediaAccess — Refactoring Backlog

> **Purpose**: A standing inventory of known cleanup opportunities for the MediaAccess C++ codebase. Findings come from the v1.68 five-agent refactoring pass (issues spotted but explicitly deferred to keep diffs surgical) plus a follow-up read of the codebase. This file is **for the developer/orchestrator only** — not user-facing, not shipped, not tied to any release.
>
> **How to use it**: When you start a cleanup wave, pick a section ("High-value, low-risk" first), pick the top one or two items, and dispatch them as small focused tasks. Each item is sized so it can be done and verified independently. Behavior changes should always stay "no" — if an item flips to "yes", it belongs in a different backlog.
>
> **Update rule**: When an item is completed, delete it (don't strike it through). When you discover a new one, append it to the right section.

---

## Inventory of files over 1000 lines

| File | Lines | Notes |
|---|---|---|
| `src/player.cpp` | 2761 | Main BASS engine. Tag speech, stream lifecycle, ReinitBass, recording. Several refactor candidates. |
| `src/daisy_book.cpp` | 2137 | DAISY 2.02 + DAISY 3 + EPUB 3 parsing, all in one TU. Three logical books inside. |
| `src/ui_radio.cpp` | 1774 | Radio search, favorites, M3U/PLS import. Has its own HTTP and JSON helpers. |
| `src/ui_podcast.cpp` | 1565 | Podcast feeds. Two near-duplicate HTTP functions. |
| `src/main.cpp` | 1419 | WinMain, window proc, message routing. |
| `src/effects.cpp` | 1281 | DSP effect graph. |
| `src/youtube.cpp` | 1275 | yt-dlp wrapping. Own HttpGet + own JSON helpers. |
| `src/translations_player.cpp` | 1224 | Generated translation table. |
| `src/ui_options.cpp` | 1162 | Options dialog. Contains the 13-block `ShowTabControls`. |
| `src/ui.cpp` | 1098 | Main UI plumbing. M3U/PLS parsers live here. |
| `src/database.cpp` | 1053 | SQLite wrapper. |

Header sizes are all reasonable (largest is `globals.h` at 196 lines).

---

## Conventions confirmed (don't break these)

These are rules the codebase already follows; future contributors and refactors must respect them.

1. **Stream-cleanup order is sacred.** Always `RemoveDSPEffects()` → `BASS_ChannelStop()` → `BASS_StreamFree(g_fxStream)` → `BASS_StreamFree(g_stream)`. See the comment in `player.cpp` near line 356 ("Always BASS_ChannelStop() before BASS_StreamFree()") and the v1.68 "do not touch BASS init/teardown" exclusion.
2. **Stable action string IDs.** The `stringId` strings in `g_actions[]` in `src/actions.cpp` MUST NEVER change after release — they are written into user keymap files.
3. **Default keymap is USA.** FR-CA and FR-FR ship as overlay text files in `KeyMaps/`. New actions get a USA default in `g_actions[]` only; the locale files override.
4. **Two languages, no third.** `Ts(...)` looks up English/French only via a single boolean (`IsFrench()` in `actions.cpp:583`). Don't introduce a third language without redesigning the translation index.
5. **UTF-8 on the wire, UTF-16 in Win32 calls.** Always use `MultiByteToWideChar(CP_UTF8, ...)` for HTTP responses, file content, INI files. `WideToUtf8`/`Utf8ToWide` are the only canonical converters — see #5 in the backlog.
6. **Wide-char Win32 only.** Everything goes through the `*W` variants of Win32 APIs (`InternetOpenW`, `InternetOpenUrlW`, `GetPrivateProfileStringW`, etc.). No A-suffix calls except where unavoidable.
7. **Tab indices in `ShowTabControls` are positional.** Indices 0..12 are wired to `IDC_*` arrays; reordering tabs requires updating both the `TabCtrl_InsertItem` calls and the `ShowTabControls` mapping in lockstep.
8. **`namespace mediaaccess`** is the canonical wrapper. Newer modules (`actions.cpp`, `actions_window.cpp`, `keymap.cpp`, `book_text_window.cpp`, `tts_player.cpp`, `daisy_player.cpp`, `daisy_book.cpp`, `books_dialog.cpp`, `sleep_timer.cpp`) are all in it. Older modules (`player.cpp`, `ui.cpp`, `main.cpp`, etc.) are at global scope. Don't fight this split in a single PR.
9. **No CLAUDE.md / no `.claude/` in repo.** Don't create one without buy-in.
10. **`g_*` globals are intentional, but each one belongs in `globals.cpp` if it crosses files.** Per-file statics stay `static` at file scope.

---

## High-value, low-risk (do these next)

### 1. Consolidate `Utf8ToWide` / `WideToUtf8` into the one in `utils.cpp`

- **Status**: proposed
- **Files affected**:
  - `src/utils.cpp` (already has canonical defs at lines 5 and 14)
  - `src/ytdlp_updater.cpp:173` (delete the duplicate `Utf8ToWide`)
  - `src/keymap.cpp:406` (delete the static `WideToUtf8`; keep `#include "utils.h"`)
  - `src/actions_window.cpp:47-65` (the local `U8ToW`/`WToU8` are also duplicates — replace call sites)
- **Effort**: 30 min
- **Risk**: low — the two implementations in `utils.cpp` and `ytdlp_updater.cpp` are byte-identical save for variable names. The `keymap.cpp` and `actions_window.cpp` copies are file-static, so removal cannot affect linkage.
- **Behavior change?**: no
- **Description**: Four copies of the same Win32 codepage conversion. `utils.h` already exports the right names. The redundant copies were added when those modules were written in isolation; nothing prevents them from including `utils.h`.
- **Suggested approach**:
  - In `ytdlp_updater.cpp`: `#include "mediaaccess/utils.h"` (or `"utils.h"`), delete the local definition at lines 173-179. Confirm no namespace clash (the existing one is in an anon namespace? Check; the snippet shows file scope).
  - In `keymap.cpp`: same — include and delete the static at 406-414.
  - In `actions_window.cpp`: include `utils.h`, replace `U8ToW(x)` → `Utf8ToWide(x)` and `WToU8(x)` → `WideToUtf8(x)` throughout, delete the local helpers at lines 47-65.
  - Build, run, smoke-test yt-dlp update + actions dialog + keymap load.
- **Dependencies**: none
- **Why we didn't fix it now**: crosses four file ownership lines.

### 2. Remove dead `CenterCancelProcessor::ApplyWindow` declaration

- **Status**: proposed
- **Files affected**:
  - `include/mediaaccess/center_cancel.h:36` (remove the declaration)
  - `src/center_cancel.cpp:128-132` (remove the definition)
- **Effort**: 30 min
- **Risk**: low — verified call sites: `ApplyWindow` is never invoked. `ProcessFrame` at `center_cancel.cpp:137-138` applies the window inline (`m_inputBufferL[i] * m_window[i]`). The function is dead code.
- **Behavior change?**: no
- **Description**: Declared in the public class interface but never called. `ProcessFrame` was rewritten to fuse the window into the FFT input copy, leaving `ApplyWindow` orphaned.
- **Suggested approach**: Delete the declaration and definition. Rebuild.
- **Dependencies**: none
- **Why we didn't fix it now**: header change crossed the agent's file allowlist.

### 3. Extract `SpatialAudioProcessor::ReleaseAllEffects()` member

- **Status**: proposed
- **Files affected**:
  - `include/mediaaccess/spatial_audio.h` (add `void ReleaseAllEffects();` private member)
  - `src/spatial_audio.cpp` lines 136-143, 179-186, 349-356 (three near-identical 8-line blocks)
- **Effort**: 30 min
- **Risk**: low — pure mechanical extraction inside a single class. All three blocks are identical (verified): each calls `p_iplBinauralEffectRelease(&m_effect*)` and nulls the pointer for L, R, FL, FR, C, SL, SR, RC.
- **Behavior change?**: no
- **Description**: The 8-pointer release sequence is copy-pasted in `SetMode`, `Initialize`, and `Shutdown`. Adding a new virtual speaker means editing three places; one of them was already missed during the rear-center addition (verify before extracting).
- **Suggested approach**:
  - Add private member `void ReleaseAllEffects()` to the class.
  - Implement once in `spatial_audio.cpp`; replace the three blocks with a call.
  - Sanity-check that all three call sites have the same effect set — if `SetMode` (line 136) was missing `m_effectRC`, fix that as part of this change (would be a latent bug).
- **Dependencies**: none
- **Why we didn't fix it now**: header change.

### 4. Add `CurrentOffsetInClipSeconds()` helper for DAISY position math

- **Status**: proposed
- **Files affected**:
  - `src/daisy_player.cpp:199-208` (`SaveCurrentPosition`)
  - `src/daisy_player.cpp:696-705` (`DaisyAddBookmarkHere`)
  - `src/daisy_player.cpp:436-438` (current-time computation also wants this)
- **Effort**: 30 min
- **Risk**: low — three sites compute `sec - clipBegin`, clamp to ≥ 0. Extracting one helper has no semantic change.
- **Behavior change?**: no
- **Description**: Two and a half copies of the same 5-line "convert current byte position to seconds within the current clip" calculation. A `static double CurrentOffsetInClipSeconds()` (probably file-static, reading `g_d.stream` and `g_d.book`/`g_d.currentClip` directly) is a clean factoring.
- **Suggested approach**:
  - Add file-static helper at top of `daisy_player.cpp` (near other helpers).
  - Replace the three blocks with calls.
  - Verify the `CurrentTime`-style accessor at line 436 also adds `g_d.clipsBeforeDuration` (different shape — may want a second helper for "absolute book seconds").
- **Dependencies**: none
- **Why we didn't fix it now**: small enough that it kept getting skipped.

### 5. Remove dead `DaisyState::currentOffset` field

- **Status**: proposed
- **Files affected**:
  - `src/daisy_player.cpp:61` (field declaration)
  - `src/daisy_player.cpp:158, 266` (the two write sites — both set to 0.0)
- **Effort**: 30 min
- **Risk**: low — grep confirms only writes, no reads.
- **Behavior change?**: no
- **Description**: `g_d.currentOffset` is written in two places but never read anywhere. Likely a refactor leftover from when the value was cached before the position-on-pause logic moved to `SaveCurrentPosition`.
- **Suggested approach**: Delete the member; delete the two assignments. Rebuild; if it builds, you're done.
- **Dependencies**: ideally do item #4 first so future readers don't reintroduce a similar cache.
- **Why we didn't fix it now**: low-priority and dead-code removal is psychologically scary in a state struct.

### 6. Move Cooley-Tukey FFT into a shared TU

- **Status**: proposed
- **Files affected**:
  - New: `src/fft_radix2.cpp` (or `src/dsp/fft.cpp`) + `include/mediaaccess/fft_radix2.h`
  - `src/convolution.cpp:211-249` (delete `ConvolutionReverb::FFT`)
  - `src/center_cancel.cpp:88-126` (delete `CenterCancelProcessor::FFT`)
  - `include/mediaaccess/convolution.h` and `include/mediaaccess/center_cancel.h` (remove the member decls)
- **Effort**: 2 h
- **Risk**: low — the comment in `convolution.cpp:210` already certifies "same as center_cancel.cpp". I diffed both implementations: identical except for being inside their respective classes.
- **Behavior change?**: no
- **Description**: Two bit-for-bit identical Cooley-Tukey radix-2 FFTs, one per class. Extract to a free function `void Fft(std::complex<float>* data, int n, bool inverse)` in its own TU. Both classes call it from `ProcessFrame` / equivalent.
- **Suggested approach**:
  - Create `include/mediaaccess/fft_radix2.h` declaring `void Fft(std::complex<float>* data, int n, bool inverse)`.
  - Create `src/fft_radix2.cpp` with the single implementation.
  - Delete both class methods (header decls + .cpp impls).
  - Replace `FFT(...)` call sites with `mediaaccess::Fft(...)` (or global, matching the namespace convention of those files — both are currently global-scope).
  - Add the new .cpp to the build (`build_new.bat`).
- **Dependencies**: none, but bundle with item #2 since both touch `center_cancel.h`.
- **Why we didn't fix it now**: header change + needs a new TU + build script edit.

### 7. Extract `FetchTagWithFallback(stream, field)` for the SpeakTag* family

- **Status**: proposed
- **Files affected**:
  - `src/player.cpp:2454-2700` (eight `SpeakTag*` functions: Title, Artist, Album, Year, Track, Genre, Comment; Bitrate/Duration/Filename are different and stay)
- **Effort**: 2 h
- **Risk**: low — the skeleton is genuinely identical (get tag stream → try BASS_TAG_OGG/etc via `GetMetadataTag` → fall back to `BASS_TAG_ID3` → speak with label or "No X"). The 8 functions are ~30 lines each; helper shrinks them to ~5.
- **Behavior change?**: no
- **Description**: Each speak-tag function repeats the same 4-stage lookup. A helper that returns the resolved string and lets the caller format it cuts ~150 lines and centralizes the ID3v1 fallback (which is currently slightly inconsistent — `Album` adds station-name fallback, `Track` decodes ID3v1.1 specially, `Comment` handles the comment[28]==0 case).
- **Suggested approach**:
  - Sketch helper signature: something like
    ```cpp
    // Returns the resolved tag value (e.g., "Track 7") or empty.
    // 'metaKey' is the BASS metadata field name ("TITLE", "ARTIST", ...).
    // 'id3Selector' is a lambda or function pointer that pulls the
    // corresponding field from a TAG_ID3*; returns "" if not present.
    std::string FetchTagWithFallback(HSTREAM stream, const char* metaKey,
                                     std::function<std::string(const TAG_ID3*)> id3Fallback);
    ```
  - Title is special (has a `BuildNowPlayingSpeech` first-shot) — leave it alone or wrap only its tail.
  - Album is special (station-name fallback) — keep its custom path.
  - Track is special (ID3v1.1 comment[29] hack) — pass a lambda that handles it.
  - Convert Artist, Year, Genre, Comment (the four uniform ones) first; revisit Album/Track after.
- **Dependencies**: none
- **Why we didn't fix it now**: 200 lines of code with subtle per-function variations; the agent didn't want to risk losing an edge case.

### 8. Replace fragile "Recent Files" submenu lookup with a stable ID-based path

- **Status**: proposed
- **Files affected**:
  - `src/settings.cpp:1015-1059` (`UpdateRecentFilesMenu`)
  - `resource.h` (add `IDM_FILE_RECENT_SUBMENU` placeholder or similar)
  - `MediaAccess.rc` (add a stable identifier on the Recent submenu MENUITEM if the resource compiler supports it, or use the position constant)
- **Effort**: 1 h
- **Risk**: medium — touches the resource file and menu lookup. NVDA reads menus, so the visible string must stay the same.
- **Behavior change?**: no
- **Description**: `UpdateRecentFilesMenu` currently iterates File menu items, gets each menu string, and matches the substring `L"Recent"`. This breaks the moment the menu is localized (and "Recent" becomes "Récents"). Lookup should use a stable position constant or a hidden command ID.
- **Suggested approach**: simplest fix is `#define RECENT_FILES_SUBMENU_POS N` (whatever the index is in the File menu) and `GetSubMenu(hFileMenu, RECENT_FILES_SUBMENU_POS)`. Add a comment in `MediaAccess.rc` next to the menu warning that reordering changes the constant. Heavier fix: use `GetMenuItemInfo` with a dummy hidden command ID on the Recent submenu placeholder.
- **Dependencies**: none, but ideally fixed before any menu-translation pass.
- **Why we didn't fix it now**: requires resource-file change and the localization of the File menu hasn't shipped yet.

### 9. Add `EnumerateEnabledAudioDevices()` accessor

- **Status**: proposed
- **Files affected**:
  - `include/mediaaccess/audio_slots.h` (declare)
  - `src/audio_slots.cpp` (implement; replace local loops at lines 90-91, 152-153)
  - `src/player.cpp:147-156, 174-190` (`FindDeviceByName`, `ShowAudioDeviceMenu`)
  - `src/ui_options.cpp:272-275` (sound-card combo population)
- **Effort**: 1 h
- **Risk**: low — pure read-only enumeration; the per-call-site `BASS_DEVICE_ENABLED` filter is identical in all six sites.
- **Behavior change?**: no
- **Description**: Five different functions open-code `for (int i = 1; BASS_GetDeviceInfo(i, &info); i++) if (info.flags & BASS_DEVICE_ENABLED)`. Return a `std::vector<DeviceInfo> { int index; std::wstring name; bool isDefault; }`.
- **Suggested approach**:
  - Add a struct + accessor in `audio_slots.h`.
  - Implement once; each call site collapses to a range-for.
  - Make sure `BassDeviceNameToWide` (in `player.cpp:140`) is centralized too — the conversion of `info.name` (which is a `char*` of unclear encoding) lives in one place.
- **Dependencies**: none
- **Why we didn't fix it now**: crossed `audio_slots.cpp` + `player.cpp` + `ui_options.cpp`.

---

## High-value, medium-risk (needs care)

### 10. Consolidate the three HTTP GETs into `http_client.cpp`

- **Status**: proposed
- **Files affected**:
  - New: `src/http_client.cpp` + `include/mediaaccess/http_client.h`
  - `src/ui_radio.cpp:109-148` (`RadioHttpGet`)
  - `src/ui_podcast.cpp:49-137` (`PodcastHttpGet`) and `:140-...` (`PodcastHttpGetAuth`)
  - `src/youtube.cpp:89-114` (`HttpGet`)
  - `src/updater.cpp:199-...` (`HttpGet` — yet a fourth! uses different signature: `host, path, https`)
- **Effort**: 4 h+
- **Risk**: medium — four implementations have meaningfully different semantics:
  - `youtube.cpp::HttpGet`: simplest. Always uses `INTERNET_FLAG_SECURE` (forces HTTPS).
  - `ui_radio.cpp::RadioHttpGet`: supports extra request headers (`Accept: application/json`), conditionally sets `INTERNET_FLAG_SECURE` based on URL scheme. **Bug spotted while reading**: `INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE` is on every request, intentional.
  - `ui_podcast.cpp::PodcastHttpGet`: uses `InternetConnect`+`HttpOpenRequest` (not `InternetOpenUrl`), exposes status code, byte count, body preview, error text via out-parameter `PodcastFetchDiag*`.
  - `ui_podcast.cpp::PodcastHttpGetAuth`: same as above plus username/password for Basic Auth.
  - `updater.cpp::HttpGet`: different signature `(host, path, https)`, uses `InternetConnect`.
- **Behavior change?**: no (if done right)
- **Description**: Unifying these is the single biggest deduplication win in the codebase, but the union of features is non-trivial.
- **Suggested approach**:
  - Design a single API:
    ```cpp
    struct HttpRequest {
        std::wstring url;
        std::wstring extraHeaders;     // L"Accept: application/json\r\n", etc.
        std::wstring basicAuthUser;    // empty = none
        std::wstring basicAuthPass;
        bool         disableCache = true;
    };
    struct HttpResponse {
        std::wstring body;
        DWORD        statusCode = 0;
        size_t       bytesReceived = 0;
        DWORD        lastError = 0;
        std::wstring errorText;
        std::wstring bodyPreview;      // first ~400 chars, for diag
    };
    HttpResponse HttpGet(const HttpRequest& req);
    ```
  - Migration order: easiest wins first.
    1. Migrate `youtube.cpp::HttpGet` (uses no exotic features).
    2. Migrate `ui_radio.cpp::RadioHttpGet` (adds extraHeaders).
    3. Migrate `ui_podcast.cpp::PodcastHttpGet` (adds diag).
    4. Migrate `ui_podcast.cpp::PodcastHttpGetAuth` (adds Basic Auth).
    5. Migrate `updater.cpp::HttpGet` last (different call shape — may be left alone if it's not worth the risk).
  - Test each migration on its actual use case (open a YouTube search → search radio → fetch a podcast feed → download an update).
- **Dependencies**: do item #1 (consolidate Utf8ToWide) first so the new `http_client.cpp` can rely on the canonical converter.
- **Why we didn't fix it now**: four files, four feature sets, and the URL-parsing logic is duplicated separately from the GET logic — needs a real design pass.

### 11. Centralize JSON helpers into `json_util.cpp`

- **Status**: proposed
- **Files affected**:
  - New: `src/json_util.cpp` + `include/mediaaccess/json_util.h`
  - `src/ui_radio.cpp:174-223` (`ExtractJsonString`, `ExtractJsonInt`, `ExtractJsonValue`)
  - `src/youtube.cpp` (`JsonUnescape` at line 121, plus its own ExtractJson* helpers and brace-matching iterators)
  - `src/ytdlp_updater.cpp:163-171` (`ExtractJsonField`)
- **Effort**: 4 h+
- **Risk**: medium — JSON parsing is one of those domains where "this regex is good enough" tends to silently fail on new data shapes. RadioBrowser, YouTube, and GitHub release JSON have different conventions for escaping and nesting.
- **Behavior change?**: no — but watch carefully for nesting depth differences.
- **Description**: Three+ ad-hoc JSON parsers using string search + brace-counting. A shared single TU with `ExtractString`, `ExtractInt`, `ExtractRawValue`, `UnescapeString`, and a brace-matching iterator would replace all of them.
- **Suggested approach**:
  - DON'T introduce a real JSON library (nlohmann::json etc.) — adds a dependency, and these parsers are intentionally minimal because they only ever read top-level fields.
  - Pick the *most correct* of the three implementations (probably `ui_radio.cpp` since it handles escaped quotes) and promote it.
  - Make `JsonUnescape` from `youtube.cpp:121` the canonical unescape — it's the only one that handles `\uXXXX` and surrogate pairs.
  - Migrate one caller at a time and run that feature end-to-end after each.
- **Dependencies**: parallel with #10 — both belong to a "shared utilities" cleanup phase.
- **Why we didn't fix it now**: high-traffic code with subtle differences; needs careful one-call-site-at-a-time migration.

### 12. Factor `ShowTabControls` to a data-driven loop

- **Status**: proposed
- **Files affected**:
  - `src/ui_options.cpp:43-171` (the function and its 13 hardcoded arrays)
- **Effort**: 2 h
- **Risk**: medium — the brief flagged "NVDA depends on tab control + ShowWindow ordering subtleties". Verified: the function does Show first / Hide last? No — it iterates each tab's array and conditionally shows or hides. Tab-switch announcement might depend on the order of `ShowWindow(SW_HIDE)` calls (the focused control should be hidden before the new tab's controls become visible, or NVDA may double-announce).
- **Behavior change?**: no — must remain bit-for-bit equivalent.
- **Description**: 13 near-identical `for (int id : ctrls) ShowWindow(GetDlgItem(hwnd, id), tab == N ? SW_SHOW : SW_HIDE);` blocks. Could collapse to:
  ```cpp
  struct TabPanel { int index; const int* ids; size_t count; };
  static const TabPanel kTabs[] = { {0, playbackCtrls, ...}, ... };
  for (const auto& t : kTabs)
      for (size_t i = 0; i < t.count; ++i)
          ShowWindow(GetDlgItem(hwnd, t.ids[i]), tab == t.index ? SW_SHOW : SW_HIDE);
  ```
- **Suggested approach**:
  - First, manually verify NVDA still announces the tab change correctly with the current implementation (baseline).
  - Refactor.
  - Re-test NVDA tab announcement and Tab/Shift+Tab traversal on every tab.
  - If subtle ordering bugs emerge, add a comment to the function explaining the constraint and revert.
- **Dependencies**: none, but only do this if you have NVDA running during the change.
- **Why we didn't fix it now**: explicit "NVDA may depend on ordering" caveat in the brief.

### 13. Resolve the `g_ytdlpPath` race

- **Status**: needs-design
- **Files affected**:
  - `src/globals.cpp:323` (declaration)
  - `src/settings.cpp:188-214` (writes during startup from `LoadSettings`)
  - `src/ytdlp_updater.cpp:219` (write from background updater thread)
  - Readers: `src/ui.cpp:849`, `src/video_engine.cpp:397-402`, `src/youtube.cpp:64, 211, 330`
- **Effort**: 2 h
- **Risk**: medium — a stale read just means the *previous* yt-dlp binary gets used for one operation. Real but small.
- **Behavior change?**: no (correctness improvement only)
- **Description**: `g_ytdlpPath` is a `std::wstring` written from both the startup thread (`LoadSettings`) and the background updater thread (`ytdlp_updater.cpp:UpdateThread`), with no synchronization. `std::wstring` assignment is not atomic; concurrent read + write is undefined behavior. In practice the updater fires several seconds after startup so there's a wide window, but it's still a real race.
- **Suggested approach**:
  - Option A (simplest): make `g_ytdlpPath` accessed only through a `std::mutex`-guarded getter/setter pair: `std::wstring GetYtdlpPath()`, `void SetYtdlpPath(std::wstring)`. All six readers migrate. Use a `static std::mutex` inside the accessor.
  - Option B (atomic pointer): store a `std::atomic<std::shared_ptr<std::wstring>>`. Faster, more "modern", but C++ atomic shared_ptr support is uneven before C++20.
  - Pick A. Migrate all six read sites and both write sites.
- **Dependencies**: none
- **Why we didn't fix it now**: every refactor of a free global is a candidate to slip and break a reader; needs a focused turn.

### 14. Extract a stream-cleanup helper for non-BASS-init sites

- **Status**: needs-design
- **Files affected**:
  - `src/player.cpp` — `LoadFile`, `LoadURL`, `FreeCurrentStream` (line 1253), and a few callbacks at 422, 526, 543, 879, 1028, 1119
  - **NOT** `ReinitBass` (line 1861) — that one is on the "do not touch BASS init/teardown" exclusion list.
- **Effort**: 2 h
- **Risk**: medium — order is sacred (see Conventions #1). Each site has subtle variations: some null `g_fxStream` after free, some don't; some call `RemoveDSPEffects()` first, some only after `BASS_ChannelStop`.
- **Behavior change?**: no — strict invariant preservation.
- **Description**: ~6 sites do "remove syncs, RemoveDSPEffects, stop, free fxStream, free stream, null the handles". A helper `static void TeardownActiveStream()` would consolidate them. The trick is identifying which sites perform the full sequence vs. partial sequences.
- **Suggested approach**:
  - Audit every site that calls `BASS_StreamFree(g_fxStream)` or `BASS_StreamFree(g_stream)` (already grepped: lines 362, 367, 422, 424, 436, 526, 543, 879, 881, 884, 1028, 1031, 1035, 1119, 1253, 1269, 1271, 1283, 1882, 1886).
  - Categorize: (a) full teardown, (b) partial (only one stream), (c) part of init/reinit (skip).
  - Extract `TeardownActiveStream()` only for category (a). Leave (b) and (c) alone.
  - This is **the** kind of change where a code review by the maintainer matters; don't merge from an agent's diff without eyes-on.
- **Dependencies**: none, but coordinate with anyone else touching `player.cpp`.
- **Why we didn't fix it now**: high risk of breaking audio-stream lifecycle; needs careful audit.

### 15. Split `actions.cpp` into action-catalog vs keymap-file-IO

- **Status**: proposed
- **Files affected**:
  - `src/actions.cpp` (737 lines) — split into `actions.cpp` (catalog + accessors, ~550 lines) + `keymap_text_format.cpp` (~190 lines from line ~645 onwards)
  - Possibly move `g_actions[]` table (lines 30-541, ~510 lines of static data) into `actions_table.inc` included from `actions.cpp`
- **Effort**: 2 h
- **Risk**: low-to-medium — pure code motion, but build script and includes need updating.
- **Behavior change?**: no
- **Description**: `actions.cpp` mixes two concerns: (1) the canonical action registry + lookup accessors, (2) the textual keymap-file format (parse/serialize a `Shortcut` to/from `"Ctrl+Shift+O"` form). They have no shared state and could live in separate TUs.
- **Suggested approach**:
  - Move the keymap-text functions (`ShortcutToKeymapText`, `ShortcutFromKeymapText`, `KeyNameDisplay`, `NameForVK`, `VKForName`, plus the `kNamedKeys[]` table) to a new `src/keymap_text_format.cpp`.
  - They depend on `Shortcut` (declared in `actions.h`), so the new file just needs to include that header.
  - Add the new file to `build_new.bat`.
  - Optionally also move `g_actions[]` to a `.inc` so editing it doesn't show up in code-review diffs as a 510-line table change.
- **Dependencies**: none
- **Why we didn't fix it now**: requires a build-script edit.

### 16. Split `actions_window.cpp` (assign-shortcut subclass + actions dialog)

- **Status**: proposed
- **Files affected**:
  - `src/actions_window.cpp` (625 lines)
- **Effort**: 2 h
- **Risk**: low
- **Behavior change?**: no
- **Description**: Same split rationale as #15 — `actions_window.cpp` holds both the Actions dialog proc and a separate WNDPROC subclass for the "press a key" shortcut-capture edit control. The subclass is independent and could move to `src/shortcut_capture_control.cpp`.
- **Suggested approach**: identify the subclass WNDPROC and its install/uninstall helpers; move them to their own TU. Mostly a `git mv`-and-update-includes operation.
- **Dependencies**: do #15 first so a coherent "actions/keymap" module structure emerges.
- **Why we didn't fix it now**: bundled-with-#15 deferral.

### 17. Extract a `playlist_parser.cpp` shared by `ui.cpp` and `ui_radio.cpp`

- **Status**: needs-design
- **Files affected**:
  - `src/ui.cpp:688-816` (`ParseM3U`, `ParsePLS`, `ParsePlaylist`)
  - `src/ui_radio.cpp:1530-1670` (radio import path — has its own M3U/PLS parsing inline)
- **Effort**: 4 h+
- **Risk**: medium — the two parsers differ in semantics:
  - `ui.cpp::ParseM3U`: accepts any URL/path, expands folders, returns full paths.
  - `ui_radio.cpp` import: filters to `http://`/`https://` only, extracts station-name from `#EXTINF:duration,Station Name` line, builds a list of `{name, url}` pairs (not bare strings).
- **Behavior change?**: no
- **Description**: Both implementations do BOM detection, line iteration, EXTINF parsing. A low-level "give me an iterator of (extinf?, payload) pairs" would let both callers post-process for their needs.
- **Suggested approach**:
  - Define an intermediate type, e.g.
    ```cpp
    struct PlaylistEntry { std::wstring extinfTitle; std::wstring url; };
    std::vector<PlaylistEntry> ParseM3ULow(const std::wstring& path);
    std::vector<PlaylistEntry> ParsePLSLow(const std::wstring& path);
    ```
  - `ui.cpp::ParsePlaylist` wraps with folder-expansion + URL/path full-path resolution.
  - `ui_radio.cpp` import wraps with HTTP filter + name extraction.
  - Be mindful that PLS parsing also has `Title<N>=<name>` entries — `ParsePLSLow` should expose those too.
- **Dependencies**: none
- **Why we didn't fix it now**: real design work, not just code motion.

---

## Low-priority (nice to have)

### 18. Consider splitting `daisy_book.cpp` (2137 lines, three parsers)

- **Status**: low-priority
- **Files affected**: `src/daisy_book.cpp`
- **Effort**: 4 h+
- **Risk**: medium — many file-static helpers; some are shared between the three parsers.
- **Behavior change?**: no
- **Description**: One file contains three book-format parsers: DAISY 2.02 (`ParseDaisy202` at 1001), DAISY 3 (`ParseDaisy3` at 1319), EPUB 3 (`ParseEpub3Metadata` at 1976). Plus shared helpers (`SplitFragment`, `FileExists`, `ReadWholeFile`, `ParseSmilFile`, etc.). Could split into:
  - `daisy_book_common.cpp` — shared helpers + the `DaisyBook` struct logic
  - `daisy_book_202.cpp`, `daisy_book_3.cpp`, `daisy_book_epub.cpp`
- **Suggested approach**: only attempt if the file genuinely becomes a pain to maintain. 2137 lines is large but not unmanageable in a single TU. Defer until either a new book format ships or a bug forces a deep audit.
- **Dependencies**: none
- **Why we didn't fix it now**: working file, no immediate pain.

### 19. Remove `MigrateLegacyHotkeysIfPresent()` after enough time has passed

- **Status**: low-priority
- **Files affected**:
  - `src/keymap.cpp:664-...` (function body)
  - `src/keymap.cpp:28` (the `extern const HotkeyAction g_hotkeyActions[]` declaration)
  - `src/main.cpp:1447` (the call site)
  - `include/mediaaccess/keymap.h:155` (function declaration)
  - Eventually: `src/globals.cpp:203-255` (the `g_hotkeyActions[]` table — but it's also read at `main.cpp:658` for the legacy `actionIdx` path, so deeper cleanup needed)
- **Effort**: 2 h (just the migration removal); 4 h+ (also retiring `g_hotkeyActions[]` entirely)
- **Risk**: medium — breaks the v1.0→v1.4x upgrade path for any user who skipped past v1.4x.
- **Behavior change?**: yes (for users who never opened v1.4x → v1.6x will lose their pre-v1.41 keymap on first launch)
- **Description**: One-shot migration that ran at the v1.41 boundary; most installs migrated long ago. The legacy `g_hotkeyActions[]` extern is also still consulted at `main.cpp:658` for the `actionIdx >= 0` path in saved hotkeys, so the migration removal is the easy half — the harder half is retiring `actionIdx` from `HotkeyConfig`/`types.h:34` as well.
- **Suggested approach**:
  - Decide on a deprecation horizon (e.g., "any user who hasn't run v1.4x or later by 2026-12 is unlikely to ever upgrade").
  - Phase 1: remove just `MigrateLegacyHotkeysIfPresent()` and its call. The legacy `g_hotkeyActions[]` extern stays.
  - Phase 2 (later): retire `actionIdx` from `HotkeyConfig`, retire `g_hotkeyActions[]` table.
- **Dependencies**: none
- **Why we didn't fix it now**: behavior change (drops upgrade path); not urgent.

### 20. Audit and consider removing `g_hotkeyActions[]` extern from `ui_internal.h`

- **Status**: low-priority
- **Files affected**:
  - `include/mediaaccess/ui_internal.h:57` (duplicate extern)
  - `include/mediaaccess/globals.h:124` (canonical extern)
- **Effort**: 30 min
- **Risk**: low — just a duplicate declaration; removing it from `ui_internal.h` is harmless as long as users include `globals.h`.
- **Behavior change?**: no
- **Description**: `g_hotkeyActions[]` is declared `extern` in two headers. Drop the `ui_internal.h` one.
- **Suggested approach**: delete the line, rebuild, fix any include errors.
- **Dependencies**: none
- **Why we didn't fix it now**: easy to forget about.

### 21. Audit `ParseM3U` UTF-8 BOM handling

- **Status**: low-priority
- **Files affected**: `src/ui.cpp:707-713`
- **Effort**: 30 min
- **Risk**: low
- **Behavior change?**: no
- **Description**: The BOM check reads 3 bytes; if file is shorter than 3 bytes, `fread` returns 0 but the `bom` buffer was zero-initialized so the test silently passes (saying "no BOM"). That's actually correct, but it's worth a one-line comment.
- **Suggested approach**: add a comment.
- **Why we didn't fix it now**: trivial.

### 22. Strip redundant `static` on file-local helpers that are already in anonymous-namespace TUs

- **Status**: low-priority
- **Files affected**: various
- **Effort**: 30 min per file
- **Risk**: low
- **Behavior change?**: no
- **Description**: Some files use both `static` *and* place helpers inside `namespace { ... }`. One is sufficient. Cosmetic only.
- **Why we didn't fix it now**: pure cosmetic.

---

## Needs design discussion (not just a refactor)

### 23. Mixed namespace strategy: `mediaaccess::` vs global scope

- **Status**: needs-design
- **Files affected**: all .cpp/.h files
- **Effort**: 4 h+ for design; weeks for full migration
- **Risk**: high if attempted in one PR; low if done module-by-module.
- **Behavior change?**: no
- **Description**: Newer modules live in `namespace mediaaccess` (DAISY player, books, actions, sleep_timer, tts_player). Older modules live at global scope (player, ui, main, settings, effects). The dividing line is rough date-of-introduction. Bringing the older modules into the namespace is "the right thing" but a long migration with churn-y diffs.
- **Suggested approach**: don't undertake without a written plan. Could be done leaf-first (utilities → DSP → UI → main). Many globals (e.g., `g_stream`, `g_hwnd`) would need namespace migration too, breaking the global-variable-by-extern pattern.
- **Why this isn't just a refactor**: it's a multi-week mechanical migration that affects every file. Probably not worth it.

### 24. Should `g_actions[]` table move to a JSON/.inc file?

- **Status**: needs-design
- **Files affected**: `src/actions.cpp:30-541`
- **Effort**: 2 h (mechanical move)
- **Risk**: low
- **Description**: 510 lines of static data with the full action catalog. Moving to `actions_table.inc` (included once from `actions.cpp`) keeps the catalog visually separate from the registry code. Alternatives: a JSON file loaded at startup (rejected — adds parsing cost + startup-failure mode), a generated header from a CSV (overkill).
- **Suggested approach**: `.inc` file. Decide whether to keep it next to `actions.cpp` in `src/` or move under `src/data/`.
- **Why this isn't just a refactor**: pure preference; need a decision on whether the visual benefit is worth the indirection.

### 25. Library boundary: should DSP live under `src/dsp/`?

- **Status**: needs-design
- **Files affected**: `center_cancel.cpp`, `convolution.cpp`, `spatial_audio.cpp`, `effects.cpp`, `tempo_processor.cpp`, plus a future `fft_radix2.cpp` (item #6)
- **Effort**: 2 h
- **Description**: Folder hygiene only — these 5+ files form a logical DSP layer. Group them in `src/dsp/`?
- **Why this isn't just a refactor**: requires updating `build_new.bat` and the agent file-allowlist conventions you've been using.

---

## Notes for future cleanup waves

- The v1.68 pass focused on per-file deduplication with strict file allowlists. The cross-file work in items #1-#11 and #14-#17 needs a different orchestration: assign **one** agent to all files in the change, not five.
- Before any wave: snapshot the build (`build_new.bat`), run smoke tests for the user-visible flows touched (radio search, podcast subscribe, YouTube play, DAISY book navigation, audio device switch, options dialog).
- After any wave touching `player.cpp`: stress-test stream lifecycle (rapid file open → URL open → device change → close).
- After any wave touching `ui_options.cpp` or menu code: run NVDA and confirm announcements.
