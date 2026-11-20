#!/usr/bin/env python3
"""Test 20: updater isolation + architecture guards.

Python-only static checks, no compilation. Over the updater/ sources:
  - every file is named updater.h or updater_<thing>.<ext>
  - quoted includes only pull in updater headers (or the generated version.h)
  - no mention of app state / app config / UI helper headers
  - every .cpp/.h declares `namespace updater`
And over the app sources that talk to the updater:
  - main.cpp / ui/app_loop.cpp / ui/menu_bar.cpp reach it only through the
    public facade updater.h (never an internal header or a raw type/function)
  - updater::startup() runs before initializeApplication() in main.cpp
  - updater::render() runs before renderWelcomeScreen() in ui/app_loop.cpp,
    and the welcome-screen gate is suppressed while the modal is up
  - no updater state leaked into core/app_state.h or core/config.h
"""
import argparse, json, re, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
UPDATER_DIR = REPO_ROOT / "updater"
TEST_NAME = "test20_updater_isolation"

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)
NAMESPACE_RE = re.compile(r'\bnamespace\s+updater\b')

# Quoted includes allowed inside updater/: own headers (name starts with
# "updater") plus the generated version header. Nothing else.
INCLUDE_ALLOW_EXTRA = {"version.h"}

# Symbols that belong to the app, never to the updater's own code.
FORBIDDEN_TOKENS = ["AppState", "AppConfig", "appState", "popup_utils",
                    "wrap_text", "open_default", "conversion_screen"]

# --- architecture guards (app side) ---------------------------------------
# App files that talk to the updater must do so only through this facade.
FACADE = "updater.h"
APP_FACADE_FILES = ["main.cpp", "ui/app_loop.cpp", "ui/menu_bar.cpp"]
# Raw updater internals an app file must never name (the facade hides these).
FORBIDDEN_FACADE_REFS = ["updater::UpdateService", "updater::Config",
                         "updater::state("]
# core/ headers that must hold no updater state.
NO_UPDATER_STATE_TOKENS = {
    "core/app_state.h": ["UpdateDialog", "Updater"],
    "core/config.h": ["updateRepoUrl", "skippedUpdateVersion", "[Updates]"],
}


def read_text(rel):
    return (REPO_ROOT / rel).read_text(errors="replace")


def rel_path(path):
    """Repo-relative path string, falling back to the name when outside."""
    try:
        return str(path.relative_to(REPO_ROOT))
    except ValueError:
        return path.name


def quoted_includes(text):
    return INCLUDE_RE.findall(text)


def check_app_facade():
    """App files include only updater.h and never name a raw updater symbol."""
    problems = []
    for rel in APP_FACADE_FILES:
        path = REPO_ROOT / rel
        if not path.is_file():
            problems.append(f"{rel}: missing (cannot verify updater facade)")
            continue
        text = read_text(rel)
        incs = quoted_includes(text)
        bases = [inc.replace("\\", "/").split("/")[-1] for inc in incs]
        if FACADE not in bases:
            problems.append(f"{rel}: does not #include \"{FACADE}\"")
        for inc in incs:
            base = inc.replace("\\", "/").split("/")[-1]
            if base.startswith("updater_"):
                problems.append(f"{rel}: includes internal updater header "
                                f"\"{inc}\" (use {FACADE} only)")
        for ref in FORBIDDEN_FACADE_REFS:
            if ref in text:
                problems.append(f"{rel}: references '{ref}' (raw updater "
                                f"internals; go through {FACADE})")
    return problems


def check_startup_order():
    """updater::startup() must be called before initializeApplication()."""
    rel = "main.cpp"
    path = REPO_ROOT / rel
    if not path.is_file():
        return [f"{rel}: missing (cannot verify updater startup order)"]
    text = read_text(rel)
    startup = text.find("updater::startup()")
    init = text.find("initializeApplication(")
    if startup < 0:
        return [f"{rel}: no 'updater::startup()' call found"]
    if init < 0:
        return [f"{rel}: no 'initializeApplication(' call found"]
    if not startup < init:
        return [f"{rel}: updater::startup() (offset {startup}) is not before "
                f"initializeApplication() (offset {init}) -- startup must run "
                f"before the window exists"]
    return []


def check_modal_before_home():
    """updater::render() runs before renderWelcomeScreen(), and the welcome
    gate is suppressed while a modal is up (!updaterModal)."""
    rel = "ui/app_loop.cpp"
    path = REPO_ROOT / rel
    if not path.is_file():
        return [f"{rel}: missing (cannot verify updater modal ordering)"]
    text = read_text(rel)
    problems = []

    render = text.find("updater::render()")
    welcome = text.find("renderWelcomeScreen(")
    if render < 0:
        problems.append(f"{rel}: no 'updater::render()' call found")
    if welcome < 0:
        problems.append(f"{rel}: no 'renderWelcomeScreen(' call found")
    if render >= 0 and welcome >= 0 and not render < welcome:
        problems.append(f"{rel}: updater::render() (offset {render}) is not "
                        f"before renderWelcomeScreen() (offset {welcome})")

    lines = text.splitlines()
    gate = next((i for i, ln in enumerate(lines)
                 if "showWelcomeScreen" in ln and "welcomeScreenInitialized" in ln),
                None)
    if gate is None:
        problems.append(f"{rel}: no welcome-screen gate line containing both "
                        f"'showWelcomeScreen' and 'welcomeScreenInitialized'")
    else:
        window = lines[gate:gate + 3]  # the gate line plus the following 2
        if not any("!updaterModal" in ln for ln in window):
            problems.append(f"{rel}: welcome-screen gate (line {gate + 1}) "
                            f"does not test '!updaterModal' within the line "
                            f"or the following 2 lines -- the home screen must "
                            f"wait for the updater modal")
    return problems


def check_app_no_updater_state():
    """core/app_state.h and core/config.h carry no updater state."""
    problems = []
    for rel, tokens in NO_UPDATER_STATE_TOKENS.items():
        path = REPO_ROOT / rel
        if not path.is_file():
            problems.append(f"{rel}: missing (cannot verify no updater state)")
            continue
        text = read_text(rel)
        for token in tokens:
            if token in text:
                problems.append(f"{rel}: mentions '{token}' (updater state "
                                f"belongs in updater/, not the app)")
        for inc in quoted_includes(text):
            base = inc.replace("\\", "/").split("/")[-1]
            if base.startswith("updater"):
                problems.append(f"{rel}: includes updater header \"{inc}\" "
                                f"(updater state must not reach app core)")
    return problems


def check_names(files):
    """Every file must be updater.h or updater_*.*."""
    bad = [f.name for f in files
           if f.name != "updater.h" and not f.name.startswith("updater_")]
    return [f"updater/{f}: file name is not 'updater.h' and does not start "
            f"with 'updater_'" for f in bad]


def check_includes(files):
    """Quoted includes stay inside the updater (version.h excepted)."""
    problems = []
    for f in files:
        text = f.read_text(errors="replace")
        for inc in INCLUDE_RE.findall(text):
            base = inc.replace("\\", "/").split("/")[-1]
            if base.startswith("updater") or base in INCLUDE_ALLOW_EXTRA:
                continue
            problems.append(f"updater/{f.name}: includes project header "
                            f"\"{inc}\" outside the updater directory")
    return problems


def check_app_symbols(files):
    """No app state, app config or UI helper names in the updater sources."""
    problems = []
    for f in files:
        text = f.read_text(errors="replace")
        for token in FORBIDDEN_TOKENS:
            if token in text:
                problems.append(f"updater/{f.name}: mentions '{token}' "
                                f"(app/UI dependency in the updater)")
    return problems


def check_namespaces(files):
    """Every .cpp/.h declares namespace updater.

    Exemption: *_compat.h files are C/pthread shims whose declarations MUST stay
    in the global namespace (libstdc++'s <mutex> references pthread_cond_clockwait
    and pthread_mutex_clocklock by unqualified name), so they are skipped here.
    """
    problems = []
    for f in files:
        if f.suffix not in (".cpp", ".h"):
            continue
        if f.name.endswith("_compat.h"):
            continue  # global-namespace C/pthread shim -- see docstring
        if not NAMESPACE_RE.search(f.read_text(errors="replace")):
            problems.append(f"updater/{f.name}: no 'namespace updater'")
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--binary", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--input", required=True)
    ap.add_argument("--golden", required=True)
    args = ap.parse_args()
    workdir = Path(args.workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    t0 = time.monotonic()

    def finish(status, summary, checks, rc):
        record = {"test": TEST_NAME, "status": status, "summary": summary,
                  "duration_s": round(time.monotonic() - t0, 3),
                  "output_type": "Updater isolation",
                  "comparisons": checks, "artifacts": []}
        (workdir / "result.json").write_text(json.dumps(record, indent=2))
        print(json.dumps({"status": status, "test": TEST_NAME,
                          "summary": summary}))
        return rc

    if not UPDATER_DIR.is_dir():
        return finish("error", f"missing directory {UPDATER_DIR.name}/", [], 2)

    files = sorted(p for p in UPDATER_DIR.iterdir() if p.is_file())
    sources = [f for f in files if f.suffix in (".cpp", ".h")]
    if not sources:
        return finish("error", "no .cpp/.h sources under updater/", [], 2)

    checks = []
    problems = []
    for name, fn in (("file_names", lambda: check_names(files)),
                     ("includes", lambda: check_includes(sources)),
                     ("app_symbols", lambda: check_app_symbols(sources)),
                     ("namespaces", lambda: check_namespaces(sources)),
                     ("app_facade", check_app_facade),
                     ("startup_order", check_startup_order),
                     ("modal_before_home", check_modal_before_home),
                     ("app_no_updater_state", check_app_no_updater_state)):
        found = fn()
        checks.append({"name": name, "status": "fail" if found else "pass"})
        problems.extend(found)

    (workdir / "run.log").write_text(
        f"scanned {len(sources)} source file(s) in {UPDATER_DIR}\n" +
        "\n".join(problems) + ("\n" if problems else ""))

    if problems:
        summary = f"{len(problems)} isolation violation(s): " + "; ".join(problems)
        return finish("fail", summary, checks, 1)
    return finish("pass", f"all {len(checks)} isolation checks passed", checks, 0)


if __name__ == "__main__":
    sys.exit(main())
