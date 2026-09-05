# lib/ — vendored libraries

Empty on purpose. Every dependency currently comes from `lib_deps` in
`platformio.ini`, pinned to an exact version (B3, #185).

This directory exists so the PlatformIO layout is complete and so a vendored
library has an obvious home when one is needed. Git does not track empty
directories, which is the only reason this file exists.

## When to vendor instead of pinning

Pin in `lib_deps` by default — it is visible, diffable, and upgradeable in one
line. Vendor here only when pinning cannot express what is needed:

- the upstream library must be patched, and the patch cannot live in our own
  source (see the TFT_eSPI rotation history in #157, where a fork living
  *outside* git was silently reverted by every library upgrade — the failure
  this directory is meant to make impossible)
- the library is not published to a registry PlatformIO can reach
- a specific commit is required and no tag exists

A vendored library needs a note saying which upstream it came from, at what
version or commit, and what was changed. Without that it becomes an
unmaintainable fork, which is worse than the problem it solved.

Layout: `lib/<LibName>/` with the library's own `src/` and `library.json`
inside. PlatformIO's Library Dependency Finder picks it up automatically and it
takes precedence over anything in `lib_deps`.
