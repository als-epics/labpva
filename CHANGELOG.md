# labpva changelog

## 2026-08-17 — native MATLAB NTTable get/put verbs

Added `pvaGetTable`, which recognises `epics:nt/NTTable:*` and returns its
label-ordered columns as a MATLAB table plus the standard complex timestamp
and alarm struct. Added `pvaPutTable`, accepting a MATLAB table, scalar struct
of columns, or field/value pairs; it validates column names and lengths, writes
only `value`, and retains `pvaPut` behavior for non-NTTable channels. Both the
classic pvAccessCPP and PVXS conversion backends implement the same contract.

## 2026-08-17 — classic backend: pvaGet did TWO network round trips per read

Found by the user's own backend A/B benchmark (30000 ops each way against real
IOCs): pvxs put ~= pvxs get ~= pvac put (~8.6-9.3 s), but **pvac get took
18.3 s — exactly double**. Root cause in `glue/pvaGlue_pvac.cpp`:
`PvaClientChannel::get(request)` consults its per-request get cache and then
**performs the get itself** (`pvaClientGet->get()` is its last step, cache hit
or miss — pvaClientChannel.cpp:337-347), so the data it returns is already
fresh; labpva then called `g->get()` **again**, issuing a second
`issueGet`+`waitGet`. Every classic-backend read — `pvaGet`,
`pvaGetStructure`, and all the metadata getters — paid twice.

Fix: the redundant `g->get()` removed (one line). Measured on the local
softIocPVA: pvac `pvaGet` 50 µs (was 79-96), `pvaGetStructure` 65 (was 94-113),
`pvaGetUnits` 50 (was 77-94) — now at parity with pvac puts and with the PVXS
backend, as a single round trip should be. Verified for freshness, not just
speed: 200 put-then-read pairs each see the immediately preceding write, plus
structure/metadata/waveform reads and the monitor-cache path.

Note this also retires an earlier claim in this changelog's 2026-08-14 A/B
notes: PVXS reads are not inherently "~2x faster than the classic backend" —
that factor was this bug. With it fixed the backends read at comparable speed.

(Both prod copies currently BUILD the PVXS backend, so their shipped binaries
are unaffected; the fix matters whenever a copy is rebuilt with `PVXS` left
undefined.)

## 2026-08-14 (later) — review hardening of the warm operations

An adversarial code review of the warm-operation work (below) confirmed ten
findings, all fixed and live-verified the same day. The dominant root cause:
pvxs parks a non-autoExec put Operation at **Done permanently** when a
disconnect hits it mid-Exec ("can't restart as server side-effects may occur"),
and `reExec*` on such an operation — or on one that is merely busy or
mid-reconnect — is **silently ignored**. The original code never detected that.

- **Silent write loss fixed:** every completion callback now classifies its
  failure (`client::RemoteError` leaves the operation Idle/reusable; anything
  else — notably `Disconnect` — is terminal) and marks the channel dead
  (`ChanState.dead`). Dead channels are dropped and rebuilt by `openPutChan`,
  never fired into. Previously the first `pvaPutNoWait` after a mid-flight
  disconnect was swallowed while reporting success, and the first blocking
  `pvaPut` stalled a full timeout on the cached dead operation.
- **No automatic re-execution of puts (at-most-once):** the timeout path used
  to re-fire the same argument on a fresh operation; if the first put was
  applied and only its ack was late, a side-effect record (`.PROC`, a relative
  move) executed twice. A failed or timed-out put now drops the channel where
  it can no longer be trusted and reports honestly — retrying is the caller's
  decision. (Instead, puts route to a one-shot operation whenever the warm one
  is not verifiably ready: alive + free + connected + matching type. One-shots
  park across a reconnect natively, so recovery needs no re-fire.)
- **Enum choices are never cached:** the choice list is data that can change
  server-side *without* a reconnect (`dbpf` on choice strings, autosave), and
  the cached list could silently write a WRONG INDEX (or spuriously reject a
  valid new index/string). Every enum put now reads the choices fresh — one
  round trip over the warm read channel — so an enum put costs 2 RTT.
  `pvaPutProto` lost its `fresh`/`fromCache` parameters, and the MEX-side
  mismatch-retry became unnecessary.
- **Stale-typed arguments can no longer reach the wire:** pvxs serialises a put
  argument against the argument's own descriptor, so after an IOC rebuild with
  a changed structure a stale-typed argument was sent anyway and misread by the
  server. The one-shot builder now validates the argument against the
  operation's freshly negotiated INIT prototype and raises a clean labpva error
  (verified live: restart the IOC with the PV re-typed ao→stringout); the warm
  path checks `equalType` against the current INIT prototype before firing.
- **`pvaClear` no longer cancels an in-flight fire-and-forget put:**
  `dropPutChan` parks such an operation in a retirement list and destroys it —
  on the MATLAB thread — only after its completion callback has run (or its
  deadline passed). Verified: 20 × (`pvaPutNoWait`; `pvaClear`) back-to-back,
  all 20 land.
- **Bounded waits against a down IOC:** a read on a channel labpva knows to be
  disconnected goes through a one-shot get (ONE timeout, as before the warm
  work), and the warm-read retry-rebuild is attempted only while the channel is
  connected; puts route to the one-shot path the same way. Previously a
  down-IOC `pvaGet` blocked 2× the configured timeout and a put up to 4×.
  Measured with a 1 s timeout: get 1.00 s, put 1.00 s.
- **Deterministic timeout identifier:** pvxs put timeouts raise
  `labpva:timeout` on both the warm and the one-shot path (the one-shot used to
  surface as `labpva:failure`).
- **No-wait ordering guarantee:** with warm and one-shot no-wait puts able to
  interleave, a warm put's data (sent immediately) could OVERTAKE a pending
  one-shot's data (sent only after its INIT round trip), so the last value
  issued was not the last value written. A per-channel ordering barrier
  (`ChanState.pendingOneShot`) now keeps the warm operation quiet until
  pending one-shots complete. Caught live by a 50-put MATLAB burst.
- **`doc/pvaBenchmarkPut.m`'s "cold" row relabelled honestly:** it never
  measured "what every put used to cost" — on PVXS it measures channel rebuild
  + put, and on the classic backend `pvaClear` does not evict pvaClient's put
  cache at all, so there it is essentially another warm put. The help text now
  says exactly that and the cross-backend `speedup_vs_cold` figure is gone.
- **Internal cleanups** (the review's below-cap items, applied later the same
  day, behaviour unchanged, full suite re-run): one shared LRU helper and one
  use-clock for both warm-channel registries; the two put-graveyard reaps
  consolidated into a single `reapFinishedPuts()` called once per put instead
  of twice; and the per-execution completion state (`OpSync`) now lives in the
  warm channel and is re-armed per execution with a generation stamp -- a
  callback whose stamp no longer matches is ignored, so late completions stay
  harmless without a fresh allocation per put/read.
- **Build fix surfaced by the above:** the MEX targets did not depend on the
  headers they include (`matlab/*.h`, `glue/*.h`), so a glue-interface change
  left stale MEX binaries that failed at load with an undefined-symbol error.
  `matlab/Makefile` now lists them (`HDR_DEPS`).

**Verified live** (softIocPVA, both prod copies, RL8 + linux arches): the full
42-check glue suite, 8 reconnect checks, 26 MATLAB checks, plus a new 20-check
findings suite that reproduces each review scenario — a `.PROC` counter
incremented EXACTLY once per put (50/50), enum choices changed live over CA
(no reconnect) seen by the next put, `pvaPutNoWait`+`pvaClear` bursts, a
no-wait put in flight across an IOC restart landing afterwards, down-IOC
latency bounds, and the type-change rebuild. Performance is unchanged:
put 34 µs, no-wait 23 µs, get 35 µs warm on the local IOC.

## 2026-08-14 — PVXS warm operations: one round trip per put AND per read

A MATLAB put loop (10000 iterations x a 3-PV list = 30000 `pvaPut`s) measured
**4.41 s on the classic backend against 27.20 s on PVXS — 6.2x slower**. Cause:
every pvxs Operation is single-shot and opens with an INIT exchange
(`pvxs/src/clientget.cpp`), so each `pvaPut` cost **four round trips** (a read
purely to learn the channel's type, then the put's INIT and its data) and each
`pvaGet` **two**, where pvaClient's cached put/get handles need **one** each.

The PVXS backend now keeps the same kind of warm handle, via PVXS's *expert*
API (`#define PVXS_ENABLE_EXPERT_API`; note that defining
`PVXS_EXPERT_API_ENABLED` yourself is a deliberate compile error in
`pvxs/version.h`): one `.autoExec(false)` Operation that parks after its INIT
and is re-fired with `Operation::reExecPut` / `reExecGet`.

Measured against a live softIocPVA on one host — `pvaPut` **34 vs 204 µs**
(5.9x), `pvaPutNoWait` **24 vs 113 µs** (4.7x), `pvaGet` **35 vs 106 µs**
(3.0x). In MATLAB the reported 3-PV loop now runs at ~37 µs/put.

- `glue/pvaGlue_pvxs.cpp` — new shared warm-operation machinery (`OpInit`,
  `OpSync`, `awaitOpInit`, `awaitOpSync`, and the INIT/completion callbacks)
  plus two registries: **`g_putChans`** (one operation per channel) and
  **`g_getChans`** (one per channel *and* pvRequest, keyed the way pvaClient's
  get cache is, so the value verb and each metadata getter keep their own). Both
  LRU-bounded at 256 entries; `pvaClear` retires them (connections stay open).
- **New glue entry point `pvaPutProto(name, fresh, fromCache, err)`** — the type
  template a put argument is built from. It is the put operation's INIT
  prototype, so a scalar/waveform put now does **no read at all**. An enum is
  the exception: `choices` are data rather than type, so an enum channel reads
  once and caches the choice list with its template. A reconnect re-INITs the
  operation, bumping a generation counter that retires the cached template, so
  an IOC that comes back a different shape cannot be written with a stale type.
- `matlab/pvaPutShared.h`, `matlab/pvaPutStructure.cpp` — build the argument
  from `pvaPutProto` instead of a fresh `pvaGet`, with one refresh-and-retry if
  a *cached* template yields a type mismatch (so a rebuilt IOC's changed enum
  choices do not surface as a spurious "enum string not among choices").
- Warm reads bring a bonus: a reused operation may receive **only the changed
  fields**, with pvxs refilling the rest from its own cache (`cache_sync`) —
  cheaper in bytes too. The reply is complete either way, and labpva's
  marshaller reads values and never marks. The cost is memory: each warm read
  channel retains one reply's worth of data (an image PV read repeatedly holds a
  frame per request), bounded by the LRU and released by `pvaClear`.
- `pvaPutNoWait` uses the warm operation only when it is **free**: `reExecPut` is
  silently ignored unless the operation is Idle, so while one fire-and-forget put
  is in flight the next falls back to a one-shot operation (which pvxs also
  re-issues across a reconnect) rather than being dropped. A busy flag that
  outlives the timeout — a put issued into a reconnect, whose callback can never
  run — marks the channel stale and rebuilds it.
- An INIT the server refuses is now reported through the operation's `.result()`
  callback instead of only timing out as "channel did not connect".

**Warnings are now on by default** (`LABPVA_WARN = -Wall -Wextra
-Wno-unused-parameter` in `configure/CONFIG_SITE`; empty it in
`CONFIG_SITE.local` to opt out). A clean build of all 30 MEX is warning-free.
This was added because the work above briefly shipped a bug that `-Wall` names
outright: renaming a local left one reference reading `free` — the C library
function, whose address is always true — so a `pvaPutNoWait` guard was always
taken and back-to-back fire-and-forget puts were silently discarded
(`-Waddress`). Found by testing, then fixed; the flag makes that class of
mistake impossible to miss again.

**Verified** against a live softIocPVA, both prod copies, both arches: 42
glue-level checks (put/read correctness for scalar/waveform/string/enum,
template caching, enum choices, repeat reads staying complete across a value
change, scoped reads, no-wait ordering, `pvaClear` invalidation, prompt failure
on a nonexistent PV) + 8 reconnect checks (operations fail cleanly while the IOC
is down, recover and stay warm) + 26 MATLAB-level checks through the real MEX
(incl. enum by choice string, `pvaPutStructure`, the metadata getters, the
monitor cache path alongside warm reads, and 50 back-to-back `pvaPutNoWait`
calls all landing).

Also added **`doc/pvaBenchmarkPut.m`** (companion to `pvaBenchmark`): times
`pvaPut`, `pvaPutNoWait`, and a put whose channel was just cleared — that last
one being what every put used to cost. It writes, so point it at a scratch PV.
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
can write wrongly (rare; **closed 2026-08-14** — the one-shot builder now
validates the argument type against the negotiated INIT prototype); `'C'` on
numeric arrays formats via `%g` on pvxs; timeouts were reported as
`labpva:failure`/`notConnected` rather than `labpva:timeout` (**changed
2026-08-14** — pvxs put timeouts now uniformly raise `labpva:timeout`).

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
  server type (and enum choices), the argument starts as `cloneEmpty()` and
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
