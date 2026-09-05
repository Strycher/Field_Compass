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
inside. PlatformIO's Library Dependency Finder picks it up automatically, and a
library here takes precedence over one of the same name in `lib_deps`.

Two caveats on that precedence, because it is easy to over-trust:

- It holds for the LDF's default `chain` mode. `platformio.ini` does not set
  `lib_ldf_mode`, so the default applies — but if that ever changes, re-check
  this claim rather than assuming.
- Precedence covers the vendored library itself, **not its dependencies**. If a
  vendored library pulls in something that is also in `lib_deps` at a different
  version, the LDF resolves that transitively and you can end up with two
  versions of the same code in one image, or a silent downgrade. Vendoring a
  leaf library is safe; vendoring one with its own dependency tree needs the
  resulting `pio run -v` dependency graph read carefully before it is trusted.
