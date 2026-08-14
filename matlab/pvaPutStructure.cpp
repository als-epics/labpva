/* pvaPutStructure.cpp - write a whole PVStructure from a MATLAB struct.
 *
 *   pvaPutStructure(pvname, s)              s mirrors the channel's structure
 *   pvaPutStructure(pvname, s, request)     request = pvRequest (default "field()")
 *   pvaPutStructure({pv1,...}, {s1,...})    cell of structs for a list of PVs
 *
 * Only the fields present in `s` are written (matched by sanitised field
 * name); fields omitted from `s` keep their fetched value. The whole
 * structure is marked changed before sending.
 */
#include "mglue.h"
#include "pvaGlue.h"
#include "pvaConvert.h"

#ifdef LABPVA_USE_PVXS

#include <pvxs/data.h>
#include <cctype>
#include <set>

using namespace labpva;

/* Honour an optional pvRequest scope: for a "field(a,b,...)" request, unmark
 * every top-level field NOT in the list so it is not sent (matching the pvac
 * behaviour where the request limits what the put writes). Any other request
 * shape is ignored (everything the MATLAB struct touched is written). */
static void applyRequestScope(PvValue &arg, const std::string &request)
{
    std::string r;
    for (size_t i = 0; i < request.size(); ++i)
        if (!std::isspace((unsigned char)request[i])) r.push_back(request[i]);
    if (r.empty() || r == "field()" || r == "field") return;
    if (r.compare(0, 6, "field(") != 0 || r[r.size() - 1] != ')' ||
        r.find('(', 6) != std::string::npos) return;

    std::set<std::string> allowed;
    std::string list = r.substr(6, r.size() - 7);
    size_t pos = 0;
    while (pos < list.size()) {
        size_t c = list.find(',', pos);
        if (c == std::string::npos) c = list.size();
        if (c > pos) {
            std::string f = list.substr(pos, c - pos);
            size_t dot = f.find('.');           /* scope by TOP-level field */
            allowed.insert(dot == std::string::npos ? f : f.substr(0, dot));
        }
        pos = c + 1;
    }
    for (pvxs::Value fld : arg.ichildren())
        if (!allowed.count(arg.nameOf(fld)))
            fld.unmark(false, true);            /* this field + its children */
}

void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
    (void)nlhs; (void)plhs;
    if (nrhs < 2)
        mexErrMsgIdAndTxt("labpva:invalidArg",
                          "pvaPutStructure: need a PV name and a struct");

    PvaError err;
    bool wasCell = false;
    std::vector<std::string> pvs = buildPVs(prhs[0], wasCell, err);
    errCheck(err);

    const mxArray *sArg = prhs[1];
    std::string request = (nrhs >= 3) ? argString(prhs[2]) : "field()";
    if (request.empty()) request = "field()";

    if (!wasCell && !mxIsStruct(sArg))
        mexErrMsgIdAndTxt("labpva:invalidArg", "pvaPutStructure: value must be a struct");
    if (wasCell && !(mxIsCell(sArg) && mxGetNumberOfElements(sArg) == pvs.size()))
        mexErrMsgIdAndTxt("labpva:invalidArg",
                          "pvaPutStructure: for a list of PVs pass a matching cell of structs");

    for (size_t i = 0; i < pvs.size(); ++i) {
        const mxArray *smx = wasCell ? mxGetCell(sArg, i) : sArg;
        if (!smx || !mxIsStruct(smx)) {
            err.err = PVA_INVALIDARG; err.msg = "each value must be a struct"; break;
        }
        PvValue proto = pvaPutPrototype(pvs[i], err);   /* type authority */
        if (err.err != PVA_OK) break;
        PvValue arg = mxToPutArgStructure(smx, proto, err);
        if (err.err != PVA_OK) break;
        applyRequestScope(arg, request);
        pvaPutExec(pvs[i], arg, /*wait=*/true, err);
        if (err.err != PVA_OK) break;
    }
    errCheck(err);
}

#else /* classic backend */

using namespace labpva;
using namespace epics::pvData;
using namespace epics::pvaClient;

void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
    (void)nlhs; (void)plhs;
    if (nrhs < 2)
        mexErrMsgIdAndTxt("labpva:invalidArg",
                          "pvaPutStructure: need a PV name and a struct");

    PvaError err;
    bool wasCell = false;
    std::vector<std::string> pvs = buildPVs(prhs[0], wasCell, err);
    errCheck(err);

    const mxArray *sArg = prhs[1];
    std::string request = (nrhs >= 3) ? argString(prhs[2]) : "field()";
    if (request.empty()) request = "field()";

    if (!wasCell && !mxIsStruct(sArg))
        mexErrMsgIdAndTxt("labpva:invalidArg", "pvaPutStructure: value must be a struct");
    if (wasCell && !(mxIsCell(sArg) && mxGetNumberOfElements(sArg) == pvs.size()))
        mexErrMsgIdAndTxt("labpva:invalidArg",
                          "pvaPutStructure: for a list of PVs pass a matching cell of structs");

    for (size_t i = 0; i < pvs.size(); ++i) {
        const mxArray *smx = wasCell ? mxGetCell(sArg, i) : sArg;
        if (!smx || !mxIsStruct(smx)) {
            err.err = PVA_INVALIDARG; err.msg = "each value must be a struct"; break;
        }
        PvaClientPutPtr put = pvaPutPrepare(pvs[i], request, err);
        if (err.err != PVA_OK) break;
        PVStructurePtr data = put->getData()->getPVStructure();
        mxToPvField(smx, data, err);
        if (err.err != PVA_OK) break;
        pvaPutCommit(put, /*whole structure*/0, /*wait=*/true, err);
        if (err.err != PVA_OK) break;
    }
    errCheck(err);
}

#endif /* LABPVA_USE_PVXS */
