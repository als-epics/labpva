/* pvaPutShared.h - shared body for pvaPut (wait) and pvaPutNoWait (no wait).
 *
 * Header-only so each MEX file stays a single translation unit (labca splits
 * the same way via theLcaPutMexFunction). Writes only the `value` field of
 * each channel's structure; use pvaPutStructure for whole-structure writes.
 *
 * The MATLAB argument handling is backend-neutral; only the "perform the put"
 * step differs: the classic backend uses pvaClient's cached put handles
 * (pvaPutPrepare/pvaPutCommit + mxToPvValue), the PVXS backend pre-builds an
 * argument Value from the channel's cached type template (pvaPutProto +
 * mxToPutArg) and sends its marked fields over that channel's warm put
 * operation (pvaPutExec). Both are one server round trip per put.
 */
#ifndef LABPVA_PUT_SHARED_H
#define LABPVA_PUT_SHARED_H

#include "mglue.h"
#include "pvaGlue.h"
#include "pvaConvert.h"

namespace labpva {

/* Element i of any numeric/logical MATLAB array, as double. `ok` false for an
 * unsupported class (then the value is unusable). Element-accurate for every
 * numeric class -- mxGetScalar would silently return element 1 for all i. */
static inline double
numericElement(const mxArray *mx, size_t i, bool &ok)
{
    ok = true;
    if (mxIsLogical(mx)) return mxGetLogicals(mx)[i] ? 1.0 : 0.0;
    switch (mxGetClassID(mx)) {
    case mxDOUBLE_CLASS: return ((const double  *)mxGetData(mx))[i];
    case mxSINGLE_CLASS: return (double)((const float   *)mxGetData(mx))[i];
    case mxINT8_CLASS:   return (double)((const int8_T  *)mxGetData(mx))[i];
    case mxUINT8_CLASS:  return (double)((const uint8_T *)mxGetData(mx))[i];
    case mxINT16_CLASS:  return (double)((const int16_T *)mxGetData(mx))[i];
    case mxUINT16_CLASS: return (double)((const uint16_T*)mxGetData(mx))[i];
    case mxINT32_CLASS:  return (double)((const int32_T *)mxGetData(mx))[i];
    case mxUINT32_CLASS: return (double)((const uint32_T*)mxGetData(mx))[i];
    case mxINT64_CLASS:  return (double)((const int64_T *)mxGetData(mx))[i];
    case mxUINT64_CLASS: return (double)((const uint64_T*)mxGetData(mx))[i];
    default: ok = false; return 0.0;
    }
}

/* The value converters read only the real part, so writing a complex value
 * would silently drop its imaginary part. True (err set) if `mx` is complex. */
static inline bool
rejectComplex(const mxArray *mx, PvaError &err)
{
    if (!mx || !mxIsComplex(mx)) return false;
    err.err = PVA_INVALIDARG;
    err.msg = "complex values cannot be written to a PV";
    return true;
}

/* Select the per-PV MATLAB value from the value argument.
 * - single PV                  -> the whole argument
 * - list + cell of N           -> element i
 * - list + numeric of N        -> a 1x1 double holding element i (caller frees)
 * - list + ONE value           -> that value, written to EVERY PV
 *
 * The last case is LabCA's broadcast semantics: lcaPut accepts a value
 * matrix with either 1 or M rows. For example,
 *
 *   pvaPut({'PV1','PV2',...}, 0)
 *
 * writes 0 to every PV without requiring the caller to build a vector
 * of zeros.
 *
 * "ONE value" means a numeric/logical scalar, a char row (a string, e.g.
 * an enum choice), a struct, a 1x1 cell, or anything else that is not
 * a per-PV container.
 *
 * A list containing ONE PV likewise takes the whole argument, so:
 *
 *   pvaPut({'PV'}, [1 2 3])
 *
 * writes a waveform, equivalent to:
 *
 *   pvaPut('PV', [1 2 3])
 *
 * Complex values are rejected in every form, single PV included.
 *
 * Returns NULL and sets err on a shape mismatch or a complex value. */
static inline const mxArray *
putValueFor(const mxArray *valArg, size_t i, size_t n, bool wasCell,
            mxArray **scratch, PvaError &err)
{
    *scratch = NULL;
    if (rejectComplex(valArg, err)) return NULL;
    if (!wasCell) return valArg;
    if (mxIsCell(valArg)) {
        size_t nv = mxGetNumberOfElements(valArg);
        if (nv != 1 && nv != n) {
            err.err = PVA_INVALIDARG;
            err.msg = "value cell must hold one value per PV, or a single "
                      "value to write to all of them";
            return NULL;
        }
        /* Callers walk i from 0: check every element before the first PV is
         * written, so a complex element cannot leave the list half-written. */
        if (i == 0)
            for (size_t k = 0; k < nv; ++k)
                if (rejectComplex(mxGetCell(valArg, k), err)) return NULL;
        return mxGetCell(valArg, nv == 1 ? 0 : i);   /* nv == 1: all PVs */
    }
    if (mxIsNumeric(valArg) || mxIsLogical(valArg)) {
        size_t nv = mxGetNumberOfElements(valArg);
        /* Scalar broadcast, or a single-PV list taking the whole argument (a
         * waveform): hand the argument over untouched. */
        if (nv == 1 || n == 1) return valArg;
        if (nv != n) {
            err.err = PVA_INVALIDARG;
            err.msg = "for a list of PVs, give a numeric vector with one "
                      "element per PV, a cell of values, or a single value to "
                      "write to all of them";
            return NULL;
        }
        bool ok = false;
        double d = numericElement(valArg, i, ok);
        if (!ok) {
            err.err = PVA_INVALIDARG;
            err.msg = "unsupported numeric class in the value vector";
            return NULL;
        }
        mxArray *s = mxCreateDoubleScalar(d);
        *scratch = s;
        return s;
    }
    if (mxIsChar(valArg) && mxGetM(valArg) > 1) {
        /* A char MATRIX is one string per row to the eye but column-major
         * garbage to mxArrayToString -- do not silently write it. */
        err.err = PVA_INVALIDARG;
        err.msg = "give per-PV strings as a cell of char rows, not a char "
                  "matrix";
        return NULL;
    }
    return valArg;                     /* string / struct / ... -> every PV */
}

#ifdef LABPVA_USE_PVXS

static inline void
pvaPutMexBody(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[], bool wait)
{
    (void)nlhs; (void)plhs;
    if (nrhs < 2)
        mexErrMsgIdAndTxt("labpva:invalidArg",
                          "pvaPut: need a PV name and a value");

    PvaError err;
    bool wasCell = false;
    std::vector<std::string> pvs = buildPVs(prhs[0], wasCell, err);
    errCheck(err);

    const mxArray *valArg = prhs[1];
    char type = (nrhs >= 3) ? parseTypeArg(prhs[2]) : 'N';
    size_t n = pvs.size();

    for (size_t i = 0; i < n; ++i) {
        mxArray *scratch = NULL;
        const mxArray *vmx = putValueFor(valArg, i, n, wasCell, &scratch, err);
        if (err.err != PVA_OK) break;

        /* The put channel's type template lets the argument be built here, on
         * the MATLAB thread: for an ordinary PV it is the warm operation's
         * INIT prototype (no server read at all); for an enum it is a fresh
         * read, because the choice list is data that can change without a
         * reconnect -- see pvaPutProto. */
        PvValue cur = pvaPutProto(pvs[i], err);
        if (err.err == PVA_OK) {
            PvValue arg = mxToPutArg(vmx, cur, type, err);
            if (err.err == PVA_OK)
                pvaPutExec(pvs[i], arg, wait, err);
        }
        if (scratch) mxDestroyArray(scratch);
        if (err.err != PVA_OK) break;
    }
    errCheck(err);
}

#else /* classic backend */

static inline void
pvaPutMexBody(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[], bool wait)
{
    (void)nlhs; (void)plhs;
    if (nrhs < 2)
        mexErrMsgIdAndTxt("labpva:invalidArg",
                          "pvaPut: need a PV name and a value");

    PvaError err;
    bool wasCell = false;
    std::vector<std::string> pvs = buildPVs(prhs[0], wasCell, err);
    errCheck(err);

    const mxArray *valArg = prhs[1];
    char type = (nrhs >= 3) ? parseTypeArg(prhs[2]) : 'N';
    size_t n = pvs.size();

    for (size_t i = 0; i < n; ++i) {
        mxArray *scratch = NULL;
        const mxArray *vmx = putValueFor(valArg, i, n, wasCell, &scratch, err);
        if (err.err != PVA_OK) break;

        epics::pvaClient::PvaClientPutPtr put =
            pvaPutPrepare(pvs[i], "field(value)", err);
        if (err.err != PVA_OK) { if (scratch) mxDestroyArray(scratch); break; }

        epics::pvData::PVStructurePtr data = put->getData()->getPVStructure();
        std::string wf = mxToPvValue(vmx, data, type, err);
        if (err.err == PVA_OK) {
            epics::pvData::PVFieldPtr vf = data->getSubField(wf);
            std::size_t off = vf ? vf->getFieldOffset() : 0;
            pvaPutCommit(put, off, wait, err);
        }
        if (scratch) mxDestroyArray(scratch);
        if (err.err != PVA_OK) break;
    }
    errCheck(err);
}

#endif /* LABPVA_USE_PVXS */

} // namespace labpva

#endif /* LABPVA_PUT_SHARED_H */
