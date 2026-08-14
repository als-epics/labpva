# labpva changelog

## 2026-08-12 — PVXS backend: warm puts are ~6x faster (one round-trip, like pvac)

A `PVXS=...` build was far slower at repeated puts than a classic build — a user
loop of `pvaPut(pvlist, [0;0;0])` over 3 PVs x 10000 iterations took 27.2 s on
PVXS versus 4.4 s on pvAccessCPP. Cause: **round-trips per put**, not PVXS
itself. A pvxs `client::Operation` is a one-shot state machine, so each
`pvaPut` paid a get (INIT + GET, just to learn the channel's type) *plus* a put
(INIT + PUT) = ~4 server round-trips, where pvaClient's cached `PvaClientPut`
needs 1. Measured per warm scalar put on a loopback softIocPVA: 250 µs (old
PVXS shape) vs 32 µs (pvac).

Both halves are fixed, in the glue only — no MATLAB-visible behaviour change:

- **The pre-put get is gone.** `pvaPutPrototype` (new, `pvaGlue.h`) hands the
  MEX layer the channel's type description to build the argument against;
  `mxToPutArg`/`mxToPutArgStructure` take it as `proto`. Only an enum channel
  needs values (its choice list), and those are fetched once per INIT.
- **One put Operation is cached per channel** (`g_putOps` in
  `pvaGlue_pvxs.cpp`), created with the pvxs expert API's `.autoExec(false)` +
  `.onInit(...)`; each later put is a single `Operation::reExecPut`.
  `pvaPutExec` sends the pre-built argument through it.

Result: **41 µs per warm put** (6.1x faster, parity with pvac's 32 µs), and one
round-trip per put on the wire. The classic backend is untouched; the read path
is unchanged (`pvaGet` still creates a Get per call — same optimisation applies,
not done here).

Robustness of the cache (all verified live, 22/22 harness checks — scalar /
waveform / string / enum-by-choice / boolean puts, whole-structure put,
`pvaPutNoWait`, no-wait burst, nonexistent PV, IOC restart):

- `.onInit` re-fires after a reconnect and bumps a generation counter, so an
  **IOC restart** — even one that changes the channel's type — refreshes the
  cached prototype with no user action.
- `reExecPut` silently does nothing unless the operation is idle, so a put
  already in flight on that channel (a preceding `pvaPutNoWait`), a
  disconnected channel, or a pvxs too old for the expert API all **fall back**
  to the previous one-shot put rather than dropping the write. The fallback's
  whole-structure pvRequest matches the fallback prototype's layout.
- A `wait` put whose completion never arrives drops the cached operation and
  reports `pvaPut '<name>': timeout` (the next call rebuilds it). A put to an
  unreachable channel now reports `pvaPut '<name>': Timeout`, matching the
  classic backend's wording.
- Callbacks still run on pvxs worker threads and still never touch MATLAB
  memory: they copy a `Value`/string, set a flag, signal an `epicsEvent`.

Requires pvxs >= 1.3 for the cached path (`PVXS_ENABLE_EXPERT_API`, defined
ahead of every pvxs header); older pvxs compiles the registry out and keeps the
old one-shot behaviour.

## 2026-08-10 — pvaPut/pvaPutNoWait: single-value broadcast over a list of PVs

`pvaPut({'PV1','PV2',...}, value)` now accepts **one** value and writes it to
**every** PV in the list, matching `lcaPut` (whose value matrix may have 1 or M
rows): `pvaPut(correctors, 0)` zeroes the whole list without building a vector
of zeros. Previously only "one value per PV" was accepted (numeric vector or
cell with `numel == #PVs`); a scalar against N>1 PVs was an error. Applies to
both backends and to `pvaPutNoWait`.

- Broadcast covers a numeric/logical scalar, a char row (e.g. an enum choice
  string), a struct, and a 1x1 cell — so `pvaPut(wfPVs, {[1 2 3]})` writes the
  same waveform to every array PV in the list.
- Per-PV forms are unchanged: a numeric vector with one element per PV still
  distributes element-wise (including the H1 non-double fix), as does a cell of
  that length.
- A list holding a *single* PV now takes the whole value argument, so
  `pvaPut({'PV'}, [1 2 3])` writes a waveform like `pvaPut('PV', [1 2 3])`
  instead of failing the length check.
- Length mismatches are still an error (never a guess), with a message that now
  names all three accepted forms. Complex values are rejected for every list
  form, not just the element-wise one.

## 2026-08-05 — deep code review of the dual-backend port: fixes + known differences

A full review (two independent passes over the new backend code plus targeted
source/live verification) produced these fixes, all verified against a live
softIocPVA (fix regression suite + the full 27-check put/monitor suite re-run):

- **Multi-PV put element bug (H1):** `pvaPut({'PV1','PV2'}, int32([10 20]))`
  (any non-double numeric class) silently wrote element 1 to every PV —
  `putValueFor` now converts per element for every numeric class, and rejects
  complex values and unsupported classes with a clear error. (Both backends.)
- **Backend-switch safety (H2):** all objects/MEX now depend on the
  `configure/` files, so toggling `PVXS` in `configure/RELEASE` rebuilds
  everything automatically (no `make clean` needed); additionally every MEX
  references a backend "link guard" symbol (`backendGuardPvac`/`...Pvxs`)
  defined only by the matching `libmpvaglue.so`, so a mismatched MEX/.so pair
  now fails to load with a clear undefined-symbol error instead of silently
  corrupting memory (the dangerous `pvaGet` differs between backends only in
  return type, which C++ mangling does not encode). Legacy pre-split
  `pvaGlue.o`/`pvaConvert.o` are removed by `clean`.
- **Empty-value crashes (H3):** `pvaPut(pv, [])` (scalar or enum target) could
  dereference NULL via `mxGetScalar`; now rejected with `labpva:typeMismatch`.
  Same guard on the trailing `poll` argument of `pvaGet`/`pvaGetStructure`.
  (Both backends.)
- **Exception barrier (M1, pvxs):** all public marshalling entry points now
  catch C++ exceptions (e.g. `std::bad_alloc` on a huge array) and report a
  labpva error instead of terminating MATLAB. pvac's `getDoubleField`/
  `getStringField` also no longer throw on nonconforming servers.
- **Pending-put cap (M2, pvxs):** parked `pvaPutNoWait` operations (a put to a
  dead PV never completes) are now bounded at 256; beyond that the oldest is
  cancelled — no more unbounded growth against a down IOC.
- **Rich-PV cache guard (M3, both backends):** `pvaGet` on a structured/rich PV
  (NTNDArray/NTTable/group) with a *narrow* monitor active no longer serves a
  partial structure from the cache — the value verb serves the cache only when
  the monitor covers the request or the cached sample carries a plain
  scalar/array/enum `value`; otherwise it reads fresh.
- **Relocatable MEX (M7):** the MEX rpath now puts `$ORIGIN` before the
  absolute bin path, so a COPIED tree resolves its own `libmpvaglue.so` (the
  established clone-a-copy site pattern previously kept loading the original
  tree's library).

**Known differences documented, not changed:** pvac's `pvaPutStructure` marks
the whole structure and (due to pvaClient's cached put handle) can resend
stale values for fields you didn't set — pvxs sends only the fields your
struct touched; `pvaNewMonitorWait` consumes one event on pvac but drains-keep-
newest on pvxs; pvxs `pvaPutStructure` request scoping is top-level only; a
parked no-wait put that reconnects after an IOC reboot with a *changed* type
can write wrongly (rare); `'C'` on numeric arrays formats via `%g` on pvxs;
timeouts are currently reported as `labpva:failure`/`notConnected` rather than
`labpva:timeout`.

## 2026-07-31 — PVXS backend complete: puts + monitors (Phases 3-4)

The PVXS backend now implements the full verb set; a `PVXS=...` build is
functionally equivalent to the classic one except for the documented
`pvaSetProvider('ca')` limitation. Verified live against a softIocPVA with the
MATLAB-free harness (27/27 checks): scalar/string/waveform puts, enum puts by
choice string and by bounds-checked index (bad index/string rejected),
pvaPutNoWait, pvaPutStructure (only fields present in the struct are written;
an optional `field(a,b)` request scopes by top-level field via unmark), and the
whole monitor lifecycle (set → initial snapshot event → quiet poll/wait →
event after a put → cache-served pvaGet → clear; NOMONITOR error preserved).

Design notes:
- Puts pre-build the argument Value on the MATLAB thread (`mxToPutArg` /
  `mxToPutArgStructure` in pvaConvert_pvxs.cpp): a fresh get supplies the
  server type (and enum choices) — *superseded 2026-08-12: the type now comes
  from a cached put operation's INIT, with no get* — the argument starts as
  `cloneEmpty()` and
  assignment marks exactly the written fields — pvxs sends only marked fields.
  The PutBuilder callback (a pvxs worker thread) only returns the pre-built
  Value; it never touches MATLAB memory.
- pvaPutNoWait parks its Operation handle (dropping it would cancel the put)
  in a registry; a result callback records completion and the handle is reaped
  later on the MATLAB thread.
- Monitors use client::Subscription with connection events masked; the FIFO
  not-empty callback only signals an epicsEvent (pvaNewMonitorWait waits on
  it with a deadline). Same drain-keep-latest cache semantics as pvac. NOTE:
  subscribing is asynchronous — pvaSetMonitor on a nonexistent PV does not
  error (the poll just never reports a sample); the pvac backend errors there.

## 2026-07-30 — PVXS backend: read path implemented and verified (Phase 2)

`glue/pvaGlue_pvxs.cpp` + `glue/pvaConvert_pvxs.cpp` now exist, so a
`PVXS = /path/to/built/pvxs` build compiles and the READ side works over
libpvxs: `pvaGet`/`pvaGetStructure` (incl. smart-pvaGet and the poll flag),
`pvaInfo`, all nine metadata getters, `pvaChannels`/`pvaIsConnected`
(client::Connect-based), timeout/provider/debug config. Verified live against
a softIocPVA (labpvaTest.db) with a MATLAB-free harness driving the real glue +
marshaller: NTScalar (full nested struct incl. display/control/valueAlarm and
the display.form enum sugar), NTEnum (choice string / 'D' index / choices),
NTScalarArray, strings, booleans, timestamps, and the error paths (timeout,
bad PV, provider refusal).

Notes discovered during bring-up:
- pvxs's `pvRequest("field(a,b)")` string parser does not treat the comma list
  as fields; the glue translates `field(a,b,...)` into builder `.field()` calls.
- Against a QSRV1 server, scoped GETs are not subset (the full structure is
  transferred) even via pvxs's own `pvxget -r` — correctness is unaffected;
  the pvac backend keeps the bandwidth saving; QSRV2 servers honour subsets.
- `pvaSetProvider` now returns success (bool): the PVXS backend refuses 'ca'
  and the MEX raises `labpva:unsupported` (PVXS has no CA provider).
- Puts (`pvaPut*`) and monitors (`pvaSetMonitor` family) raise a clear
  `labpva:unsupported` error under PVXS until Phases 3-4.
- Added `doc/pvaBenchmark.m` for classic-vs-PVXS read timing A/B.

## 2026-07-30 — dual-backend plumbing: optional PVXS client (Phase 1 of the port)

The glue layer can now be built against either pvAccess implementation,
selected in `configure/RELEASE`: leave `PVXS` commented out for the classic
pvaClient/pvAccessCPP stack (the default — behavior unchanged), or set
`PVXS = /path/to/built/pvxs` to compile against libpvxs (`-DLABPVA_USE_PVXS`).

- `glue/pvaBackend.h` (new): `labpva::PvValue` — the one structure handle that
  crosses the glue boundary (`PVStructurePtr` on pvac, `pvxs::Value` on pvxs).
- Backend split: `pvaGlue.cpp` → `pvaGlue_pvac.cpp`, `pvaConvert.cpp` →
  `pvaConvert_pvac.cpp`; the Makefiles build `pva{Glue,Convert}_$(LABPVA_BACKEND).o`.
- Interface neutralized: all read-path MEX no longer reference `epics::pvData`
  — `pvaGetNelem`/`pvaGetEnumStrings`/`pvaInfo` now use new convert helpers
  (`pvValueNelem`, `pvEnumChoicesToMx`, `pvTypeId`/`pvIntrospect`);
  `getDoubleField`/`getStringField` moved from mglue (now backend-free) into
  pvaConvert. The write path (pvaPut*/pvaPutStructure and mxToPv*) stays
  pvac-only behind `#ifndef LABPVA_USE_PVXS` until the put phase of the port.
- configure: `ifdef PVXS` selects backend, includes, `-lpvxs -lCom`, libdirs and
  rpaths; a bad `PVXS` path fails at configure time with a clear message.

**Status:** the pvac default rebuilds cleanly on both arches with identical
behavior. The `pvaGlue_pvxs.cpp`/`pvaConvert_pvxs.cpp` backend sources are the
next phases (read path, then puts, then monitors); until they exist, defining
`PVXS` intentionally stops the build at the missing file.

## 2026-06-29 — fix: segfault when quitting MATLAB after a failed connect / pvaClear

**Symptom.** `pvaGet` on a non-existent PV (connect fails), or `pvaClear` on a
name with no active monitor (or repeated `pvaClear`), left MATLAB crashing with a
segmentation violation **on exit** — the crash stack a background thread in
`clone`/`libpthread` jumping to a bad address. A `pvaGet` of an existing PV then
exit was clean.

**Root cause.** labpva never locked its MEX in memory. On quit, MATLAB unloads
the MEX (and with them the EPICS client libraries), but a pvAccess worker thread
(e.g. the periodic channel-search timer left running by an unconnected channel)
is still alive and jumps into code that was just unmapped → segfault. Successful,
fully-connected reads happened to tear down cleanly; the half-connected / no-op
paths did not.

**Fix.** Mirror labca's `CONFIG_MEXLOCK`: call `mexLock()` once on the first
channel-touching call (`labpva::lockMexFile()`, invoked from `buildPVs` — which
covers every verb that names a PV — and from `pvaInfo`). The MEX (and the EPICS
libs it depends on) then stay mapped for the life of the process, so worker
threads never execute unmapped code. We never unlock — like labca, EPICS client
state isn't torn down cleanly mid-session; the process just exits with the libs
loaded. Config-only verbs (timeout/provider/debug/lastError) don't lock.

**Consequence.** `clear mex` no longer unloads labpva. After rebuilding, **restart
MATLAB** (not `clear mex`) to pick up new binaries.

> **2026-06-11 session summary.** First live-IOC bring-up of labpva from MATLAB.
> Fixed the monitor / process-global-state build bug (shared `libmpvaglue.so`),
> isolated the monitor cache from metadata reads, added a working `help` system,
> `printvals`, and hands-free auto-load via `startup.m`. **Verified working live**
> against a softIocPVA serving `mdach:circle`: reads, `pvaGetStructure`, monitors
> (`pvaSetMonitor` → `pvaNewMonitorValue` → cache-served read), the metadata
> getters, NTEnum reads, `help labpva` / `help pvaGet`, and auto-startup are all
> confirmed. **Not yet exercised live:** the write path (`pvaPut` /
> `pvaPutNoWait` / `pvaPutStructure`) and array/string round-trips. Details below.

## 2026-06-11 — code review: monitor-cache isolation + small correctness fixes

A full read-through of the implementation produced these fixes (all built and
**verified working against the live IOC**, 2026-06-11):

- **Monitor cache no longer contaminates structure/metadata reads (the one that
  mattered).** The glue `pvaGet` short-circuited to the cached monitored value
  for *every* caller, ignoring the `request`. So once any monitor was active,
  `pvaGetStructure` returned only the monitored subset, and
  `pvaGetUnits`/`pvaGetPrecision`/`pvaGetControl|Graphic|Alarm|WarnLimits`
  silently returned `""`/`0`/`NaN` (the default monitor request omits
  `display`/`control`/`valueAlarm`). Fix: `pvaGet` gained a
  `useMonitorCache` parameter that **defaults to false** (fail-safe); only the
  `pvaGet` value verb opts in (`true`). Structure, status, nelem, enum-strings,
  metadata, and `pvaInfo` now always read the fields they ask for. ezca-style
  value caching is preserved for the value read.
- **`pvaSetMonitor` stops the previous monitor before re-subscribing** (was
  overwriting the registry entry and relying on the destructor), avoiding a
  lingering server-side subscription on re-subscribe.
- **Type letter case normalised** in `parseTypeArg` (`'n'` → `'N'`, etc.). A
  lowercase `'n'` previously fell through and made `pvaGet(pv,'n')` on an NTEnum
  return the index instead of the choice string, inconsistent with `'N'`.
- **`pvaClear` doc corrected**: it tears down the monitor subscription but the
  pvaClient channel stays cached (the connection persists) — the header had
  claimed the channel was released.

Left as documented notes / accepted tradeoffs (see ARCHITECTURE §7): 64-bit int
→ double precision, scalar-field writes taking the first element of a vector,
the `mexCallMATLAB("double")` longjmp hazard, and confirming waveform row-vs-
column orientation against labca.

## 2026-06-11 — first live-IOC bring-up: monitor/state fix, help system, doc corrections

First time labpva was driven against a live IOC from MATLAB (a softIocPVA
serving `mdach:circle`, an NTScalar-style structure with `angle`/`x`/`y`
sub-signals). Basic reads worked immediately; bringing up monitors surfaced a
build bug. Everything below was discovered and fixed in that session.

### Fixed: process-global state was duplicated per MEX (monitors didn't work)

**Symptom.** `pvaSetMonitor('mdach:circle','field()')` returned without error,
but the next `pvaNewMonitorValue('mdach:circle')` threw
`no monitor set on 'mdach:circle'`.

**Root cause.** `matlab/Makefile` static-linked *all* the glue objects —
including `pvaGlue.o`, which holds the file-scope statics `g_monitors`,
`g_client`, `g_provider`, `g_timeout` — into **every** `pva*.mexa64`. MATLAB
`dlopen`s each MEX as a separate module with private symbols, so each verb got
its **own** copy of that state. The monitor registered in
`pvaSetMonitor.mexa64`'s `g_monitors` was invisible to
`pvaNewMonitorValue.mexa64`'s (separate, empty) `g_monitors`. The same flaw
silently affected `pvaSetProvider`/`pvaGetProvider`, `pvaSetTimeout`/
`pvaGetTimeout`, `pvaClear`, `pvaLastError`, and the "serve `pvaGet` from the
monitor cache" path (its `g_monitors` lookup always missed, so every
`pvaGet`/`pvaGetStructure` was an unintended fresh read). Single, self-contained
calls (`pvaGet`, `pvaPut`) worked, which is why reads looked fine.

**Fix.** Mirror labca's `libezca.so`: the two stateful, MATLAB-symbol-free
objects (`pvaGlue.o` + `pvaError.o`) are now linked into one shared
`bin/<arch>/labpva/libmpvaglue.so` that every MEX dynamically links against
(`-lmpvaglue`, with an rpath to the bin dir). The `mx*`-using helpers
(`pvaConvert.o`, `mglue.o`) are stateless and stay static per-MEX. Result: a
single `g_monitors`/`g_client`/etc. in the MATLAB process, shared across all
verbs. Verified at the binary level: `pvaSetMonitor.mexa64` no longer *defines*
`labpva::pvaMonitorSet` (it's an undefined ref resolved from the `.so`);
`libmpvaglue.so` is the sole definition; each MEX has `NEEDED libmpvaglue.so` +
the rpath, and `ldd` resolves it.

  - `glue/Makefile`: builds `libmpvaglue.so` (plain `g++ -shared`, EPICS libs +
    rpath baked in so the `.so` is self-sufficient).
  - `matlab/Makefile`: dropped `pvaGlue.o`/`pvaError.o` from `GLUE_OBJS`, added
    `-L$(BINDIR) -lmpvaglue` and a second `-Wl,-rpath,$(abspath $(BINDIR))`.

**To pick up the fix in a running MATLAB:** `clear mex` (the old static MEX are
cached in the session) before re-running, or restart MATLAB.

### Confirmed: connections are cached, not reopened per call

Every verb funnels through `PvaClient::channel(name, provider, timeout)`, which
(per pvaClientCPP source) consults a channel cache keyed by `name+provider`:
the **first** access to a PV connects, **all** later get/put/monitor calls reuse
the open connection — the labca/ezca model. `pvaPut`/`pvaPutNoWait`/
`pvaPutStructure` do **not** reconnect each call. This reuse is only session-wide
*because* of the shared-client fix above (previously each verb had its own
`g_client` and thus its own channel cache). Note: a monitor is a read-side
subscription; neither labpva `pvaPut` nor labca `caPut` uses one — what they
reuse is the persistent channel. The caching goes one level deeper:
`PvaClientChannel` also caches its `PvaClientPut`/`PvaClientGet` handles keyed by
the pvRequest string, so the connect + initial value-get happens only on the
*first* put/get to a PV; every later warm `pvaPut` is just the write round-trip
(`pvaPutNoWait` doesn't even wait), and a warm `pvaGet` just the read. (Earlier
draft notes claimed `pvaPut` re-creates its put handle and re-fetches the
structure each call — that was wrong; pvaClientCPP already caches it.)

### Added: a working `help` system for the MEX verbs

MEX files carry no help text, and `help`/`doc` only read a same-named `.m` file
on the path. Added:

  - `doc/gen_help_stubs.py` — generates one `.m` help stub per verb (signature,
    behavior, "See also") into every `bin/<arch>/labpva/` next to the MEX. With
    a `.m` and same-named MEX co-located, MATLAB shows the `.m` help but
    *executes* the MEX (the stub's body `error()`s only if the MEX is missing).
    Re-run after a signature change.
  - `Contents.m` is copied into `bin/<arch>/labpva/` (a folder named `labpva`)
    so `help labpva` shows the grouped verb list.

Now `help labpva`, `help pvaGet`, `doc pvaGet`, and tab-completion all work.

### Added: `doc/printvals.m`

Companion to `printpvs.m`. `printpvs` dumps *every* leaf of a structure
(value, alarm, display, control, valueAlarm, …); `printvals` prints only each
signal's `.value` + a human-readable `timeStamp`, recursing into grouped
sub-signals. Useful for structures like `mdach:circle` where `printpvs` floods
the console with metadata.

### Added: hands-free startup

`~/Documents/MATLAB/startup.m` (MATLAB's default `userpath`, auto-run at launch)
now `addpath`s the labpva bin dir + `doc/`, so `pva*` and `help labpva` work in
every session with nothing typed. No `LD_LIBRARY_PATH` needed — the MEX carry an
`RPATH` to the EPICS lib dir and their own bin dir (`DT_RPATH`, which also covers
transitive deps). The script avoids naming its variable `labpva` (which would
shadow `help labpva`) and clears it afterward. Path + arch are hardcoded; update
the two lines if the repo moves or the build arch changes.

### Doc corrections

  - **Don't name the MATLAB path variable `labpva`/`LABPVA`** — a workspace
    variable shadows `help labpva` (MATLAB resolves variables before folders).
    Use e.g. `labpvaRoot`.
  - **Arch on this host is `RL8-x86_64`, not `linux-x86_64`.** README/skill
    `addpath` examples were corrected (one also had a `…/labca` typo for a
    labpva path).
  - MEX count is **26**, not 24 (README "Status" was stale).
