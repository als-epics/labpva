/* pvaGetTable.cpp - read an NTTable as a MATLAB table.
 *
 *   table              = pvaGetTable(pvname)
 *   [table, ts]        = pvaGetTable(pvname)       ts = sec + i*nsec
 *   [table, ts, alarm] = pvaGetTable(pvname)
 *   table              = pvaGetTable(pvname, poll)
 *
 * For a non-NTTable channel, value and timestamp match pvaGet; the optional
 * third output is the standard alarm structure.
 */
#include "mglue.h"
#include "pvaGlue.h"
#include "pvaConvert.h"

using namespace labpva;

static mxArray *assembleAlarmOutput(std::vector<mxArray *> &alarms, bool wasCell)
{
    if (!wasCell)
        return alarms.empty() ? mxCreateStructMatrix(1, 1, 0, NULL) : alarms[0];
    mxArray *cell = mxCreateCellMatrix(alarms.size(), 1);
    for (size_t i = 0; i < alarms.size(); ++i) mxSetCell(cell, i, alarms[i]);
    return cell;
}

void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
    if (nrhs < 1)
        mexErrMsgIdAndTxt("labpva:invalidArg", "pvaGetTable: need at least a PV name");

    PvaError err;
    bool wasCell = false;
    std::vector<std::string> pvs = buildPVs(prhs[0], wasCell, err);
    errCheck(err);

    char type = 'N';
    bool poll = false;
    for (int a = 1; a < nrhs; ++a) {
        const mxArray *m = prhs[a];
        if (mxIsChar(m))
            type = parseTypeArg(m);
        else if ((mxIsLogical(m) || mxIsNumeric(m)) &&
                 mxGetNumberOfElements(m) >= 1)
            poll = (mxGetScalar(m) != 0);
    }

    std::vector<mxArray *> values, alarms;
    std::vector<double> secs, nsecs;
    const bool wantTs = nlhs >= 2;
    const bool wantAlarm = nlhs >= 3;
    for (size_t i = 0; i < pvs.size(); ++i) {
        PvValue pv = pvaGet(pvs[i], "field()", err,
                            /*useMonitorCache=*/!poll,
                            /*requireWholeMonitor=*/true);
        if (err.err != PVA_OK) break;
        values.push_back(pvIsNTTable(pv) ? pvTableToMx(pv, err)
                                         : pvValueToMx(pv, type, err));
        if (err.err != PVA_OK) break;
        if (wantTs) {
            double sec = 0.0, nsec = 0.0;
            pvTimeStampSecNsec(pv, sec, nsec);
            secs.push_back(sec);
            nsecs.push_back(nsec);
        }
        if (wantAlarm) alarms.push_back(pvAlarmToMx(pv));
    }
    errCheck(err);

    plhs[0] = assembleValueOutput(values, wasCell);
    if (wantTs) plhs[1] = complexColumn(secs, nsecs);
    if (wantAlarm) plhs[2] = assembleAlarmOutput(alarms, wasCell);
}