#pragma once

// Opens the embedded user manual in the default system browser. The manual
// (docs/user_manual.html + docs/screenshots/**) is embedded at build time and
// extracted once per run into a per-process temp directory. Safe to call
// repeatedly; the second call reuses the extraction and re-opens the browser.
void openUserManual();

// Removes this run's extracted manual temp directory. Call on application exit.
// Best-effort: on Windows the browser may still hold handles, in which case the
// directory is left for pruneStaleUserManualTemp() to collect next start.
void cleanupUserManualTemp();

// Sweeps leftover manual temp directories from crashed/previous runs. Only
// removes directories whose owning process is no longer alive (falling back to
// age for unparseable names), so a concurrently running instance is never
// disturbed. Call once at startup.
void pruneStaleUserManualTemp();
