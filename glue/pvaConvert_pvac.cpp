/* pvaConvert_pvac.cpp - see pvaConvert.h. Classic-backend implementation
 * (pvDataCPP). The PVXS implementation is pvaConvert_pvxs.cpp; the Makefile
 * picks one via LABPVA_BACKEND.
 *
 * All EPICS type access goes through the generic Convert path
 * (PVScalar::getAs<T>/putFrom<T>, PVScalarArray::getAs<T>/putFrom<T>) so we
 * never have to switch on the 12 scalar types by hand: numeric leaves come
 * back as double, strings as std::string, regardless of the PV's native width.
 */
#include "pvaConvert.h"

#include <pv/pvTimeStamp.h>
#include <pv/timeStamp.h>
#include <pv/pvAlarm.h>
#include <pv/alarm.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <cstring>

using namespace epics::pvData;

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

static std::string getStdString(const mxArray *mx)
{
    char *c = mxArrayToString(mx);
    std::string s = c ? c : "";
    if (c) mxFree(c);
    return s;
}

/* Build a uniquified, MATLAB-legal field-name list from PV field names,
 * preserving order. */
static std::vector<std::string> legalFieldNames(const StringArray &names)
{
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (size_t i = 0; i < names.size(); ++i) {
        std::string n = mxFieldName(names[i]);
        std::string base = n;
        int k = 2;
        while (seen.count(n)) { n = base + "_" + std::to_string(k++); }
        seen.insert(n);
        out.push_back(n);
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* PV -> MATLAB                                                        */
/* ------------------------------------------------------------------ */

static mxArray *scalarToMx(const PVScalarPtr &s, PvaError &err)
{
    ScalarType st = s->getScalar()->getScalarType();
    if (st == pvBoolean)
        return mxCreateLogicalScalar(s->getAs<boolean>() ? true : false);
    if (st == pvString)
        return mxCreateString(s->getAs<std::string>().c_str());
    return mxCreateDoubleScalar(s->getAs<double>());
}

static mxArray *scalarArrayToMx(const PVScalarArrayPtr &a, PvaError &err)
{
    ScalarType st = a->getScalarArray()->getElementType();
    if (st == pvString) {
        shared_vector<const std::string> v;
        a->getAs<std::string>(v);
        size_t n = v.size();
        mxArray *cell = mxCreateCellMatrix(1, n);
        for (size_t i = 0; i < n; ++i)
            mxSetCell(cell, i, mxCreateString(v[i].c_str()));
        return cell;
    }
    if (st == pvBoolean) {
        shared_vector<const boolean> v;
        a->getAs<boolean>(v);
        size_t n = v.size();
        mxArray *m = mxCreateLogicalMatrix(1, n);
        mxLogical *p = mxGetLogicals(m);
        for (size_t i = 0; i < n; ++i) p[i] = v[i] ? true : false;
        return m;
    }
    shared_vector<const double> v;
    a->getAs<double>(v);
    size_t n = v.size();
    mxArray *m = mxCreateDoubleMatrix(1, n, mxREAL);
    if (n) memcpy(mxGetPr(m), v.data(), n * sizeof(double));
    return m;
}

/* NT-aware sugar: an enum_t value structure -> struct(index,choices,choice). */
static mxArray *enumToMx(const PVStructurePtr &pv)
{
    const char *fn[3] = { "index", "choices", "choice" };
    mxArray *s = mxCreateStructMatrix(1, 1, 3, fn);

    PVIntPtr idx = pv->getSubField<PVInt>("index");
    int32 i = idx ? idx->get() : -1;
    mxSetFieldByNumber(s, 0, 0, mxCreateDoubleScalar((double)i));

    PVStringArrayPtr ch = pv->getSubField<PVStringArray>("choices");
    if (ch) {
        shared_vector<const std::string> v = ch->view();
        size_t n = v.size();
        mxArray *cell = mxCreateCellMatrix(1, n);
        for (size_t k = 0; k < n; ++k)
            mxSetCell(cell, k, mxCreateString(v[k].c_str()));
        mxSetFieldByNumber(s, 0, 1, cell);
        if (i >= 0 && (size_t)i < n)
            mxSetFieldByNumber(s, 0, 2, mxCreateString(v[i].c_str()));
        else
            mxSetFieldByNumber(s, 0, 2, mxCreateString(""));
    } else {
        mxSetFieldByNumber(s, 0, 1, mxCreateCellMatrix(1, 0));
        mxSetFieldByNumber(s, 0, 2, mxCreateString(""));
    }
    return s;
}

static mxArray *structureToMx(const PVStructurePtr &pv, PvaError &err)
{
    if (pv->getStructure()->getID() == "enum_t")
        return enumToMx(pv);

    const StringArray &names = pv->getStructure()->getFieldNames();
    std::vector<std::string> fn = legalFieldNames(names);
    std::vector<const char *> fnp(fn.size());
    for (size_t i = 0; i < fn.size(); ++i) fnp[i] = fn[i].c_str();

    mxArray *s = mxCreateStructMatrix(1, 1, (int)fn.size(),
                                      fn.empty() ? NULL : &fnp[0]);
    const PVFieldPtrArray &fields = pv->getPVFields();
    for (size_t i = 0; i < fields.size(); ++i)
        mxSetFieldByNumber(s, 0, (int)i, pvFieldToMx(fields[i], err));
    return s;
}

static mxArray *structureArrayToMx(const PVStructureArrayPtr &arr, PvaError &err)
{
    StructureConstPtr elem = arr->getStructureArray()->getStructure();
    const StringArray &names = elem->getFieldNames();
    std::vector<std::string> fn = legalFieldNames(names);
    std::vector<const char *> fnp(fn.size());
    for (size_t i = 0; i < fn.size(); ++i) fnp[i] = fn[i].c_str();

    PVStructureArray::const_svector view = arr->view();
    size_t n = view.size();
    mxArray *s = mxCreateStructMatrix(1, n, (int)fn.size(),
                                      fn.empty() ? NULL : &fnp[0]);
    for (size_t e = 0; e < n; ++e) {
        if (!view[e]) continue;                 /* null element -> empty fields */
        const PVFieldPtrArray &fields = view[e]->getPVFields();
        for (size_t i = 0; i < fields.size() && i < fn.size(); ++i)
            mxSetFieldByNumber(s, e, (int)i, pvFieldToMx(fields[i], err));
    }
    return s;
}

mxArray *pvFieldToMx(const PVFieldPtr &field, PvaError &err)
{
    if (!field) return mxCreateDoubleMatrix(0, 0, mxREAL);

    switch (field->getField()->getType()) {
    case scalar:
        return scalarToMx(std::tr1::static_pointer_cast<PVScalar>(field), err);
    case scalarArray:
        return scalarArrayToMx(std::tr1::static_pointer_cast<PVScalarArray>(field), err);
    case structure:
        return structureToMx(std::tr1::static_pointer_cast<PVStructure>(field), err);
    case structureArray:
        return structureArrayToMx(std::tr1::static_pointer_cast<PVStructureArray>(field), err);
    case union_: {
        PVUnionPtr u = std::tr1::static_pointer_cast<PVUnion>(field);
        PVFieldPtr stored = u->get();
        return stored ? pvFieldToMx(stored, err)
                      : mxCreateDoubleMatrix(0, 0, mxREAL);
    }
    case unionArray: {
        PVUnionArrayPtr ua = std::tr1::static_pointer_cast<PVUnionArray>(field);
        PVUnionArray::const_svector view = ua->view();
        size_t n = view.size();
        mxArray *cell = mxCreateCellMatrix(1, n);
        for (size_t i = 0; i < n; ++i) {
            PVFieldPtr stored = view[i] ? view[i]->get() : PVFieldPtr();
            mxSetCell(cell, i, stored ? pvFieldToMx(stored, err)
                                      : mxCreateDoubleMatrix(0, 0, mxREAL));
        }
        return cell;
    }
    default:
        if (err.err == PVA_OK) {
            err.err = PVA_UNSUPPORTED;
            err.msg = "unrepresentable field type";
        }
        return mxCreateDoubleMatrix(0, 0, mxREAL);
    }
}

mxArray *pvStructureToMx(const PVStructurePtr &pv, PvaError &err)
{
    if (!pv) return mxCreateStructMatrix(1, 1, 0, NULL);
    return structureToMx(pv, err);
}

mxArray *pvValueToMx(const PVStructurePtr &pv, char typeReq, PvaError &err)
{
    if (!pv) return mxCreateDoubleMatrix(0, 0, mxREAL);

    PVFieldPtr value = pv->getSubField("value");
    if (!value)                       /* not a value-bearing NT: hand back tree */
        return pvStructureToMx(pv, err);

    Type t = value->getField()->getType();

    /* enum_t value: labca's lcaGet on an mbbi returns the *string*; mirror
     * that by default, but honour a numeric request by returning the index. */
    if (t == structure) {
        PVStructurePtr vs = std::tr1::static_pointer_cast<PVStructure>(value);
        if (vs->getStructure()->getID() == "enum_t") {
            PVIntPtr idx = vs->getSubField<PVInt>("index");
            PVStringArrayPtr ch = vs->getSubField<PVStringArray>("choices");
            int32 i = idx ? idx->get() : -1;
            if (typeReq != 'C' && typeReq != 'c') {
                /* numeric request -> index */
                if (typeReq && typeReq != 'N')
                    return mxCreateDoubleScalar((double)i);
            }
            if (ch) {
                shared_vector<const std::string> v = ch->view();
                if (i >= 0 && (size_t)i < v.size())
                    return mxCreateString(v[i].c_str());
            }
            return mxCreateDoubleScalar((double)i);
        }
        return pvStructureToMx(pv, err);  /* non-enum structured value -> whole tree */
    }

    /* Only a plain scalar or scalar-array value is returned bare (labca-faithful
     * -- the value you'd compute with). Anything richer -- a union (NTNDArray
     * image), structureArray or unionArray -- is handed back as the whole nested
     * structure, so pvaGet "just works" on rich PVs (like pvaGetStructure). */
    if (t != scalar && t != scalarArray)
        return pvStructureToMx(pv, err);

    /* 'C' forces string presentation of a scalar/array value. */
    if ((typeReq == 'C' || typeReq == 'c')) {
        if (t == scalar)
            return mxCreateString(
                std::tr1::static_pointer_cast<PVScalar>(value)->getAs<std::string>().c_str());
        if (t == scalarArray) {
            shared_vector<const std::string> v;
            std::tr1::static_pointer_cast<PVScalarArray>(value)->getAs<std::string>(v);
            mxArray *cell = mxCreateCellMatrix(1, v.size());
            for (size_t i = 0; i < v.size(); ++i)
                mxSetCell(cell, i, mxCreateString(v[i].c_str()));
            return cell;
        }
    }

    return pvFieldToMx(value, err);
}

bool pvIsNTTable(const PvValue &pv)
{
    if (!pv) return false;
    const std::string id = pv->getStructure()->getID();
    const std::string prefix = "epics:nt/NTTable:";
    return id.compare(0, prefix.size(), prefix) == 0;
}

mxArray *pvTableToMx(const PvValue &pv, PvaError &err)
{
    if (!pvIsNTTable(pv)) {
        err.err = PVA_TYPEMISMATCH;
        err.msg = "channel is not an NTTable";
        return mxCreateDoubleMatrix(0, 0, mxREAL);
    }

    PVStringArrayPtr labels = pv->getSubField<PVStringArray>("labels");
    PVStructurePtr value = pv->getSubField<PVStructure>("value");
    if (!labels || !value) {
        err.err = PVA_NOFIELD;
        err.msg = "NTTable must contain labels and a structured value field";
        return mxCreateDoubleMatrix(0, 0, mxREAL);
    }

    shared_vector<const std::string> names = labels->view();
    std::vector<mxArray *> args;
    args.reserve(names.size() + 2);
    for (size_t i = 0; i < names.size(); ++i) {
        PVFieldPtr field = value->getSubField(names[i]);
        if (!field) {
            err.err = PVA_NOFIELD;
            err.msg = std::string("NTTable value is missing column '") + names[i] + "'";
            break;
        }
        mxArray *row = pvFieldToMx(field, err);
        if (err.err != PVA_OK) {
            mxDestroyArray(row);
            break;
        }
        if (field->getField()->getType() == scalarArray &&
            std::tr1::static_pointer_cast<PVScalarArray>(field)
                ->getScalarArray()->getElementType() == pvString) {
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

void pvValidateTableColumns(const PvValue &pv, const mxArray *columns, PvaError &err)
{
    if (!pvIsNTTable(pv) || !columns || !mxIsStruct(columns) ||
        mxGetNumberOfElements(columns) != 1) {
        err.err = PVA_TYPEMISMATCH;
        err.msg = "NTTable value must be a scalar struct of columns";
        return;
    }
    PVStructurePtr value = pv->getSubField<PVStructure>("value");
    if (!value) {
        err.err = PVA_NOFIELD;
        err.msg = "NTTable has no structured value field";
        return;
    }
    std::vector<std::string> valid =
        legalFieldNames(value->getStructure()->getFieldNames());
    for (int field = 0; field < mxGetNumberOfFields(columns); ++field) {
        const char *name = mxGetFieldNameByNumber(columns, field);
        if (std::find(valid.begin(), valid.end(), name ? name : "") == valid.end()) {
            err.err = PVA_NOFIELD;
            err.msg = std::string("NTTable has no column '") + (name ? name : "") + "'";
            return;
        }
    }
}

void pvTimeStampSecNsec(const PVStructurePtr &pv, double &secOut, double &nsecOut)
{
    secOut = 0.0; nsecOut = 0.0;
    PVFieldPtr tsf = pv ? pv->getSubField("timeStamp") : PVFieldPtr();
    if (tsf) {
        PVTimeStamp pvts;
        if (pvts.attach(tsf)) {
            TimeStamp t;
            pvts.get(t);
            secOut  = (double)t.getSecondsPastEpoch();
            nsecOut = (double)t.getNanoseconds();
        }
    }
}

mxArray *pvAlarmToMx(const PVStructurePtr &pv)
{
    const char *fn[3] = { "severity", "status", "message" };
    mxArray *s = mxCreateStructMatrix(1, 1, 3, fn);
    double sev = 0, sta = 0;
    std::string msg;
    PVFieldPtr af = pv ? pv->getSubField("alarm") : PVFieldPtr();
    if (af) {
        PVAlarm pva;
        if (pva.attach(af)) {
            Alarm a;
            pva.get(a);
            sev = (double)a.getSeverity();
            sta = (double)a.getStatus();
            msg = a.getMessage();
        }
    }
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
    PVFieldPtr v = pv ? pv->getSubField("value") : PVFieldPtr();
    if (v && v->getField()->getType() == scalarArray)
        return (double)std::tr1::static_pointer_cast<PVScalarArray>(v)->getLength();
    return 1.0;
}

mxArray *pvEnumChoicesToMx(const PvValue &pv)
{
    PVFieldPtr value = pv ? pv->getSubField("value") : PVFieldPtr();
    if (value && value->getField()->getType() == structure) {
        PVStructurePtr vs = std::tr1::static_pointer_cast<PVStructure>(value);
        PVStringArrayPtr ch = vs->getSubField<PVStringArray>("choices");
        if (ch) {
            shared_vector<const std::string> v = ch->view();
            mxArray *cell = mxCreateCellMatrix(1, v.size());
            for (size_t i = 0; i < v.size(); ++i)
                mxSetCell(cell, i, mxCreateString(v[i].c_str()));
            return cell;
        }
    }
    return mxCreateCellMatrix(1, 0);
}

std::string pvTypeId(const PvValue &pv)
{
    return pv ? pv->getStructure()->getID() : std::string();
}

std::string pvIntrospect(const PvValue &pv)
{
    std::ostringstream oss;
    if (pv) oss << *pv;                 /* pvData's own tree+value dump */
    return oss.str();
}

double getDoubleField(const PvValue &pv, const std::string &path, double dflt)
{
    if (!pv) return dflt;
    PVFieldPtr f = pv->getSubField(path);
    if (!f || f->getField()->getType() != scalar) return dflt;
    try {                                   /* nonconforming type -> default */
        return std::tr1::static_pointer_cast<PVScalar>(f)->getAs<double>();
    } catch (std::exception &) { return dflt; }
}

std::string getStringField(const PvValue &pv, const std::string &path)
{
    if (!pv) return "";
    PVFieldPtr f = pv->getSubField(path);
    if (!f || f->getField()->getType() != scalar) return "";
    try {
        return std::tr1::static_pointer_cast<PVScalar>(f)->getAs<std::string>();
    } catch (std::exception &) { return ""; }
}

/* ------------------------------------------------------------------ */
/* MATLAB -> PV                                                        */
/* ------------------------------------------------------------------ */

static void mxToScalar(const mxArray *mx, const PVScalarPtr &s, PvaError &err)
{
    if ((mxIsNumeric(mx) || mxIsLogical(mx)) && mxGetNumberOfElements(mx) == 0) {
        err.err = PVA_TYPEMISMATCH;         /* mxGetScalar on [] is undefined */
        err.msg = "cannot write an empty value to a scalar PV field";
        return;
    }
    if (mxIsChar(mx)) {
        s->putFrom<std::string>(getStdString(mx));
    } else if (mxIsLogical(mx)) {
        s->putFrom<boolean>(mxIsLogicalScalarTrue(mx) ? 1 : 0);
    } else if (mxIsNumeric(mx)) {
        s->putFrom<double>(mxGetScalar(mx));
    } else {
        err.err = PVA_TYPEMISMATCH;
        err.msg = "cannot assign this MATLAB type to a scalar PV field";
    }
}

static void mxToScalarArray(const mxArray *mx, const PVScalarArrayPtr &a, PvaError &err)
{
    ScalarType st = a->getScalarArray()->getElementType();
    if (st == pvString) {
        size_t n = mxGetNumberOfElements(mx);
        shared_vector<std::string> tmp(n);
        if (mxIsCell(mx)) {
            for (size_t i = 0; i < n; ++i) {
                mxArray *c = mxGetCell(mx, i);
                tmp[i] = c ? getStdString(c) : "";
            }
        } else if (mxIsChar(mx)) {          /* single string -> 1-elem array */
            tmp = shared_vector<std::string>(1);
            tmp[0] = getStdString(mx);
        } else {
            err.err = PVA_TYPEMISMATCH;
            err.msg = "string-array PV field needs a cell array of char";
            return;
        }
        a->putFrom<std::string>(freeze(tmp));
        return;
    }
    if (!mxIsNumeric(mx) && !mxIsLogical(mx)) {
        err.err = PVA_TYPEMISMATCH;
        err.msg = "numeric-array PV field needs a numeric MATLAB array";
        return;
    }
    size_t n = mxGetNumberOfElements(mx);
    shared_vector<double> tmp(n);
    if (mxIsDouble(mx) && n) {
        memcpy(tmp.data(), mxGetPr(mx), n * sizeof(double));
    } else {                                /* coerce any numeric/logical */
        mxArray *d = NULL;
        const mxArray *in = mx;
        if (!mxIsDouble(mx)) {
            mexCallMATLAB(1, &d, 1, (mxArray **)&in, "double");
            in = d;
        }
        if (n) memcpy(tmp.data(), mxGetPr(in), n * sizeof(double));
        if (d) mxDestroyArray(d);
    }
    a->putFrom<double>(freeze(tmp));
}

void mxToPvField(const mxArray *mx, const PVFieldPtr &target, PvaError &err)
{
    if (!target) { err.err = PVA_NOFIELD; err.msg = "null target field"; return; }

    switch (target->getField()->getType()) {
    case scalar:
        mxToScalar(mx, std::tr1::static_pointer_cast<PVScalar>(target), err);
        return;
    case scalarArray:
        mxToScalarArray(mx, std::tr1::static_pointer_cast<PVScalarArray>(target), err);
        return;
    case structure: {
        if (!mxIsStruct(mx)) {
            err.err = PVA_TYPEMISMATCH;
            err.msg = "structure PV field needs a MATLAB struct";
            return;
        }
        PVStructurePtr ps = std::tr1::static_pointer_cast<PVStructure>(target);
        const StringArray &names = ps->getStructure()->getFieldNames();
        std::vector<std::string> fn = legalFieldNames(names);
        for (size_t i = 0; i < names.size(); ++i) {
            int idx = mxGetFieldNumber(mx, fn[i].c_str());
            if (idx < 0) continue;          /* MATLAB omitted it -> leave as-is */
            mxArray *v = mxGetFieldByNumber(mx, 0, idx);
            if (v) mxToPvField(v, ps->getPVFields()[i], err);
        }
        return;
    }
    case structureArray: {
        if (!mxIsStruct(mx)) {
            err.err = PVA_TYPEMISMATCH;
            err.msg = "structureArray PV field needs a MATLAB struct array";
            return;
        }
        PVStructureArrayPtr arr = std::tr1::static_pointer_cast<PVStructureArray>(target);
        StructureConstPtr elem = arr->getStructureArray()->getStructure();
        const StringArray &names = elem->getFieldNames();
        std::vector<std::string> fn = legalFieldNames(names);
        size_t n = mxGetNumberOfElements(mx);
        PVDataCreatePtr create = getPVDataCreate();
        PVStructureArray::svector vec(n);
        for (size_t e = 0; e < n; ++e) {
            PVStructurePtr es = create->createPVStructure(elem);
            for (size_t i = 0; i < names.size(); ++i) {
                int idx = mxGetFieldNumber(mx, fn[i].c_str());
                if (idx < 0) continue;
                mxArray *v = mxGetFieldByNumber(mx, e, idx);
                if (v) mxToPvField(v, es->getPVFields()[i], err);
            }
            vec[e] = es;
        }
        arr->replace(freeze(vec));
        return;
    }
    default:
        err.err = PVA_UNSUPPORTED;
        err.msg = "cannot write this PV field type";
        return;
    }
}

std::string mxToPvValue(const mxArray *mx, const PVStructurePtr &pv, char typeReq, PvaError &err)
{
    if (!pv) { err.err = PVA_NOFIELD; err.msg = "no structure to write"; return ""; }

    PVFieldPtr value = pv->getSubField("value");
    if (value) {
        /* enum value: accept a numeric index, or a string matched to choices */
        if (value->getField()->getType() == structure) {
            PVStructurePtr vs = std::tr1::static_pointer_cast<PVStructure>(value);
            if (vs->getStructure()->getID() == "enum_t") {
                PVIntPtr idx = vs->getSubField<PVInt>("index");
                if (mxIsChar(mx)) {
                    std::string want = getStdString(mx);
                    PVStringArrayPtr ch = vs->getSubField<PVStringArray>("choices");
                    if (ch && idx) {
                        shared_vector<const std::string> v = ch->view();
                        for (size_t i = 0; i < v.size(); ++i)
                            if (v[i] == want) { idx->put((int32)i); return "value"; }
                    }
                    err.err = PVA_TYPEMISMATCH;
                    err.msg = "enum string not among choices";
                    return "";
                }
                if (idx) {
                    if ((mxIsNumeric(mx) || mxIsLogical(mx)) &&
                        mxGetNumberOfElements(mx) == 0) {
                        err.err = PVA_TYPEMISMATCH;
                        err.msg = "cannot write an empty value to an enum PV";
                        return "";
                    }
                    int32 i = (int32)mxGetScalar(mx);
                    PVStringArrayPtr ch = vs->getSubField<PVStringArray>("choices");
                    size_t nch = ch ? ch->view().size() : 0;
                    if (nch > 0 && (i < 0 || (size_t)i >= nch)) {
                        err.err = PVA_INVALIDARG;
                        err.msg = "enum index out of range";
                        return "";
                    }
                    idx->put(i);
                    return "value";
                }
            }
        }
        mxToPvField(mx, value, err);
        return err.err == PVA_OK ? "value" : "";
    }

    /* No "value" field: if the structure has exactly one field, write it. */
    const PVFieldPtrArray &fields = pv->getPVFields();
    if (fields.size() == 1) {
        mxToPvField(mx, fields[0], err);
        return err.err == PVA_OK ? fields[0]->getFieldName() : "";
    }

    err.err = PVA_NOFIELD;
    err.msg = "structure has no 'value' field; use pvaPutStructure";
    return "";
}

} // namespace labpva
