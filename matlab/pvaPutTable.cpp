/* pvaPutTable.cpp - write NTTable columns and wait for completion.
 *
 *   pvaPutTable(pvname, table)
 *   pvaPutTable(pvname, structOfColumns)
 *   pvaPutTable(pvname, field1, value1, field2, value2, ...)
 *
 * A non-NTTable channel follows pvaPut(pvname, value [,type]) semantics.
 */
#include "pvaPutShared.h"

#include <algorithm>
#include <set>

using namespace labpva;

static std::string fieldName(const mxArray *mx, PvaError &err)
{
    if (mxIsChar(mx)) return argString(mx);
    if (mxIsClass(mx, "string") && mxGetNumberOfElements(mx) == 1) {
        mxArray *ch = NULL;
        mxArray *in = const_cast<mxArray *>(mx);
        mxArray *trap = mexCallMATLABWithTrap(1, &ch, 1, &in, "char");
        if (!trap && ch) {
            std::string name = argString(ch);
            mxDestroyArray(ch);
            return name;
        }
        if (trap) mxDestroyArray(trap);
        if (ch) mxDestroyArray(ch);
    }
    err.err = PVA_INVALIDARG;
    err.msg = "NTTable column names must be char rows or string scalars";
    return std::string();
}

static mxArray *normaliseColumn(const mxArray *input, PvaError &err)
{
    if (mxIsClass(input, "string")) {
        mxArray *cell = NULL;
        mxArray *in = const_cast<mxArray *>(input);
        mxArray *trap = mexCallMATLABWithTrap(1, &cell, 1, &in, "cellstr");
        if (trap) {
            mxDestroyArray(trap);
            if (cell) mxDestroyArray(cell);
            err.err = PVA_TYPEMISMATCH;
            err.msg = "could not convert a MATLAB string column to cellstr";
            return NULL;
        }
        return cell;
    }
    if (mxIsChar(input) && mxGetM(input) > 1) {
        mxArray *cell = NULL;
        mxArray *in = const_cast<mxArray *>(input);
        mxArray *trap = mexCallMATLABWithTrap(1, &cell, 1, &in, "cellstr");
        if (trap) {
            mxDestroyArray(trap);
            if (cell) mxDestroyArray(cell);
            err.err = PVA_TYPEMISMATCH;
            err.msg = "could not convert a character matrix column to cellstr";
            return NULL;
        }
        return cell;
    }
    return mxDuplicateArray(input);
}

static size_t columnLength(const mxArray *column)
{
    if (mxIsChar(column)) return 1;
    return mxGetNumberOfElements(column);
}

static mxArray *normaliseStruct(const mxArray *input, PvaError &err)
{
    if (!input || !mxIsStruct(input) || mxGetNumberOfElements(input) != 1) {
        err.err = PVA_INVALIDARG;
        err.msg = "NTTable input must be a MATLAB table or a scalar struct of columns";
        return NULL;
    }
    const int count = mxGetNumberOfFields(input);
    if (count == 0) {
        err.err = PVA_INVALIDARG;
        err.msg = "NTTable input must contain at least one column";
        return NULL;
    }
    std::vector<const char *> names(count);
    for (int field = 0; field < count; ++field)
        names[field] = mxGetFieldNameByNumber(input, field);
    mxArray *out = mxCreateStructMatrix(1, 1, count, &names[0]);
    size_t rows = 0;
    for (int field = 0; field < count; ++field) {
        const mxArray *value = mxGetFieldByNumber(input, 0, field);
        if (!value) {
            err.err = PVA_INVALIDARG;
            err.msg = std::string("NTTable column '") + names[field] + "' has no value";
            break;
        }
        mxArray *column = normaliseColumn(value, err);
        if (err.err != PVA_OK) break;
        const size_t length = columnLength(column);
        if (field == 0) rows = length;
        else if (length != rows) {
            mxDestroyArray(column);
            err.err = PVA_INVALIDARG;
            err.msg = "all supplied NTTable columns must have the same number of rows";
            break;
        }
        mxSetFieldByNumber(out, 0, field, column);
    }
    if (err.err != PVA_OK) {
        mxDestroyArray(out);
        return NULL;
    }
    return out;
}

static mxArray *normaliseTableObject(const mxArray *input, PvaError &err)
{
    mxArray *asStruct = NULL;
    mxArray *args[3] = {
        const_cast<mxArray *>(input),
        mxCreateString("ToScalar"),
        mxCreateLogicalScalar(true)
    };
    mxArray *trap = mexCallMATLABWithTrap(1, &asStruct, 3, args, "table2struct");
    mxDestroyArray(args[1]);
    mxDestroyArray(args[2]);
    if (trap) {
        mxDestroyArray(trap);
        if (asStruct) mxDestroyArray(asStruct);
        err.err = PVA_TYPEMISMATCH;
        err.msg = "could not convert the MATLAB table to NTTable columns";
        return NULL;
    }
    mxArray *out = normaliseStruct(asStruct, err);
    mxDestroyArray(asStruct);
    return out;
}

static mxArray *normaliseTableInput(const mxArray *input, PvaError &err)
{
    return mxIsClass(input, "table") ? normaliseTableObject(input, err)
                                      : normaliseStruct(input, err);
}

static mxArray *columnsFromPairs(int nrhs, const mxArray *prhs[], PvaError &err)
{
    if ((nrhs - 1) % 2 != 0) {
        err.err = PVA_INVALIDARG;
        err.msg = "NTTable field/value arguments must occur in pairs";
        return NULL;
    }
    const int count = (nrhs - 1) / 2;
    std::vector<std::string> names(count);
    std::vector<const char *> namePtrs(count);
    std::set<std::string> seen;
    for (int field = 0; field < count; ++field) {
        names[field] = fieldName(prhs[1 + 2 * field], err);
        if (err.err != PVA_OK) return NULL;
        if (names[field].empty() || !seen.insert(names[field]).second) {
            err.err = PVA_INVALIDARG;
            err.msg = names[field].empty() ? "NTTable column names cannot be empty"
                                           : std::string("duplicate NTTable column '") + names[field] + "'";
            return NULL;
        }
        namePtrs[field] = names[field].c_str();
    }
    mxArray *raw = mxCreateStructMatrix(1, 1, count, &namePtrs[0]);
    for (int field = 0; field < count; ++field)
        mxSetFieldByNumber(raw, 0, field,
                           mxDuplicateArray(prhs[2 + 2 * field]));
    mxArray *out = normaliseStruct(raw, err);
    mxDestroyArray(raw);
    return out;
}

static mxArray *tableColumnsFor(const mxArray *valueArg, int nrhs,
                                const mxArray *prhs[], PvaError &err)
{
    return nrhs == 2 ? normaliseTableInput(valueArg, err)
                     : columnsFromPairs(nrhs, prhs, err);
}

void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
    (void)nlhs; (void)plhs;
    if (nrhs < 2)
        mexErrMsgIdAndTxt("labpva:invalidArg",
                          "pvaPutTable: need a PV name and a value");

    PvaError err;
    bool wasCell = false;
    std::vector<std::string> pvs = buildPVs(prhs[0], wasCell, err);
    errCheck(err);

    for (size_t i = 0; i < pvs.size(); ++i) {
        mxArray *scratch = NULL;
        const mxArray *valueArg = putValueFor(prhs[1], i, pvs.size(), wasCell,
                                              &scratch, err);
        if (err.err != PVA_OK) break;

#ifdef LABPVA_USE_PVXS
        PvValue current = pvaPutProto(pvs[i], err);
        if (err.err == PVA_OK && pvIsNTTable(current)) {
            mxArray *columns = tableColumnsFor(valueArg, nrhs, prhs, err);
            if (err.err == PVA_OK) pvValidateTableColumns(current, columns, err);
            if (err.err == PVA_OK) {
                PvValue arg = mxToPutArg(columns, current, 'N', err);
                if (err.err == PVA_OK) pvaPutExec(pvs[i], arg, /*wait=*/true, err);
            }
            if (columns) mxDestroyArray(columns);
        } else if (err.err == PVA_OK) {
            if (nrhs > 3) {
                err.err = PVA_INVALIDARG;
                err.msg = "non-NTTable channels accept pvaPut(pvname, value [,type]) arguments";
            } else {
                const char type = nrhs == 3 ? parseTypeArg(prhs[2]) : 'N';
                PvValue arg = mxToPutArg(valueArg, current, type, err);
                if (err.err == PVA_OK) pvaPutExec(pvs[i], arg, /*wait=*/true, err);
            }
        }
#else
        epics::pvaClient::PvaClientPutPtr put =
            pvaPutPrepare(pvs[i], "field(value)", err);
        if (err.err == PVA_OK) {
            PvValue current = put->getData()->getPVStructure();
            if (pvIsNTTable(current)) {
                mxArray *columns = tableColumnsFor(valueArg, nrhs, prhs, err);
                if (err.err == PVA_OK) pvValidateTableColumns(current, columns, err);
                if (err.err == PVA_OK) {
                    std::string written = mxToPvValue(columns, current, 'N', err);
                    if (err.err == PVA_OK) {
                        epics::pvData::PVFieldPtr field = current->getSubField(written);
                        if (!field) {
                            err.err = PVA_NOFIELD;
                            err.msg = "converted NTTable field is absent from the put structure";
                        } else {
                            pvaPutCommit(put, field->getFieldOffset(), /*wait=*/true, err);
                        }
                    }
                }
                if (columns) mxDestroyArray(columns);
            } else if (nrhs > 3) {
                err.err = PVA_INVALIDARG;
                err.msg = "non-NTTable channels accept pvaPut(pvname, value [,type]) arguments";
            } else {
                const char type = nrhs == 3 ? parseTypeArg(prhs[2]) : 'N';
                std::string written = mxToPvValue(valueArg, current, type, err);
                if (err.err == PVA_OK) {
                    epics::pvData::PVFieldPtr field = current->getSubField(written);
                    if (!field) {
                        err.err = PVA_NOFIELD;
                        err.msg = "converted value field is absent from the put structure";
                    } else {
                        pvaPutCommit(put, field->getFieldOffset(), /*wait=*/true, err);
                    }
                }
            }
        }
#endif
        if (scratch) mxDestroyArray(scratch);
        if (err.err != PVA_OK) break;
    }
    errCheck(err);
}