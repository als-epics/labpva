/* pvaConvert_pvxs.cpp - see pvaConvert.h. PVXS-backend implementation.
 *
 * Marshals pvxs::Value trees to MATLAB per the same representation contract as
 * the classic backend (pvaConvert_pvac.cpp): numeric leaves collapse to
 * double, strings to char rows / cells, structures to nested structs, unions
 * unwrap to the stored member, and enum_t gets the NT-aware sugar. Array
 * elements are read through TYPED shared_array accessors (one case per
 * TypeCode) and converted per element, so no reliance on untyped buffers.
 *
 * The MATLAB->PV write path is deliberately absent here -- it lands with the
 * put phase of the port (pvxs puts are builder-based; see pvaGlue.h).
 */
#include "pvaConvert.h"

#include <pvxs/data.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace pvxs;

namespace labpva {

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

std::string mxFieldName(const std::string &pvName)
{
    std::string out;
    out.reserve(pvName.size());
    for (size_t i = 0; i < pvName.size(); ++i) {
        char c = pvName[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_';
        out.push_back(ok ? c : '_');
    }
    if (out.empty() || !((out[0] >= 'a' && out[0] <= 'z') ||
                         (out[0] >= 'A' && out[0] <= 'Z')))
        out = "f_" + out;
    /* MATLAB identifiers are capped at namelengthmax (63). */
    if (out.size() > 63) out.resize(63);
    return out;
}

/* Build a uniquified, MATLAB-legal field-name list from PV field names,
 * preserving order. */
static std::vector<std::string> legalFieldNames(const std::vector<std::string> &names)
{
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (size_t i = 0; i < names.size(); ++i) {
        std::string n = mxFieldName(names[i]);
        std::string base = n;
        int k = 2;
        while (seen.count(n)) {
            std::ostringstream oss;
            oss << base << "_" << k++;
            n = oss.str();
        }
        seen.insert(n);
        out.push_back(n);
    }
    return out;
}

static std::string safeId(const Value &v)
{
    if (!v) return std::string();
    try { return v.id(); } catch (std::exception &) { return std::string(); }
}

/* True for a plain scalar leaf (not an array, not Struct/Union/Any). */
static bool isScalarLeaf(TypeCode tc)
{
    return !tc.isarray() &&
           tc.code != TypeCode::Struct && tc.code != TypeCode::Union &&
           tc.code != TypeCode::Any;
}

/* ------------------------------------------------------------------ */
/* PV -> MATLAB                                                        */
/* ------------------------------------------------------------------ */

static mxArray *anyToMx(const Value &v, PvaError &err);   /* fwd */

static mxArray *scalarToMx(const Value &v)
{
    switch (v.type().code) {
    case TypeCode::Bool:
        return mxCreateLogicalScalar(v.as<bool>());
    case TypeCode::String:
        return mxCreateString(v.as<std::string>().c_str());
    default:                                   /* all numeric widths -> double */
        return mxCreateDoubleScalar(v.as<double>());
    }
}

template<typename T>
static mxArray *numArrToMx(const Value &v)
{
    shared_array<const T> a(v.as<shared_array<const T> >());
    mxArray *m = mxCreateDoubleMatrix(1, a.size(), mxREAL);
    double *p = mxGetPr(m);
    for (size_t i = 0; i < a.size(); ++i) p[i] = (double)a[i];
    return m;
}

static mxArray *arrayToMx(const Value &v, PvaError &err)
{
    switch (v.type().code) {
    case TypeCode::StringA: {
        shared_array<const std::string> a(v.as<shared_array<const std::string> >());
        mxArray *cell = mxCreateCellMatrix(1, a.size());
        for (size_t i = 0; i < a.size(); ++i)
            mxSetCell(cell, i, mxCreateString(a[i].c_str()));
        return cell;
    }
    case TypeCode::BoolA: {
        shared_array<const bool> a(v.as<shared_array<const bool> >());
        mxArray *m = mxCreateLogicalMatrix(1, a.size());
        mxLogical *p = mxGetLogicals(m);
        for (size_t i = 0; i < a.size(); ++i) p[i] = a[i];
        return m;
    }
    case TypeCode::Int8A:    return numArrToMx<int8_t>(v);
    case TypeCode::Int16A:   return numArrToMx<int16_t>(v);
    case TypeCode::Int32A:   return numArrToMx<int32_t>(v);
    case TypeCode::Int64A:   return numArrToMx<int64_t>(v);
    case TypeCode::UInt8A:   return numArrToMx<uint8_t>(v);
    case TypeCode::UInt16A:  return numArrToMx<uint16_t>(v);
    case TypeCode::UInt32A:  return numArrToMx<uint32_t>(v);
    case TypeCode::UInt64A:  return numArrToMx<uint64_t>(v);
    case TypeCode::Float32A: return numArrToMx<float>(v);
    case TypeCode::Float64A: return numArrToMx<double>(v);
    default:
        if (err.err == PVA_OK) {
            err.err = PVA_UNSUPPORTED;
            err.msg = "unrepresentable array type";
        }
        return mxCreateDoubleMatrix(0, 0, mxREAL);
    }
}

/* Element count of a scalar-array Value (0 on anything else). */
static size_t arrayLen(const Value &v)
{
    switch (v.type().code) {
    case TypeCode::StringA:
        return v.as<shared_array<const std::string> >().size();
    case TypeCode::BoolA:
        return v.as<shared_array<const bool> >().size();
    case TypeCode::Int8A:    return v.as<shared_array<const int8_t> >().size();
    case TypeCode::Int16A:   return v.as<shared_array<const int16_t> >().size();
    case TypeCode::Int32A:   return v.as<shared_array<const int32_t> >().size();
    case TypeCode::Int64A:   return v.as<shared_array<const int64_t> >().size();
    case TypeCode::UInt8A:   return v.as<shared_array<const uint8_t> >().size();
    case TypeCode::UInt16A:  return v.as<shared_array<const uint16_t> >().size();
    case TypeCode::UInt32A:  return v.as<shared_array<const uint32_t> >().size();
    case TypeCode::UInt64A:  return v.as<shared_array<const uint64_t> >().size();
    case TypeCode::Float32A: return v.as<shared_array<const float> >().size();
    case TypeCode::Float64A: return v.as<shared_array<const double> >().size();
    default: return 0;
    }
}

/* NT-aware sugar: an enum_t value structure -> struct(index,choices,choice). */
static mxArray *enumToMx(const Value &pv)
{
    const char *fn[3] = { "index", "choices", "choice" };
    mxArray *s = mxCreateStructMatrix(1, 1, 3, fn);

    int32_t i = -1;
    Value idx = pv["index"];
    if (idx) { try { i = idx.as<int32_t>(); } catch (std::exception &) {} }
    mxSetFieldByNumber(s, 0, 0, mxCreateDoubleScalar((double)i));

    Value ch = pv["choices"];
    if (ch) {
        shared_array<const std::string> v(ch.as<shared_array<const std::string> >());
        mxArray *cell = mxCreateCellMatrix(1, v.size());
        for (size_t k = 0; k < v.size(); ++k)
            mxSetCell(cell, k, mxCreateString(v[k].c_str()));
        mxSetFieldByNumber(s, 0, 1, cell);
        if (i >= 0 && (size_t)i < v.size())
            mxSetFieldByNumber(s, 0, 2, mxCreateString(v[i].c_str()));
        else
            mxSetFieldByNumber(s, 0, 2, mxCreateString(""));
    } else {
        mxSetFieldByNumber(s, 0, 1, mxCreateCellMatrix(1, 0));
        mxSetFieldByNumber(s, 0, 2, mxCreateString(""));
    }
    return s;
}

static mxArray *structToMx(const Value &pv, PvaError &err)
{
    if (safeId(pv) == "enum_t")
        return enumToMx(pv);

    std::vector<std::string> names;
    std::vector<Value>       kids;
    for (Value fld : pv.ichildren()) {
        names.push_back(pv.nameOf(fld));
        kids.push_back(fld);
    }
    std::vector<std::string> fn = legalFieldNames(names);
    std::vector<const char *> fnp(fn.size());
    for (size_t i = 0; i < fn.size(); ++i) fnp[i] = fn[i].c_str();

    mxArray *s = mxCreateStructMatrix(1, 1, (int)fn.size(),
                                      fn.empty() ? NULL : &fnp[0]);
    for (size_t i = 0; i < kids.size(); ++i)
        mxSetFieldByNumber(s, 0, (int)i, anyToMx(kids[i], err));
    return s;
}

static mxArray *structArrayToMx(const Value &arr, PvaError &err)
{
    shared_array<const Value> view(arr.as<shared_array<const Value> >());
    size_t n = view.size();

    /* Field names from the first valid element (elements are homogeneous). */
    std::vector<std::string> names;
    for (size_t e = 0; e < n; ++e) {
        if (!view[e]) continue;
        for (Value fld : view[e].ichildren())
            names.push_back(view[e].nameOf(fld));
        break;
    }
    std::vector<std::string> fn = legalFieldNames(names);
    std::vector<const char *> fnp(fn.size());
    for (size_t i = 0; i < fn.size(); ++i) fnp[i] = fn[i].c_str();

    mxArray *s = mxCreateStructMatrix(1, n, (int)fn.size(),
                                      fn.empty() ? NULL : &fnp[0]);
    for (size_t e = 0; e < n; ++e) {
        if (!view[e]) continue;                 /* null element -> empty fields */
        size_t i = 0;
        for (Value fld : view[e].ichildren()) {
            if (i >= fn.size()) break;
            mxSetFieldByNumber(s, e, (int)i, anyToMx(fld, err));
            ++i;
        }
    }
    return s;
}

static mxArray *anyToMx(const Value &v, PvaError &err)
{
    if (!v) return mxCreateDoubleMatrix(0, 0, mxREAL);

    TypeCode tc = v.type();
    switch (tc.code) {
    case TypeCode::Struct:
        return structToMx(v, err);
    case TypeCode::Union:
    case TypeCode::Any: {
        Value stored = v["->"];                 /* the selected member */
        return stored ? anyToMx(stored, err)
                      : mxCreateDoubleMatrix(0, 0, mxREAL);
    }
    case TypeCode::StructA:
        return structArrayToMx(v, err);
    case TypeCode::UnionA:
    case TypeCode::AnyA: {
        shared_array<const Value> view(v.as<shared_array<const Value> >());
        size_t n = view.size();
        mxArray *cell = mxCreateCellMatrix(1, n);
        for (size_t i = 0; i < n; ++i) {
            Value e = view[i];
            if (e && (e.type().code == TypeCode::Union ||
                      e.type().code == TypeCode::Any))
                e = e["->"];
            mxSetCell(cell, i, e ? anyToMx(e, err)
                                 : mxCreateDoubleMatrix(0, 0, mxREAL));
        }
        return cell;
    }
    default:
        if (tc.isarray()) return arrayToMx(v, err);
        return scalarToMx(v);
    }
}

/* Exception barrier shared by the public entry points: no C++ exception may
 * unwind through the extern-"C" mexFunction boundary (std::terminate). Any
 * escape (e.g. std::bad_alloc) is converted to a PvaError. */
#define LABPVA_CONVERT_CATCH(errref, retexpr)                                  \
    catch (std::exception &e_) {                                               \
        if ((errref).err == PVA_OK) {                                          \
            (errref).err = PVA_FAILURE;                                        \
            (errref).msg = std::string("value conversion failed: ") + e_.what(); \
        }                                                                      \
        return (retexpr);                                                      \
    }

mxArray *pvStructureToMx(const PvValue &pv, PvaError &err)
{
    try {
        if (!pv) return mxCreateStructMatrix(1, 1, 0, NULL);
        if (pv.type().code == TypeCode::Struct) return structToMx(pv, err);
        return anyToMx(pv, err);
    }
    LABPVA_CONVERT_CATCH(err, mxCreateStructMatrix(1, 1, 0, NULL))
}

mxArray *pvValueToMx(const PvValue &pv, char typeReq, PvaError &err)
{
    try {                                   /* barrier; body unindented */
    if (!pv) return mxCreateDoubleMatrix(0, 0, mxREAL);

    Value value = pv["value"];
    if (!value)                       /* not a value-bearing NT: hand back tree */
        return pvStructureToMx(pv, err);

    TypeCode tc = value.type();

    /* enum_t value: labca's lcaGet on an mbbi returns the *string*; mirror
     * that by default, but honour a numeric request by returning the index. */
    if (tc.code == TypeCode::Struct) {
        if (safeId(value) == "enum_t") {
            int32_t i = -1;
            Value idx = value["index"];
            if (idx) { try { i = idx.as<int32_t>(); } catch (std::exception &) {} }
            if (typeReq != 'C' && typeReq != 'c') {
                /* numeric request -> index */
                if (typeReq && typeReq != 'N')
                    return mxCreateDoubleScalar((double)i);
            }
            Value ch = value["choices"];
            if (ch) {
                shared_array<const std::string> v(
                    ch.as<shared_array<const std::string> >());
                if (i >= 0 && (size_t)i < v.size())
                    return mxCreateString(v[i].c_str());
            }
            return mxCreateDoubleScalar((double)i);
        }
        return pvStructureToMx(pv, err);  /* non-enum structured value -> whole tree */
    }

    /* Only a plain scalar or scalar-array value is returned bare (labca-
     * faithful). Anything richer -- a union (NTNDArray image), structureArray
     * or unionArray -- is handed back as the whole nested structure. */
    if (!isScalarLeaf(tc) && !(tc.isarray() && tc.code != TypeCode::StructA &&
                               tc.code != TypeCode::UnionA &&
                               tc.code != TypeCode::AnyA))
        return pvStructureToMx(pv, err);

    /* 'C' forces string presentation of a scalar/array value. */
    if (typeReq == 'C' || typeReq == 'c') {
        if (!tc.isarray())
            return mxCreateString(value.as<std::string>().c_str());
        if (tc.code == TypeCode::StringA)
            return arrayToMx(value, err);
        /* numeric array -> cell of numeric strings */
        mxArray *dbl = arrayToMx(value, err);
        size_t n = mxGetNumberOfElements(dbl);
        double *p = mxGetPr(dbl);
        mxArray *cell = mxCreateCellMatrix(1, n);
        for (size_t i = 0; i < n; ++i) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%g", p[i]);
            mxSetCell(cell, i, mxCreateString(buf));
        }
        mxDestroyArray(dbl);
        return cell;
    }

    return tc.isarray() ? arrayToMx(value, err) : scalarToMx(value);
    }
    LABPVA_CONVERT_CATCH(err, mxCreateDoubleMatrix(0, 0, mxREAL))
}

bool pvIsNTTable(const PvValue &pv)
{
    const std::string id = safeId(pv);
    const std::string prefix = "epics:nt/NTTable:";
    return id.compare(0, prefix.size(), prefix) == 0;
}

mxArray *pvTableToMx(const PvValue &pv, PvaError &err)
{
    try {
        if (!pvIsNTTable(pv)) {
            err.err = PVA_TYPEMISMATCH;
            err.msg = "channel is not an NTTable";
            return mxCreateDoubleMatrix(0, 0, mxREAL);
        }

        Value labels = pv["labels"];
        Value value = pv["value"];
        if (!labels || !value || value.type().code != TypeCode::Struct) {
            err.err = PVA_NOFIELD;
            err.msg = "NTTable must contain labels and a structured value field";
            return mxCreateDoubleMatrix(0, 0, mxREAL);
        }

        shared_array<const std::string> names(
            labels.as<shared_array<const std::string> >());
        std::vector<mxArray *> args;
        args.reserve(names.size() + 2);
        for (size_t i = 0; i < names.size(); ++i) {
            Value field = value[names[i]];
            if (!field) {
                err.err = PVA_NOFIELD;
                err.msg = std::string("NTTable value is missing column '") + names[i] + "'";
                break;
            }
            mxArray *row = anyToMx(field, err);
            if (err.err != PVA_OK) {
                mxDestroyArray(row);
                break;
            }
            if (field.type().code == TypeCode::StringA) {
                mxArray *strings = NULL;
                mxArray *trap = mexCallMATLABWithTrap(1, &strings, 1, &row, "string");
                mxDestroyArray(row);
                if (trap) {
                    mxDestroyArray(trap);
                    err.err = PVA_FAILURE;
                    err.msg = std::string("could not convert NTTable column '") +
                              names[i] + "' to MATLAB strings";
                    break;
                }
                row = strings;
            }
            mxArray *column = NULL;
            mxArray *trap = mexCallMATLABWithTrap(1, &column, 1, &row, "transpose");
            mxDestroyArray(row);
            if (trap) {
                mxDestroyArray(trap);
                err.err = PVA_FAILURE;
                err.msg = std::string("could not shape NTTable column '") + names[i] + "'";
                break;
            }
            args.push_back(column);
        }

        mxArray *table = NULL;
        if (err.err == PVA_OK) {
            args.push_back(mxCreateString("VariableNames"));
            mxArray *nameCell = mxCreateCellMatrix(1, names.size());
            for (size_t i = 0; i < names.size(); ++i)
                mxSetCell(nameCell, i, mxCreateString(names[i].c_str()));
            args.push_back(nameCell);
            mxArray *trap = mexCallMATLABWithTrap(1, &table, args.size(), &args[0], "table");
            if (trap) {
                mxDestroyArray(trap);
                err.err = PVA_FAILURE;
                err.msg = "could not construct a MATLAB table from the NTTable columns";
            }
        }
        for (size_t i = 0; i < args.size(); ++i) mxDestroyArray(args[i]);
        return table ? table : mxCreateDoubleMatrix(0, 0, mxREAL);
    }
    LABPVA_CONVERT_CATCH(err, mxCreateDoubleMatrix(0, 0, mxREAL))
}

void pvValidateTableColumns(const PvValue &pv, const mxArray *columns, PvaError &err)
{
    try {
        if (!pvIsNTTable(pv) || !columns || !mxIsStruct(columns) ||
            mxGetNumberOfElements(columns) != 1) {
            err.err = PVA_TYPEMISMATCH;
            err.msg = "NTTable value must be a scalar struct of columns";
            return;
        }
        Value value = pv["value"];
        if (!value || value.type().code != TypeCode::Struct) {
            err.err = PVA_NOFIELD;
            err.msg = "NTTable has no structured value field";
            return;
        }
        std::vector<std::string> names;
        for (Value field : value.ichildren()) names.push_back(value.nameOf(field));
        std::vector<std::string> valid = legalFieldNames(names);
        for (int field = 0; field < mxGetNumberOfFields(columns); ++field) {
            const char *name = mxGetFieldNameByNumber(columns, field);
            if (std::find(valid.begin(), valid.end(), name ? name : "") == valid.end()) {
                err.err = PVA_NOFIELD;
                err.msg = std::string("NTTable has no column '") + (name ? name : "") + "'";
                return;
            }
        }
    } catch (std::exception &e) {
        if (err.err == PVA_OK) {
            err.err = PVA_FAILURE;
            err.msg = std::string("NTTable column validation failed: ") + e.what();
        }
    }
}

void pvTimeStampSecNsec(const PvValue &pv, double &secOut, double &nsecOut)
{
    secOut = 0.0; nsecOut = 0.0;
    if (!pv) return;
    Value ts = pv["timeStamp"];
    if (!ts) return;
    try {
        Value s = ts["secondsPastEpoch"];
        if (s) secOut = s.as<double>();
        Value n = ts["nanoseconds"];
        if (n) nsecOut = n.as<double>();
    } catch (std::exception &) { secOut = 0.0; nsecOut = 0.0; }
}

mxArray *pvAlarmToMx(const PvValue &pv)
{
    const char *fn[3] = { "severity", "status", "message" };
    mxArray *s = mxCreateStructMatrix(1, 1, 3, fn);
    double sev = 0, sta = 0;
    std::string msg;
    try {
    if (pv) {
        Value a = pv["alarm"];
        if (a) {
            try {
                Value f;
                if ((f = a["severity"])) sev = f.as<double>();
                if ((f = a["status"]))   sta = f.as<double>();
                if ((f = a["message"]))  msg = f.as<std::string>();
            } catch (std::exception &) {}
        }
    }
    } catch (std::exception &) { sev = 0; sta = 0; msg.clear(); }
    mxSetFieldByNumber(s, 0, 0, mxCreateDoubleScalar(sev));
    mxSetFieldByNumber(s, 0, 1, mxCreateDoubleScalar(sta));
    mxSetFieldByNumber(s, 0, 2, mxCreateString(msg.c_str()));
    return s;
}

/* ------------------------------------------------------------------ */
/* introspection helpers (keep the MEX free of backend types)          */
/* ------------------------------------------------------------------ */

double pvValueNelem(const PvValue &pv)
{
    try {
    if (!pv) return 1.0;
    Value v = pv["value"];
    if (v) {
        size_t n = arrayLen(v);       /* 0 unless a scalar array */
        if (n || v.type().isarray()) {
            switch (v.type().code) {  /* struct/union arrays still count as 1 */
            case TypeCode::StructA:
            case TypeCode::UnionA:
            case TypeCode::AnyA:
                break;
            default:
                return (double)n;
            }
        }
    }
    return 1.0;
    } catch (std::exception &) { return 1.0; }
}

mxArray *pvEnumChoicesToMx(const PvValue &pv)
{
    try {
    if (pv) {
        Value value = pv["value"];
        if (value && value.type().code == TypeCode::Struct &&
            safeId(value) == "enum_t") {
            Value ch = value["choices"];
            if (ch) {
                shared_array<const std::string> v(
                    ch.as<shared_array<const std::string> >());
                mxArray *cell = mxCreateCellMatrix(1, v.size());
                for (size_t i = 0; i < v.size(); ++i)
                    mxSetCell(cell, i, mxCreateString(v[i].c_str()));
                return cell;
            }
        }
    }
    return mxCreateCellMatrix(1, 0);
    } catch (std::exception &) { return mxCreateCellMatrix(1, 0); }
}

std::string pvTypeId(const PvValue &pv)
{
    return safeId(pv);
}

std::string pvIntrospect(const PvValue &pv)
{
    try {
        std::ostringstream oss;
        if (pv) oss << pv;            /* pvxs's own tree+value dump */
        return oss.str();
    } catch (std::exception &e) {
        return std::string("<introspection failed: ") + e.what() + ">";
    }
}

double getDoubleField(const PvValue &pv, const std::string &path, double dflt)
{
    if (!pv) return dflt;
    Value f = pv[path];
    if (!f || !isScalarLeaf(f.type())) return dflt;
    try { return f.as<double>(); } catch (std::exception &) { return dflt; }
}

std::string getStringField(const PvValue &pv, const std::string &path)
{
    if (!pv) return "";
    Value f = pv[path];
    if (!f || !isScalarLeaf(f.type())) return "";
    try { return f.as<std::string>(); } catch (std::exception &) { return ""; }
}

/* ------------------------------------------------------------------ */
/* MATLAB -> PV (write path; see pvaConvert.h)                         */
/* ------------------------------------------------------------------ */

static std::string getStdString(const mxArray *mx)
{
    char *c = mxArrayToString(mx);
    std::string s = c ? c : "";
    if (c) mxFree(c);
    return s;
}

/* Any numeric/logical MATLAB array -> doubles (no mexCallMATLAB, so this is
 * also usable outside a MEX context). False if the class is unsupported. */
static bool mxNumericToDoubles(const mxArray *mx, std::vector<double> &out)
{
    size_t n = mxGetNumberOfElements(mx);
    out.resize(n);
    if (mxIsLogical(mx)) {
        mxLogical *p = mxGetLogicals(mx);
        for (size_t i = 0; i < n; ++i) out[i] = p[i] ? 1.0 : 0.0;
        return true;
    }
    switch (mxGetClassID(mx)) {
#define LABPVA_CASE(CLS, T) \
    case CLS: { const T *p = (const T *)mxGetData(mx); \
                for (size_t i = 0; i < n; ++i) out[i] = (double)p[i]; \
                return true; }
    LABPVA_CASE(mxDOUBLE_CLASS, double)
    LABPVA_CASE(mxSINGLE_CLASS, float)
    LABPVA_CASE(mxINT8_CLASS,   int8_t)
    LABPVA_CASE(mxUINT8_CLASS,  uint8_t)
    LABPVA_CASE(mxINT16_CLASS,  int16_t)
    LABPVA_CASE(mxUINT16_CLASS, uint16_t)
    LABPVA_CASE(mxINT32_CLASS,  int32_t)
    LABPVA_CASE(mxUINT32_CLASS, uint32_t)
    LABPVA_CASE(mxINT64_CLASS,  int64_t)
    LABPVA_CASE(mxUINT64_CLASS, uint64_t)
#undef LABPVA_CASE
    default: return false;
    }
}

template<typename T>
static void arrFromDoubles(Value &target, const std::vector<double> &d)
{
    shared_array<T> tmp(d.size());
    for (size_t i = 0; i < d.size(); ++i) tmp[i] = (T)d[i];
    target.from(tmp.freeze());
}

/* Populate an existing target field from a MATLAB value (recursive; the
 * target's type is authoritative). Assignment marks the written fields. */
static void mxIntoValue(const mxArray *mx, Value target, PvaError &err)
{
    if (!target) { err.err = PVA_NOFIELD; err.msg = "null target field"; return; }
    TypeCode tc = target.type();

    switch (tc.code) {
    case TypeCode::Struct: {
        if (!mxIsStruct(mx)) {
            err.err = PVA_TYPEMISMATCH;
            err.msg = "structure PV field needs a MATLAB struct";
            return;
        }
        std::vector<std::string> names;
        std::vector<Value>       kids;
        for (Value f : target.ichildren()) {
            names.push_back(target.nameOf(f));
            kids.push_back(f);
        }
        std::vector<std::string> fn = legalFieldNames(names);
        for (size_t i = 0; i < kids.size(); ++i) {
            int idx = mxGetFieldNumber(mx, fn[i].c_str());
            if (idx < 0) continue;          /* MATLAB omitted it -> leave as-is */
            mxArray *v = mxGetFieldByNumber(mx, 0, idx);
            if (v) mxIntoValue(v, kids[i], err);
            if (err.err != PVA_OK) return;
        }
        return;
    }
    case TypeCode::StructA: {
        if (!mxIsStruct(mx)) {
            err.err = PVA_TYPEMISMATCH;
            err.msg = "structureArray PV field needs a MATLAB struct array";
            return;
        }
        size_t n = mxGetNumberOfElements(mx);
        shared_array<Value> vec(n);
        for (size_t e = 0; e < n; ++e) {
            Value es(target.allocMember());
            std::vector<std::string> names;
            std::vector<Value>       kids;
            for (Value f : es.ichildren()) {
                names.push_back(es.nameOf(f));
                kids.push_back(f);
            }
            std::vector<std::string> fn = legalFieldNames(names);
            for (size_t i = 0; i < kids.size(); ++i) {
                int idx = mxGetFieldNumber(mx, fn[i].c_str());
                if (idx < 0) continue;
                mxArray *v = mxGetFieldByNumber(mx, e, idx);
                if (v) mxIntoValue(v, kids[i], err);
                if (err.err != PVA_OK) return;
            }
            vec[e] = es;
        }
        target.from(vec.freeze());
        return;
    }
    case TypeCode::Union:
    case TypeCode::Any:
    case TypeCode::UnionA:
    case TypeCode::AnyA:
        err.err = PVA_UNSUPPORTED;
        err.msg = "cannot write this PV field type";
        return;
    default:
        break;
    }

    if (tc.isarray()) {                     /* scalar arrays */
        if (tc.code == TypeCode::StringA) {
            shared_array<std::string> tmp;
            if (mxIsCell(mx)) {
                size_t n = mxGetNumberOfElements(mx);
                tmp = shared_array<std::string>(n);
                for (size_t i = 0; i < n; ++i) {
                    mxArray *c = mxGetCell(mx, i);
                    tmp[i] = c ? getStdString(c) : std::string();
                }
            } else if (mxIsChar(mx)) {      /* single string -> 1-elem array */
                tmp = shared_array<std::string>(1);
                tmp[0] = getStdString(mx);
            } else {
                err.err = PVA_TYPEMISMATCH;
                err.msg = "string-array PV field needs a cell array of char";
                return;
            }
            target.from(tmp.freeze());
            return;
        }
        std::vector<double> d;
        if (!mxNumericToDoubles(mx, d)) {
            err.err = PVA_TYPEMISMATCH;
            err.msg = "numeric-array PV field needs a numeric MATLAB array";
            return;
        }
        switch (tc.code) {
        case TypeCode::BoolA: {
            shared_array<bool> tmp(d.size());
            for (size_t i = 0; i < d.size(); ++i) tmp[i] = d[i] != 0.0;
            target.from(tmp.freeze());
            return;
        }
        case TypeCode::Int8A:    arrFromDoubles<int8_t>(target, d);   return;
        case TypeCode::Int16A:   arrFromDoubles<int16_t>(target, d);  return;
        case TypeCode::Int32A:   arrFromDoubles<int32_t>(target, d);  return;
        case TypeCode::Int64A:   arrFromDoubles<int64_t>(target, d);  return;
        case TypeCode::UInt8A:   arrFromDoubles<uint8_t>(target, d);  return;
        case TypeCode::UInt16A:  arrFromDoubles<uint16_t>(target, d); return;
        case TypeCode::UInt32A:  arrFromDoubles<uint32_t>(target, d); return;
        case TypeCode::UInt64A:  arrFromDoubles<uint64_t>(target, d); return;
        case TypeCode::Float32A: arrFromDoubles<float>(target, d);    return;
        case TypeCode::Float64A: arrFromDoubles<double>(target, d);   return;
        default:
            err.err = PVA_UNSUPPORTED;
            err.msg = "cannot write this PV array type";
            return;
        }
    }

    /* scalar leaf: pvxs converts on assignment (string parses, double
     * narrows), so hand over the natural representation */
    if ((mxIsNumeric(mx) || mxIsLogical(mx)) && mxGetNumberOfElements(mx) == 0) {
        err.err = PVA_TYPEMISMATCH;         /* mxGetScalar on [] is undefined */
        err.msg = "cannot write an empty value to a scalar PV field";
        return;
    }
    try {
        if (mxIsChar(mx)) {
            target.from(getStdString(mx));
        } else if (mxIsLogical(mx)) {
            bool b = mxIsLogicalScalarTrue(mx);
            target.from(b);
        } else if (mxIsNumeric(mx)) {
            target.from(mxGetScalar(mx));
        } else {
            err.err = PVA_TYPEMISMATCH;
            err.msg = "cannot assign this MATLAB type to a scalar PV field";
        }
    } catch (std::exception &e) {
        err.err = PVA_TYPEMISMATCH;
        err.msg = std::string("cannot convert value for PV field: ") + e.what();
    }
}

PvValue mxToPutArg(const mxArray *mx, const PvValue &fetched, char typeReq, PvaError &err)
{
    try {                                   /* barrier; body unindented */
    (void)typeReq;                          /* kept for signature parity */
    if (!fetched) {
        err.err = PVA_NOFIELD;
        err.msg = "no structure to write";
        return PvValue();
    }
    Value arg(fetched.cloneEmpty());
    Value value = arg["value"];
    if (value) {
        /* enum value: accept a numeric index, or a string matched to choices
         * (read from the FETCHED value -- the empty clone has no data) */
        if (value.type().code == TypeCode::Struct && safeId(value) == "enum_t") {
            shared_array<const std::string> choices;
            Value ch = fetched["value"]["choices"];
            if (ch) {
                try { choices = ch.as<shared_array<const std::string> >(); }
                catch (std::exception &) {}
            }
            int32_t i = -1;
            if (mxIsChar(mx)) {
                std::string want = getStdString(mx);
                bool found = false;
                for (size_t k = 0; k < choices.size(); ++k)
                    if (choices[k] == want) { i = (int32_t)k; found = true; break; }
                if (!found) {
                    err.err = PVA_TYPEMISMATCH;
                    err.msg = "enum string not among choices";
                    return PvValue();
                }
            } else if (mxIsNumeric(mx) || mxIsLogical(mx)) {
                if (mxGetNumberOfElements(mx) == 0) {
                    err.err = PVA_TYPEMISMATCH;
                    err.msg = "cannot write an empty value to an enum PV";
                    return PvValue();
                }
                i = (int32_t)mxGetScalar(mx);
                if (!choices.empty() && (i < 0 || (size_t)i >= choices.size())) {
                    err.err = PVA_INVALIDARG;
                    err.msg = "enum index out of range";
                    return PvValue();
                }
            } else {
                err.err = PVA_TYPEMISMATCH;
                err.msg = "enum PV needs a choice string or a numeric index";
                return PvValue();
            }
            Value idx = value["index"];
            if (idx) idx.from(i);           /* marks value.index */
            return err.err == PVA_OK ? arg : PvValue();
        }
        mxIntoValue(mx, value, err);
        return err.err == PVA_OK ? arg : PvValue();
    }

    /* No "value" field: if the structure has exactly one field, write it. */
    Value only;
    int count = 0;
    for (Value f : arg.ichildren()) { if (count++ == 0) only = f; }
    if (count == 1) {
        mxIntoValue(mx, only, err);
        return err.err == PVA_OK ? arg : PvValue();
    }
    err.err = PVA_NOFIELD;
    err.msg = "structure has no 'value' field; use pvaPutStructure";
    return PvValue();
    }
    LABPVA_CONVERT_CATCH(err, PvValue())
}

PvValue mxToPutArgStructure(const mxArray *mx, const PvValue &fetched, PvaError &err)
{
    try {
        if (!fetched) {
            err.err = PVA_NOFIELD;
            err.msg = "no structure to write";
            return PvValue();
        }
        if (!mxIsStruct(mx)) {
            err.err = PVA_INVALIDARG;
            err.msg = "pvaPutStructure: value must be a struct";
            return PvValue();
        }
        Value arg(fetched.cloneEmpty());
        mxIntoValue(mx, arg, err);
        return err.err == PVA_OK ? arg : PvValue();
    }
    LABPVA_CONVERT_CATCH(err, PvValue())
}

} // namespace labpva
