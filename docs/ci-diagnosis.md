# CI diagnosis — Field Compass

Deliverable of #219, under Epic #218, Feature #217. **Read-only research.** No workflow YAML
was written; this is the evidence the rest of #217 gets planned from.

Captured 2026-09-05 against `main` at `5223000`.

## Provenance

| tag | meaning |
|---|---|
| `[TOOL]` | Executed and observed — compiler, `gh`, `ls`, real output |
| `[READ]` | Read the file directly |
| `[REASONED]` | Inference. Not executed. Treat as a hypothesis |

Two regex-derived claims were reported as fact earlier in this project's history and both
were wrong. Anything not marked `[TOOL]` should be treated as unverified.

---

## Headline: CI is viable today, and #150's stated blocker is gone

`[TOOL]` **A clean build succeeds from an empty PlatformIO core directory.**

```
PLATFORMIO_CORE_DIR=<empty dir>  pio run -e feather_s3
[SUCCESS] Took 183.86 seconds
RAM:   103232 bytes (31.5%)
Flash: 1859542 bytes (64.5%)
0 error lines
```

Every dependency installed from `lib_deps` into the empty core dir, including
`boschsensortec/bsec2@1.10.2610` and `BME68x@1.3.40408`. `src/src.ino.cpp.o` compiled and
the image linked.

This matters because **#150 names as its "real work"** the fact that `lv_conf.h` and
TFT_eSPI's `User_Setup.h` lived outside the repo where CI could not see them. Epic B closed
both — `lv_conf.h` vendored at `include/lv_conf.h` (#158), TFT_eSPI moved to `build_flags`
(#184), 15 libraries pinned in `lib_deps` (#185). **#150's scope is now mostly stale: what
remains is the workflow itself, not a dependency problem.**

### The one thing that could have broken it, and didn't

`[READ]` `src/src.ino:44` has an unguarded include into a library's internal tree:

```c
const uint8_t bsec2_config[] = {
  #include "config/bme680/bme680_iaq_33v_3s_4d/bsec_iaq.txt"
};
```

`[TOOL]` That path exists in the OneDrive sketchbook at
`…/Arduino/libraries/bsec2/src/config/…` and **not** in the repo — so it looked like exactly
the machine-local dependency #150 warned about. The clean build proves PlatformIO resolves it
from the `lib_deps` copy instead. **Worth keeping in mind: it is a hard dependency on the
internal directory layout of a third-party library**, and a bsec2 upgrade that reorganises
`src/config/` breaks the build with a confusing error.

---

## Q1–Q3 — can CI build this, and what does it cost

| question | answer |
|---|---|
| Q1 clean build works? | `[TOOL]` **Yes.** Empty core dir, 0 errors |
| Q2 machine-local deps? | `[TOOL]` **None that affect the build.** See doc drift below |
| Q3 cost? | `[TOOL]` **~184 s compile.** Package download added ~290 s on the cold run |

**Caching is worth it but not required.** A cold CI run is roughly 5–8 minutes wall-clock,
most of it downloading the ESP32 toolchain and framework. `[TOOL]` 21 libraries were installed
(15 direct from `lib_deps`, 6 transitive).

**Do not cache the core directory.** `[TOOL]` Measured after the clean build:

| path | size |
|---|---|
| **total core dir** | **5.6 GB** |
| `packages/` (unpacked toolchain + framework) | 3.4 GB |
| `tools/` | 1.5 GB |
| `dist/` (the downloads) | 428 MB |
| `platforms/` | 22 MB |

Caching 5.6 GB would cost more to upload and restore than re-downloading, quite apart from
GitHub's cache limits. meshcore caches `~/.cache/pip` and `~/.platformio/.cache` — the
**downloads**, not the unpacked tree — which is the correct shape and is why their composite
action is worth copying rather than reinventing.

### Doc drift found (not a build blocker)

`[TOOL]` Four comments still claim TFT_eSPI pins come from `User_Setup.h`:
`src/src.ino:38`, `:106`, `:107`, `:259`. Wrong since #184 moved that configuration to
`build_flags` with `USER_SETUP_LOADED=1`. Same class of defect as #149 — documentation
asserting behaviour the repo no longer implements. Cheap to fix; should not ride along in a
CI change.

### An unexplained delta, and it bears on #188

`[TOOL]` The clean-core build and the dev-box build of the same source disagree:

| build | Flash | RAM |
|---|---|---|
| dev-box core dir (#186 builds) | 1,855,926 (64.4%) | 103,232 |
| clean isolated core dir | 1,859,542 (64.5%) | 103,232 |
| delta | **+3,616 bytes** | 0 |

Same pinned `lib_deps`, same source, identical RAM. `[REASONED]` Candidates: different
resolved toolchain or framework patch versions between the two core directories, or build-path
length differences affecting embedded strings — the isolated path is far longer. **Not
proven.**

**This is directly relevant to #188**, which exists to explain a 68 KB PlatformIO/arduino-cli
delta. This finding shows *build environment alone* can move flash by kilobytes on identical
source. #188 must control for environment or it will chase the wrong variable. It also means
**CI output will not be byte-identical to a local build**, which E4's verification harness
must account for — compare like-for-like, or compare symbol sets rather than totals.

---

## Q4–Q7 — what shape should the workflows take

### `[TOOL]` meshcore-firmware — three patterns worth taking

1. **Composite action** `.github/actions/setup-build-environment`: `actions/cache@v5` over
   `~/.cache/pip` and `~/.platformio/.cache` keyed `${{ runner.os }}-pio`,
   `actions/setup-python@v6`, `pip install --upgrade platformio`. Directly reusable.
2. **`config-lint` is an invariant self-test suite, not config linting.** Jobs like *"Every
   ESP32 env compiling SerialBLEInterface.cpp declares NimBLE"* and *"Env capability-claim
   self-test"*, several marked **(advisory)**, with reports uploaded as artifacts. The stated
   rationale: a NimBLE defect recurred four times because the affected envs were neither in
   CI nor able to fail a release. **It asserts a rule instead of sampling.**
3. **"Envs are DISCOVERED, not listed."** The test job enumerates envs rather than hardcoding
   them.

### `[TOOL]` meshcore-client — the structural precedent for the review gate

Flutter/Dart, so its build CI does not transfer. **`release-gate.yml` does**, and it is the
closest existing analogue to what #217 wants:

> "Fails a PR whose pubspec version lacks any of the four human-authored release texts…
> This runs on EVERY PR so a release-cut PR goes red before merge if a note is missing. It is
> cheap and platform-agnostic… rc.2 and rc.3 shipped with only a pubspec bump. **This makes
> that unmergeable.**"

Thin YAML, logic in `.github/scripts/release-gate.sh`. `[TOOL]` That repo has a
`.github/scripts/` convention (`release-gate.sh`, `check_no_emdash.py`); **Field Compass has
none — its workflows are inline.** Adopting the script convention is new here but established
in the sibling repo.

### Recommendations

- **Q6 — build matrix: not yet, but discover rather than list.** One env today
  (`feather_s3`), UM FeatherS3 coming. A matrix of one is noise, but **discovering envs from
  `platformio.ini` costs the same to write and means the UM env is covered the day it lands**
  rather than being the env nobody added. This directly prevents meshcore's stated defect
  class.
- **Q7 — a `config-lint` job earns its place, but not for cross-env invariants.** With one
  env there is nothing to compare. The invariants worth asserting here are different: that
  `CLAUDE.md`'s documented build command matches `platformio.ini`, that the `version-bump.yml`
  path filter still matches where the source actually lives (it was wrong after #186 and
  would have silently stopped versioning firmware), and the review gate below. Same family —
  *assert a rule, cheaply, on every PR*.

---

## Q8–Q13 — enforcing `llm-consult.py` review

**Decided by the owner:** keep `scripts/llm-consult.py` as the review; add CI that verifies
it ran against the merged code. Do not move the review into CI — *"after it's in the CI
pipeline, it seems to get lip service at best."*

`[TOOL]` That is confirmed by inspection: Unfocused's `gemini-review.yml` is named *warn-only*
and ends `run: exit 0`. Nothing obliges anyone to answer it. By contrast, `/pushpr` step 3
requires every finding to be **fixed or justified in the PR body** — and that produced a real
catch on #186, where the review rejected an unsupported behaviour-neutrality claim and led to
the symbol-level comparison that found 608 shifted addresses.

### Q8 — what the committed artifact should contain: **a digest, not the full log**

`[TOOL]` Measured over the 5 existing logs (140 KB total, 14–55 KB each). For the #186 log:

| part | chars | |
|---|---|---|
| `prompt` | 43,673 | **80%** — bundles full file contents |
| `response` | 7,636 | the findings |
| everything else | 651 | metadata |

The prompt is a **copy of source already in the repo**, duplicated on every review. A digest —
timestamp, topic, backend, model, per-file path + content SHA, and the response — is ~8 KB
instead of 55 KB and keeps everything the gate and a human reader need. The prompt is
reconstructible from the hashes plus the repo.

### Q9 — commit it: **yes**

`[TOOL]` `docs/llm-consultations/` holds 5 logs and **0 are tracked in git**. CI can only see
committed files, so committing is not optional if the gate lives in CI — and it makes the
review evidence part of the PR diff, which is where the owner will actually read it.

### Q10 — where the gate lives: **CI**

A pre-push hook is bypassable with `--no-verify`; CI is not. The check reads committed files,
needs no API key and no secrets, so it works on fork PRs. Content hashes survive rebase, since
rebasing rewrites commit SHAs but not file content.

### Q11 — what it must assert

For every changed **source** file in the PR, a committed digest exists whose recorded hash
equals that file's content in the PR head. Details to settle in implementation:

- Scope: `src/**`, `include/**`, `lib/**`, `scripts/**`, `.claude/hooks/**`. Docs-only and
  workflow-only PRs exempt, or the gate blocks its own introduction
- The digest file must not require covering itself
- Deletions need no digest; renames are hashed at the new path
- `[TOOL]` **Hash the bytes as committed, and normalise line endings.** #205 was exactly this
  failure — a CRLF checkout made `read -r` produce `Field_Compass\r` and the gate reported a
  Citadel outage. A content hash that differs between checkouts would fail the same way
- The failure message must name **which** file drifted

### Q12 — relationship to `/pushpr` step 3: **complementary, and say so**

The step's value is that findings must be addressed; the gate's value is that it cannot be
skipped. Neither replaces the other. `/pushpr` step 3 should stay, with wording updated to
note that CI will verify the consultation exists.

### Q13 — cost

The check itself is free. `[TOOL]` The #186 consultation cost $0.0247 (12,947 in / 1,694 out,
46 s). Re-running after fixing findings is the honest behaviour, so budget 2–3 invocations per
substantial PR, well under $0.10.

---

## Q14–Q15 — gating policy

- **Q14 — advisory first, then required.** Land the compile check advisory, confirm it goes
  red on a deliberately broken sketch (#150's own acceptance criterion), then promote to
  required via branch protection. Note the interaction with standards#359: the close-before-push
  gate already forces issues closed before CI has ever run, so a required check that fails
  leaves an issue closed against a red branch. **That ordering problem should be resolved
  before the check becomes required.**
- **Q15 — the human hardware gate stays.** CI proves it compiles; it cannot prove the display
  renders or the GPS locks. `[TOOL]` #166 is the standing example — a whole dead SPI bus that
  compiled perfectly. CI goes *before* hardware verification in the sequence, not instead of it.

---

## Incidental finding worth recording

`[TOOL]` **PlatformIO refuses to run under MSYS/Git Bash** — `ERROR: MSys/Mingw is not
supported`. The first clean-build attempt failed for this reason and briefly looked like a
repo problem. Irrelevant to CI (ubuntu-latest), but **E4's task 1.4 verification harness must
invoke `pio` from PowerShell or cmd**, not from the Bash tool. Cheap to write down, expensive
to rediscover.

---

## Proposed follow-on structure

Proposed only. The owner grants structure.

**Epic — compile gate**
- Composite action with pip + PlatformIO caching
- `compile.yml`: `pio run` on push and PR, envs **discovered** from `platformio.ini`, paths
  filter `src/** include/** lib/** platformio.ini partitions/**`
- Verify it blocks: push a deliberate syntax error, confirm red, revert
- Correct `CLAUDE.md`'s CI-gate row; close #150

**Epic — review gate**
- Add per-file content hashes to `llm-consult.py`; emit a digest alongside the full log
- Commit digests under `docs/llm-consultations/`
- `.github/scripts/verify-consultation.sh` + thin workflow, following meshcore-client's
  `release-gate.yml` shape
- Update `/pushpr` step 3 wording

**Epic — invariant checks (config-lint equivalent)**
- `CLAUDE.md` build command matches `platformio.ini`
- `version-bump.yml` path filter matches where source actually lives
- Advisory first

**Deferred to after E4 (#212)** — native unit tests. `pio test -e native` needs code separable
from hardware; a monolithic `.ino` reaching into 111 file-scope globals is not natively
testable. Testability is a product of the decomposition, not a prerequisite.

---

## Owner decisions

**1. The +3,616 byte environment delta folds into #188.** It is the same question at a
smaller scale — why do builds of identical source disagree — and #188 already owns it. #188's
investigation must therefore control for build environment, not just toolchain.

**2. Adopt the `.github/scripts/` convention.** New for Field Compass, whose three workflows
are inline today; established in meshcore-client (`release-gate.sh`, `check_no_emdash.py`).
Gate logic lives in a script, the workflow YAML stays thin.

**3. The review gate covers `.claude/hooks/**`.** Governance code gets mandatory review.

`[TOOL]` Coverage is narrower than it first appears, and worth stating precisely so nobody
over-trusts it. Only six files under `.claude/` are tracked in this repo:

```
.claude/hooks/block-raw-flash.py        .claude/hooks/block-registry-edit.py
.claude/hooks/dw-project                .claude/hooks/session-state.sh
.claude/settings.json                   .claude/settings.local.json
```

The canonical hooks — `require-checkpoint.py`, `block-bash-file-mutation.py`,
`block-primary-clone-edit.py`, `pre-commit`, `pre-push` and the rest — are distributed by
DifferentWire/standards and are **not tracked here**, so a Field Compass gate cannot cover
them. Their review belongs upstream.

Against the four hook defects found on 2026-09-05:

| defect | file | covered? |
|---|---|---|
| #195 backtick false positive | `block-raw-flash.py` | **yes** |
| #205 CRLF marker regression | `.gitattributes`, `dw-project` | **yes** |
| #208 redirect false positive | `block-registry-edit.py` | **yes** |
| #203 newline bypass | `block-bash-file-mutation.py` | no — canonical, upstream |

Three of four. **Exclude `.claude/settings.local.json`** — it is machine-local permission
churn, changes constantly, and requiring review of it would train people to override the
gate.
