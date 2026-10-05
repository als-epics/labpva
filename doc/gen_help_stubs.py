#!/usr/bin/env python3
"""Generate `help` stubs for the labpva MEX verbs.

A MATLAB MEX file (pvaGet.mexa64) carries no help text. The supported idiom is
to place a same-named .m file *in the same folder*: MATLAB shows the .m help
but executes the MEX. This script writes one such stub per verb into every
bin/<arch>/labpva/ directory (next to the .mexa64 files) and copies Contents.m
there so `help labpva` works too.

Re-run after changing a signature:  python3 doc/gen_help_stubs.py
"""
import os
import glob
import shutil
import textwrap

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONTENTS = os.path.join(REPO, "doc", "Contents.m")

# name -> (h1, [usage lines], description, see_also)
VERBS = {
"pvaGet": (
    "Read the value of one or more pvAccess channels.",
    ["val        = pvaGet(pvName)",
     "val        = pvaGet(pvName, type)      % type one of 'NBSLFDC' (labca-style)",
     "val        = pvaGet(pvName, poll)      % poll=true: force a fresh read",
     "val        = pvaGet(pvName, type, poll)",
     "[val, ts]  = pvaGet(...)               % ts = sec + i*nsec (complex double)",
     "vals       = pvaGet({pv1,pv2,...})     % cell of names -> N-by-1 result"],
    "For an NTScalar this returns the .value (drop-in for lcaGet); for an "
    "NTScalarArray the waveform; for an NTEnum the selected choice string (or "
    "its index if a numeric type is requested). Anything richer -- an NTNDArray "
    "image, an NTTable, a custom multi-field group, or any PV without a "
    "top-level scalar/array value -- comes back as the whole nested struct (same "
    "as pvaGetStructure), so you rarely need pvaGetStructure. While a monitor is "
    "active on the channel the value is served "
    "from the monitor cache (no network round-trip); pass poll=true to force a "
    "fresh server read (equivalent to reading after pvaClear, but without "
    "dropping the monitor).",
    "pvaGetStructure, pvaPut, pvaSetMonitor, pvaGetStatus, pvaInfo"),

"pvaGetTable": (
    "Read an NTTable as a native MATLAB table.",
    ["t              = pvaGetTable(pvName)",
     "[t, ts]        = pvaGetTable(pvName)       % ts = sec + i*nsec",
     "[t, ts, alarm] = pvaGetTable(pvName)",
     "t              = pvaGetTable(pvName, poll) % poll=true: fresh read",
     "c              = pvaGetTable({pv1,...})    % N-by-1 cell when tables are present"],
    "For an NTTable, returns a MATLAB table whose variables follow the NTTable "
    "labels order; string columns are MATLAB string arrays. For a non-NTTable, "
    "the value and timestamp follow pvaGet, with the standard alarm struct "
    "available as a third output. A narrow monitor cache is bypassed when it "
    "does not contain the complete NTTable structure.",
    "pvaPutTable, pvaGet, pvaGetStructure"),

"pvaGetStructure": (
    "Read a whole pvAccess PVStructure as a nested MATLAB struct.",
    ["s = pvaGetStructure(pvName)",
     "s = pvaGetStructure(pvName, request)   % pvRequest, default 'field()'",
     "s = pvaGetStructure(pvName, poll)      % poll=true: force a fresh read",
     "s = pvaGetStructure(pvName, request, poll)",
     "c = pvaGetStructure({pv1,pv2,...})     % N-by-1 cell of structs"],
    "The labpva-specific capability with no labca analogue: the entire tree "
    "(value, alarm, timeStamp, display, control, valueAlarm, and any custom or "
    "nested fields, NTTable columns, etc.) is marshalled recursively into a "
    "nested struct. Use printpvs to dump every leaf, printvals for just the "
    "values + timestamps. request selects which sub-tree is fetched. Like "
    "pvaGet, this is served from the monitor cache BY DEFAULT while a monitor is "
    "active (and its request covers the requested fields), so reads cost no "
    "network round-trip; pass poll=true to force a fresh server read. With no "
    "active monitor, or when the monitor does not cover the request, it reads "
    "fresh regardless (the result is always complete).",
    "pvaGet, pvaPutStructure, pvaInfo, printpvs, printvals"),

"pvaPut": (
    "Write value(s) to channels and wait for completion.",
    ["pvaPut(pvName, value)",
     "pvaPut(pvName, value, type)            % type one of 'NBSLFDC'",
     "pvaPut({pv1,...}, values [,type])      % values: numeric vector or cell, one per PV",
     "pvaPut({pv1,...}, value  [,type])      % ONE value -> written to every PV"],
    "Blocks until each put's completion callback fires. For an enum channel a "
    "string value is matched against the choice list; a numeric value sets the "
    "index. With a cell of PV names you may pass either one value per PV (a "
    "numeric vector with numel == number of PVs, or a cell of that length) or "
    "a SINGLE value -- a scalar, a string, a struct -- which is written to "
    "every PV in the list, so pvaPut(correctors, 0) zeroes them all without "
    "building a vector of zeros (same broadcast as lcaPut).",
    "pvaPutNoWait, pvaPutStructure, pvaGet"),

"pvaPutTable": (
    "Write NTTable columns and wait for completion.",
    ["pvaPutTable(pvName, t)                    % MATLAB table",
     "pvaPutTable(pvName, s)                    % scalar struct of columns",
     "pvaPutTable(pvName, field1, value1, ...)  % named column pairs",
     "pvaPutTable(pvName, value [,type])        % non-NTTable: same as pvaPut"],
    "Writes only the NTTable value structure. Supplied columns must have equal "
    "lengths and must name fields in the target NTTable. MATLAB string arrays "
    "and character matrices are converted to string-array column values. A "
    "table or struct may be broadcast to a cell array of PV names; a matching "
    "cell of table/struct values writes one per PV.",
    "pvaGetTable, pvaPut, pvaPutStructure"),

"pvaPutNoWait": (
    "Write value(s) without waiting for completion (fire-and-forget).",
    ["pvaPutNoWait(pvName, value [,type])",
     "pvaPutNoWait({pv1,...}, values [,type])",
     "pvaPutNoWait({pv1,...}, value  [,type])  % ONE value -> every PV"],
    "Same arguments as pvaPut (including the single-value broadcast over a "
    "list of PVs), but returns as soon as the request is issued.",
    "pvaPut, pvaPutStructure"),

"pvaPutStructure": (
    "Write a whole PVStructure from a MATLAB struct.",
    ["pvaPutStructure(pvName, s)             % s mirrors the channel structure",
     "pvaPutStructure(pvName, s, request)    % pvRequest, default 'field()'",
     "pvaPutStructure({pv1,...}, {s1,...})   % cell of structs for a PV list"],
    "Only the fields present in s are written (matched by sanitised field "
    "name); fields omitted from s keep their fetched value.",
    "pvaGetStructure, pvaPut"),

"pvaInfo": (
    "Introspect a channel: type id and field-tree dump.",
    ["s = pvaInfo(pvName)"],
    "Returns a struct with .name, .typeid (the normative-type id, e.g. "
    "'epics:nt/NTScalar:1.0') and .introspection (a text dump of the field "
    "tree with current values). Useful before writing port code to see the "
    "exact shape pvaGet / pvaGetStructure will return.",
    "pvaGetStructure, pvaGet, pvaGetEnumStrings"),

"pvaSetMonitor": (
    "Subscribe to value changes on one or more channels.",
    ["pvaSetMonitor(pvName)",
     "pvaSetMonitor(pvName, request)         % default 'field(value,alarm,timeStamp)'",
     "pvaSetMonitor({pv1,...} [,request])"],
    "After subscribing, pvaNewMonitorValue / pvaNewMonitorWait report fresh "
    "samples and pvaGet / pvaGetStructure on the channel are served from the "
    "monitor cache. Use request 'field()' to cache the whole structure.",
    "pvaNewMonitorValue, pvaNewMonitorWait, pvaClear, pvaGet"),

"pvaNewMonitorValue": (
    "Non-blocking test for a new monitored sample.",
    ["tf = pvaNewMonitorValue(pvName)        % logical",
     "tf = pvaNewMonitorValue({pv1,...})     % N-by-1 logical"],
    "Returns true once per arrived sample; read the value itself with pvaGet / "
    "pvaGetStructure (served from the monitor cache). Requires a prior "
    "pvaSetMonitor.",
    "pvaSetMonitor, pvaNewMonitorWait, pvaClear"),

"pvaNewMonitorWait": (
    "Block until a monitored channel produces a new sample.",
    ["tf = pvaNewMonitorWait(pvName)",
     "tf = pvaNewMonitorWait(pvName, timeout)   % seconds (0 => configured default)",
     "tf = pvaNewMonitorWait({pv1,...} [,timeout])"],
    "Returns true when a sample arrived, false on timeout. For a list, each "
    "channel is waited on in turn. Requires a prior pvaSetMonitor.",
    "pvaSetMonitor, pvaNewMonitorValue, pvaClear, pvaSetTimeout"),

"pvaClear": (
    "Tear down monitors / cached channels.",
    ["pvaClear()                             % clear all monitors",
     "pvaClear(pvName)                        % clear one channel",
     "pvaClear({pv1,...})                     % clear several"],
    "Clearing a channel's monitor also makes the next pvaGet a fresh server "
    "read instead of a cached monitored value.",
    "pvaSetMonitor, pvaGet"),

"pvaMonitors": (
    "List the PV names that currently have an active monitor.",
    ["names = pvaMonitors()                  % N-by-1 cell of char (empty if none)"],
    "Read-only view of labpva's monitor registry (channels set with pvaSetMonitor "
    "and not yet pvaClear'd). No side effects.",
    "pvaIsMonitored, pvaSetMonitor, pvaClear"),

"pvaIsMonitored": (
    "Is a monitor currently active on a channel?",
    ["tf = pvaIsMonitored(pvName)             % logical",
     "tf = pvaIsMonitored({pv1,...})          % N-by-1 logical column"],
    "Read-only and NON-destructive (unlike pvaNewMonitorValue it does not consume "
    "the new-value flag). True iff pvaSetMonitor is active on the name.",
    "pvaMonitors, pvaSetMonitor, pvaNewMonitorValue"),

"pvaChannels": (
    "List the PV names labpva has opened a channel for.",
    ["names = pvaChannels()                  % N-by-1 cell of char (empty if none)"],
    "Channels connected this session (any PV touched by pvaGet/pvaPut/"
    "pvaSetMonitor; the connection persists and is reused). Superset of "
    "pvaMonitors. Read-only. Use pvaIsConnected for live state.",
    "pvaIsConnected, pvaMonitors, pvaGet"),

"pvaIsConnected": (
    "Is a channel currently connected to its IOC?",
    ["tf = pvaIsConnected(pvName)             % logical",
     "tf = pvaIsConnected({pv1,...})          % N-by-1 logical column"],
    "Live connection state of a channel labpva already opened. False if never "
    "opened -- does NOT open a new channel to check (no accidental connect).",
    "pvaChannels, pvaGet, pvaIsMonitored"),

"pvaGetStatus": (
    "Read alarm severity/status (and timestamp) of channels.",
    ["[sev, sta]      = pvaGetStatus(pvName[s])",
     "[sev, sta, ts]  = pvaGetStatus(pvName[s])   % ts = sec + i*nsec (complex)"],
    "severity/status are the EPICS alarm codes (0=NO_ALARM, 1=MINOR, 2=MAJOR, "
    "3=INVALID), read from the NT alarm field and returned as double.",
    "pvaGet, pvaGetAlarmLimits, pvaGetWarnLimits"),

"pvaGetNelem": (
    "Element count of a channel's value field.",
    ["n = pvaGetNelem(pvName[s])"],
    "1 for a scalar or enum, the array length for an NTScalarArray.",
    "pvaGet, pvaGetStructure"),

"pvaGetControlLimits": (
    "Drive-range (control) limits of a channel.",
    ["[lo, hi] = pvaGetControlLimits(pvName[s])"],
    "Read from the NT control field (control.limitLow / control.limitHigh).",
    "pvaGetGraphicLimits, pvaGetAlarmLimits, pvaGetWarnLimits"),

"pvaGetGraphicLimits": (
    "Display-range (graphic) limits of a channel.",
    ["[lo, hi] = pvaGetGraphicLimits(pvName[s])"],
    "Read from the NT display field (display.limitLow / display.limitHigh).",
    "pvaGetControlLimits, pvaGetAlarmLimits, pvaGetUnits"),

"pvaGetAlarmLimits": (
    "Alarm thresholds of a channel.",
    ["[lo, hi] = pvaGetAlarmLimits(pvName[s])"],
    "Read from the NT valueAlarm field "
    "(valueAlarm.lowAlarmLimit / valueAlarm.highAlarmLimit).",
    "pvaGetWarnLimits, pvaGetControlLimits, pvaGetStatus"),

"pvaGetWarnLimits": (
    "Warning thresholds of a channel.",
    ["[lo, hi] = pvaGetWarnLimits(pvName[s])"],
    "Read from the NT valueAlarm field "
    "(valueAlarm.lowWarningLimit / valueAlarm.highWarningLimit).",
    "pvaGetAlarmLimits, pvaGetControlLimits, pvaGetStatus"),

"pvaGetUnits": (
    "Engineering units string of a channel.",
    ["u = pvaGetUnits(pvName)                % char",
     "u = pvaGetUnits({pv1,...})             % N-by-1 cell of char"],
    "Read from the NT display.units field.",
    "pvaGetPrecision, pvaGetGraphicLimits"),

"pvaGetPrecision": (
    "Display precision of a channel.",
    ["p = pvaGetPrecision(pvName[s])"],
    "Read from the NT display.precision field.",
    "pvaGetUnits, pvaGetGraphicLimits"),

"pvaGetEnumStrings": (
    "Choice strings of an NTEnum channel.",
    ["c = pvaGetEnumStrings(pvName)          % 1-by-K cell of strings",
     "c = pvaGetEnumStrings({pv1,...})       % N-by-1 cell, each a 1-by-K cell"],
    "Read from value.choices of an NTEnum. Non-enum channels yield an empty "
    "cell.",
    "pvaGet, pvaInfo"),

"pvaSetTimeout": (
    "Set the connect/IO timeout, in seconds.",
    ["pvaSetTimeout(seconds)"],
    "Applies to subsequent connect and get/put/monitor operations.",
    "pvaGetTimeout, pvaNewMonitorWait"),

"pvaGetTimeout": (
    "Read the connect/IO timeout, in seconds.",
    ["t = pvaGetTimeout()"],
    "",
    "pvaSetTimeout"),

"pvaSetProvider": (
    "Choose the provider for subsequently-opened channels.",
    ["pvaSetProvider('pva')                  % use pvAccess (default)",
     "pvaSetProvider('ca')                   % use Channel Access"],
    "Affects channels opened after the call; already-cached channels keep "
    "their provider. No labca analogue (Channel Access has one protocol).",
    "pvaGetProvider"),

"pvaGetProvider": (
    "Read the current provider token ('pva' or 'ca').",
    ["p = pvaGetProvider()"],
    "",
    "pvaSetProvider"),

"pvaDebugOn": (
    "Enable pvaClient + labpva debug output.",
    ["pvaDebugOn()"],
    "",
    "pvaDebugOff, pvaLastError"),

"pvaDebugOff": (
    "Disable pvaClient + labpva debug output.",
    ["pvaDebugOff()"],
    "",
    "pvaDebugOn"),

"pvaLastError": (
    "Read the most recent labpva error.",
    ["code        = pvaLastError()",
     "[code, msg] = pvaLastError()"],
    "code is 0 when the last operation succeeded.",
    "pvaDebugOn"),
}


def stub_text(name, spec):
    h1, usage, desc, see_also = spec
    lines = [f"%{name}  {h1}", "%"]
    for u in usage:
        lines.append(f"%   {u}")
    if desc:
        lines.append("%")
        for para in textwrap.wrap(desc, width=74):
            lines.append(f"%   {para}")
    if see_also:
        lines.append("%")
        lines.append(f"%   See also {see_also}.")
    help_block = "\n".join(lines)
    return f"""\
function varargout = {name}(varargin) %#ok<STOUT>
{help_block}

% This .m file documents the compiled MEX {name}.mexa64. MATLAB shows the help
% above but EXECUTES the MEX. Reaching the error below means the MEX is not on
% the path -- see `help labpva` / labpva/README.md.
error('labpva:mexMissing', ...
      '%s: compiled MEX not found on the MATLAB path (see `help labpva`).', ...
      mfilename);
end
"""


def main():
    targets = sorted(glob.glob(os.path.join(REPO, "bin", "*", "labpva")))
    if not targets:
        raise SystemExit("no bin/<arch>/labpva directories found -- build first")
    for d in targets:
        n = 0
        for name, spec in VERBS.items():
            with open(os.path.join(d, name + ".m"), "w") as f:
                f.write(stub_text(name, spec))
            n += 1
        if os.path.exists(CONTENTS):
            shutil.copyfile(CONTENTS, os.path.join(d, "Contents.m"))
        print(f"wrote {n} help stubs + Contents.m to {d}")


if __name__ == "__main__":
    main()
