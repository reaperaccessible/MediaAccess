#!/usr/bin/env python3
# =============================================================================
# validate_keymaps.py — pre-build sanity check for MediaAccess keymaps.
#
# Purpose: catch any drift between src/actions.cpp's g_actions[] registry and
# the regional default keymaps shipped under KeyMaps/*.MediaAccessKeyMap before
# they ever reach a user's machine. Triggered automatically by build_new.bat
# (added in v1.84) — the build aborts if the validator exits non-zero.
#
# Checks (all four accumulate; the script prints every issue then exits 1):
#   1. Action missing — an action with a non-empty default shortcut in
#      g_actions[] is not listed in a shipped keymap. Symptom for the user:
#      the action would be unbound until LoadActiveKeyMapAtStartup's
#      MergeMissingDefaults step inserts the default at next launch. We
#      catch it pre-ship so the keymap on disk matches the registry exactly.
#   2. Default mismatch — an action's shortcut in the shipped keymap differs
#      from the default declared in g_actions[]. For FR-FR this is expected
#      for the AZERTY-remapped actions (PLAYER_PREV, EFFECT_PREV/NEXT); the
#      script lists those exceptions explicitly. Any other mismatch is a bug.
#   3. Action orphan — the keymap lists an action that has no default in
#      g_actions[] OR an unknown action ID. Suggests the registry was edited
#      but the shipped keymap wasn't regenerated.
#   4. Intra-category duplicate — two actions in the same category share the
#      same shortcut. This is exactly what ResolveSameCategoryDuplicates fights
#      at runtime; catching it at build time prevents the wiped-line bug from
#      v1.83 where the runtime dedup blanked one of the colliding entries and
#      MergeMissingDefaults then couldn't tell phantom from genuine tombstone
#      (fixed in v1.84, see keymap.cpp).
# =============================================================================

import os
import re
import sys
from pathlib import Path
from collections import defaultdict

ROOT = Path(__file__).resolve().parent.parent
ACTIONS_CPP = ROOT / "src" / "actions.cpp"
KEYMAPS_DIR = ROOT / "KeyMaps"
KEYMAP_FILES = ["USA", "FR-CA", "FR-FR"]

# FR-FR has documented AZERTY remaps in keymap.cpp BuildDefaultFrFrKeyMap.
# We don't flag those as default mismatches.
FRFR_EXPECTED_REMAPS = {
    "PLAYER_PREV": "W",
    "EFFECT_PREV": "OEMCloseBracket",
    "EFFECT_NEXT": "OEMSemicolon",
}

# =============================================================================
# Parse g_actions[] from src/actions.cpp
#
# Row format (multi-line, spans 3 lines):
#   { "STRING_ID",                IDM_*,                ActionCategory::Cat,
#     "English",                  "Français",
#     { VK_OR_LITERAL, ctrl, shift, alt[, win] } },
# =============================================================================

NAMED_VK = {
    "VK_SPACE": "Space", "VK_BACK": "Backspace", "VK_TAB": "Tab",
    "VK_RETURN": "Enter", "VK_ESCAPE": "Escape",
    "VK_LEFT": "Left", "VK_RIGHT": "Right", "VK_UP": "Up", "VK_DOWN": "Down",
    "VK_HOME": "Home", "VK_END": "End",
    "VK_PRIOR": "PageUp", "VK_NEXT": "PageDown",
    "VK_INSERT": "Insert", "VK_DELETE": "Delete",
    "VK_F1": "F1", "VK_F2": "F2", "VK_F3": "F3", "VK_F4": "F4",
    "VK_F5": "F5", "VK_F6": "F6", "VK_F7": "F7", "VK_F8": "F8",
    "VK_F9": "F9", "VK_F10": "F10", "VK_F11": "F11", "VK_F12": "F12",
    "VK_F13": "F13", "VK_F14": "F14", "VK_F15": "F15", "VK_F16": "F16",
    "VK_F17": "F17", "VK_F18": "F18", "VK_F19": "F19", "VK_F20": "F20",
    "VK_NUMPAD0": "NumPad0", "VK_NUMPAD1": "NumPad1", "VK_NUMPAD2": "NumPad2",
    "VK_NUMPAD3": "NumPad3", "VK_NUMPAD4": "NumPad4", "VK_NUMPAD5": "NumPad5",
    "VK_NUMPAD6": "NumPad6", "VK_NUMPAD7": "NumPad7", "VK_NUMPAD8": "NumPad8",
    "VK_NUMPAD9": "NumPad9",
    "VK_MULTIPLY": "Multiply", "VK_ADD": "Add", "VK_SUBTRACT": "Subtract",
    "VK_DECIMAL": "Decimal", "VK_DIVIDE": "Divide",
    "VK_OEM_COMMA": "OEMComma", "VK_OEM_PERIOD": "OEMPeriod",
    "VK_OEM_MINUS": "OEMMinus", "VK_OEM_PLUS": "OEMPlus",
    "VK_OEM_1": "OEMSemicolon", "VK_OEM_2": "OEMSlash",
    "VK_OEM_3": "OEMTilde", "VK_OEM_4": "OEMOpenBracket",
    "VK_OEM_5": "OEMBackslash", "VK_OEM_6": "OEMCloseBracket",
    "VK_OEM_7": "OEMQuotes",
}


def vk_to_name(vk_token):
    """Convert a single VK token (e.g. 'VK_SPACE', "'X'", "'1'", 0) to keymap text."""
    t = vk_token.strip()
    if t == "0":
        return ""  # no default
    if t in NAMED_VK:
        return NAMED_VK[t]
    m = re.match(r"^'(.)'$", t)
    if m:
        return m.group(1)
    # Last resort
    return f"VK({t})"


def parse_shortcut(vk, ctrl, shift, alt, win):
    if not vk:
        return ""
    parts = []
    if ctrl:
        parts.append("Ctrl")
    if shift:
        parts.append("Shift")
    if alt:
        parts.append("Alt")
    if win:
        parts.append("Win")
    parts.append(vk)
    return "+".join(parts)


def parse_actions_cpp(path):
    """Parse g_actions[] entries. Returns list of dicts:
       {stringId, category, default_shortcut} — default_shortcut is "" if none.
    """
    text = path.read_text(encoding="utf-8", errors="replace")
    # Restrict to the g_actions[] body.
    start = text.find("static const Action g_actions[] = {")
    if start < 0:
        print("[FATAL] Could not locate g_actions[] in actions.cpp", file=sys.stderr)
        sys.exit(2)
    body = text[start:]

    # Strip C/C++ line and block comments so they don't break the regex —
    # several actions have explanatory comments wedged between the French
    # name and the { vk, ... } initializer (e.g. VIDEO_SUB_CYCLE).
    no_block = re.sub(r"/\*.*?\*/", "", body, flags=re.DOTALL)
    no_comments = re.sub(r"//[^\n]*", "", no_block)

    # Match each entry. Two shapes:
    #   Full:  { "ID", IDM_, Cat, "en", "fr", { vk, c, s, a[, w] } }
    #   Empty: { "ID", IDM_, Cat, "en", "fr", {} }            (Global category)
    pattern = re.compile(
        r'\{\s*"([A-Z0-9_]+)"\s*,\s*'      # stringId
        r'[A-Za-z0-9_]+\s*,\s*'             # commandId
        r'ActionCategory::(\w+)\s*,\s*'     # category
        r'"[^"]*"\s*,\s*'                   # English name
        r'"[^"]*"\s*,\s*'                   # French name
        r'(\{[^{}]*\})',                    # shortcut initializer (no nesting)
        re.MULTILINE | re.DOTALL,
    )

    actions = []
    for m in pattern.finditer(no_comments):
        string_id, cat, init = m.groups()
        inner = init.strip()[1:-1].strip()  # strip the outer braces
        if not inner:
            # {} — no default shortcut
            actions.append({
                "stringId": string_id,
                "category": cat,
                "default": "",
            })
            continue
        parts = [p.strip() for p in inner.split(",")]
        if len(parts) < 4:
            actions.append({
                "stringId": string_id,
                "category": cat,
                "default": "",
            })
            continue
        vk_tok = parts[0]
        ctrl = parts[1] == "true"
        shift = parts[2] == "true"
        alt = parts[3] == "true"
        win = (len(parts) >= 5 and parts[4] == "true")
        vk_name = vk_to_name(vk_tok)
        actions.append({
            "stringId": string_id,
            "category": cat,
            "default": parse_shortcut(vk_name, ctrl, shift, alt, win),
        })
    return actions


# =============================================================================
# Parse a .MediaAccessKeyMap file
# =============================================================================

def parse_keymap(path):
    """Returns dict stringId -> list of shortcut texts (may be empty list)."""
    bindings = {}
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or line.startswith(";"):
            continue
        if "=" not in line:
            continue
        key, val = line.split("=", 1)
        key = key.strip()
        val = val.strip()
        if key in ("NAME", "REGION"):
            continue
        if not re.match(r"^[A-Z0-9_]+$", key):
            continue
        shortcuts = [s.strip() for s in val.split(",") if s.strip()] if val else []
        bindings[key] = shortcuts
    return bindings


# =============================================================================
# Main
# =============================================================================

def main():
    print("=== MediaAccess Keymap Validation ===")
    actions = parse_actions_cpp(ACTIONS_CPP)
    by_id = {a["stringId"]: a for a in actions}

    cat_counts = defaultdict(int)
    for a in actions:
        cat_counts[a["category"]] += 1
    with_default = sum(1 for a in actions if a["default"])

    cat_summary = ", ".join(f"{c}: {n}" for c, n in sorted(cat_counts.items()))
    print(f"Actions in g_actions[]    : {len(actions)} ({cat_summary})")
    print(f"Actions with defaults     : {with_default}")
    print(f"Keymaps validated         : {', '.join(KEYMAP_FILES)}")
    print()

    issues = []

    for km_name in KEYMAP_FILES:
        km_path = KEYMAPS_DIR / f"{km_name}.MediaAccessKeyMap"
        if not km_path.exists():
            issues.append(f"[FAIL] Keymap file missing: {km_path}")
            continue
        bindings = parse_keymap(km_path)

        # Check 1: default action missing from keymap.
        for a in actions:
            if not a["default"]:
                continue
            if a["stringId"] not in bindings:
                issues.append(
                    f"[FAIL] {a['stringId']}: default '{a['default']}' missing in {km_name}"
                )

        # Check 2: default mismatch.
        for a in actions:
            if not a["default"]:
                continue
            if a["stringId"] not in bindings:
                continue
            shortcuts = bindings[a["stringId"]]
            if not shortcuts:
                # Empty list in shipped keymap is suspicious.
                issues.append(
                    f"[FAIL] {a['stringId']}: empty in {km_name} but has default '{a['default']}'"
                )
                continue
            # FR-FR remaps are first-shortcut substitutions.
            if km_name == "FR-FR" and a["stringId"] in FRFR_EXPECTED_REMAPS:
                expected_vk = FRFR_EXPECTED_REMAPS[a["stringId"]]
                # The remapped shortcut keeps the same modifier prefix; strip
                # modifiers from the default to compare just the key portion.
                default_key = a["default"].split("+")[-1]
                got_key = shortcuts[0].split("+")[-1]
                if got_key != expected_vk:
                    issues.append(
                        f"[FAIL] {a['stringId']}: FR-FR expected key '{expected_vk}', got '{got_key}' in '{shortcuts[0]}'"
                    )
                continue
            # All other cases: first shortcut must match the default exactly.
            if shortcuts[0] != a["default"]:
                issues.append(
                    f"[FAIL] {a['stringId']}: default '{a['default']}' but {km_name} has '{shortcuts[0]}'"
                )

        # Check 3: orphan actions in the keymap.
        for sid in bindings:
            if sid not in by_id:
                issues.append(f"[FAIL] {km_name}: unknown action '{sid}'")
                continue
            # Known action with empty default but listed in keymap is allowed
            # (user may want to assign manually) but on shipped default keymaps
            # we shouldn't ship empty action lines.
            if not by_id[sid]["default"] and not bindings[sid]:
                issues.append(
                    f"[FAIL] {km_name}: orphan entry '{sid} =' (no default in registry)"
                )

        # Check 4: intra-category duplicates.
        by_cat_shortcut = defaultdict(list)  # (category, shortcut) -> [stringIds]
        for sid, shortcuts in bindings.items():
            if sid not in by_id:
                continue
            cat = by_id[sid]["category"]
            for sc in shortcuts:
                by_cat_shortcut[(cat, sc)].append(sid)
        for (cat, sc), owners in by_cat_shortcut.items():
            if len(owners) > 1:
                issues.append(
                    f"[FAIL] {km_name} / {cat}: '{sc}' assigned to multiple actions: {', '.join(owners)}"
                )

    if issues:
        for line in issues:
            print(line)
        print()
        print(f"{len(issues)} issue(s) found. Exit 1.")
        return 1

    print("OK — all keymaps consistent with g_actions[].")
    return 0


if __name__ == "__main__":
    sys.exit(main())
