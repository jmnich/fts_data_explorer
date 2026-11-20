---
test: test20_updater_isolation
output_type: Updater isolation
requires_golden: false
---
# Test 20: Updater Isolation + Architecture Guards
## What this test verifies
Static (Python-only, no compilation) checks.

**Module isolation** — `updater/` stays a self-contained module:
- **File names**: every file is `updater.h` or starts with `updater_`
- **Includes**: a quoted `#include "..."` in a `.cpp`/`.h` under `updater/`
  may only name an `updater*` header or the generated `version.h` — no
  project header from another directory
- **No app/UI coupling**: no `.cpp`/`.h` under `updater/` mentions `AppState`,
  `AppConfig`, `appState`, `popup_utils`, `wrap_text`, `open_default` or
  `conversion_screen`
- **Namespace**: every `.cpp`/`.h` under `updater/` declares
  `namespace updater` — except `*_compat.h` shims, whose C/pthread
  declarations must stay in the global namespace (libstdc++'s `<mutex>`
  references `pthread_cond_clockwait`/`pthread_mutex_clocklock` unqualified)

**Architecture guards** — the app talks to the updater only through its
public facade, in the right order, and keeps no updater state of its own:
- **App facade** (`main.cpp`, `ui/app_loop.cpp`, `ui/menu_bar.cpp`): each
  `#include "updater.h"`, and none includes an internal `updater_*.h` header
  or names `updater::UpdateService`, `updater::Config` or `updater::state(`
- **Startup order** (`main.cpp`): the first `updater::startup()` appears
  before the first `initializeApplication(` (startup runs before the window)
- **Modal-before-home** (`ui/app_loop.cpp`): the first `updater::render()`
  appears before the first `renderWelcomeScreen(`, and the welcome-screen
  gate line (holding both `showWelcomeScreen` and `welcomeScreenInitialized`)
  tests `!updaterModal` on that line or within the following two lines
- **No updater state in the app**: `core/app_state.h` does not mention
  `UpdateDialog`/`Updater` or include an updater header; `core/config.h` does
  not mention `updateRepoUrl`, `skippedUpdateVersion` or `[Updates]`
## Method
Lists `updater/` from the repo root and runs the four isolation checks over
its sources, then greps the named app sources for the four architecture
guards; reports one comparison per check (eight total). Any violation fails
the test with the offending file and reason in the summary; a missing
`updater/` directory or an empty one is an error. No binary, no files are
written.
## Timeout
timeout: 60
## Dependencies
- Python 3 stdlib only (no packages, no C++ compiler, no build tree)
