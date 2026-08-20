% labpva - EPICS pvAccess interface for MATLAB (a labca-style PVA binding)
%
% Read / write
%   pvaGet              - read a channel's value(s)        [val,ts] = pvaGet(pv[s][,type])
%   pvaGetTable         - read an NTTable as a native MATLAB table
%   pvaGetStructure     - read the whole PVStructure as a nested struct
%   pvaPut              - write value(s), wait for completion
%   pvaPutTable         - write NTTable table/struct/named columns
%   pvaPutNoWait        - write value(s), do not wait
%   pvaPutStructure     - write a whole structure from a MATLAB struct
%   pvaInfo             - introspect a channel (type id + field-tree dump)
%
% Monitors
%   pvaSetMonitor       - subscribe to value changes
%   pvaNewMonitorValue  - non-blocking: has a new value arrived?
%   pvaNewMonitorWait   - block until a new value arrives
%   pvaClear            - tear down monitors / cached channels
%   pvaMonitors         - list PV names that currently have a monitor
%   pvaIsMonitored      - is a monitor active on a channel? (non-destructive)
%   pvaChannels         - list PV names labpva has opened a channel for
%   pvaIsConnected      - is a channel currently connected to its IOC?
%
% Metadata
%   pvaGetStatus        - [severity, status, timestamp]
%   pvaGetNelem         - element count of the value field
%   pvaGetControlLimits - drive-range limits          (control.*)
%   pvaGetGraphicLimits - display-range limits         (display.*)
%   pvaGetAlarmLimits   - alarm thresholds             (valueAlarm.*)
%   pvaGetWarnLimits    - warning thresholds           (valueAlarm.*)
%   pvaGetUnits         - engineering units string
%   pvaGetPrecision     - display precision
%   pvaGetEnumStrings   - enum choice strings
%
% Configuration / diagnostics
%   pvaSetTimeout / pvaGetTimeout   - connect/IO timeout (seconds)
%   pvaSetProvider / pvaGetProvider - 'pva' (default) or 'ca'
%   pvaDebugOn / pvaDebugOff        - toggle debug output
%   pvaLastError                    - [code, message] of last operation
%
% Helpers (MATLAB functions in doc/, not MEX verbs)
%   pvaGetImage         - NTNDArray image -> shaped 2-D/3-D array (+ offset axes)
%   printpvs            - print every leaf of a fetched structure
%   printvals           - print only each signal's .value (+ timestamp)
%   pvaBenchmark        - time the read verbs (backend A/B comparison)
%   pvaBenchmarkPut     - time the write verbs (WRITES: use a scratch PV)
%
% Timestamps are complex doubles: real = seconds past epoch, imag = nanoseconds.
% See ARCHITECTURE.md for the labca->PVA mapping and the structure model.
