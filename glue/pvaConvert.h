/* pvaConvert.h - PV structure <-> MATLAB mxArray marshalling for labpva
 *
 * This is the heart of labpva and the part with no analogue in labca: a
 * Channel Access PV carries a single typed value (scalar or waveform), but a
 * pvAccess channel carries an arbitrary structure tree (NTScalar wraps the
 * payload in a `.value` field alongside `.alarm`, `.timeStamp`, `.display`,
 * ...; NTTable is a structure-of-columns; NTNDArray is an image; and a server
 * may expose entirely custom structures). These functions translate that tree
 * to and from native MATLAB types.
 *
 * The structure handle crossing this boundary is `labpva::PvValue`
 * (pvaBackend.h): PVStructurePtr on the classic backend, pvxs::Value on the
 * PVXS backend. Each backend implements this header in its own
 * pvaConvert_<backend>.cpp; the MEX entry points see only these declarations.
 *
 * Representation contract (see ARCHITECTURE.md for the full table):
 *   PV scalar (numeric)   -> 1x1 double          (int widths collapse to double,
 *                                                  matching labca's ergonomics)
 *   PV scalar (boolean)   -> 1x1 logical
 *   PV scalar (string)    -> char row vector
 *   PV scalarArray num    -> 1xN double row vector
 *   PV scalarArray string -> 1xN cell of char
 *   PV structure          -> 1x1 struct, one field per PV sub-field (recursive)
 *   PV structureArray     -> 1xN struct array (homogeneous fields)
 *   PV union              -> the stored field, unwrapped (or [] if empty)
 *   PV unionArray         -> 1xN cell array
 *   enum_t sub-structure  -> struct(index, choices{}, choice)  [NT-aware sugar]
 *
 * Field names that are not valid MATLAB identifiers are sanitised (see
 * mxFieldName); the original name is preserved verbatim on write-back via the
 * target structure's own field list, so round-tripping is name-safe.
 */
#ifndef PVA_CONVERT_H
#define PVA_CONVERT_H

#include "mex.h"
#include "pvaError.h"
#include "pvaBackend.h"

namespace labpva {

/* ---- PV -> MATLAB ---------------------------------------------------- */

/* Top-level convenience: convert a whole structure to a 1x1 MATLAB struct. */
mxArray *pvStructureToMx(const PvValue &pv, PvaError &err);

/* Value-only extraction used by pvaGet (the labca-faithful path). Returns the
 * bare `value` for a scalar/scalar-array/enum; anything richer (or value-less)
 * comes back as the whole structure via pvStructureToMx. `typeReq` mirrors
 * labca's type letter ('N','D','C',...): 'C' forces string/cell output, other
 * letters request numeric. Returns a freshly created mxArray. */
mxArray *pvValueToMx(const PvValue &pv, char typeReq, PvaError &err);

/* True when the top-level normative type id is epics:nt/NTTable:*.
 * pvaGetTable uses the id rather than guessing from field names. */
bool pvIsNTTable(const PvValue &pv);

/* Convert an NTTable's value columns to a MATLAB table. Columns follow the
 * NTTable labels order and are returned as column vectors. */
mxArray *pvTableToMx(const PvValue &pv, PvaError &err);

/* Validate that every field in a scalar MATLAB struct names a column in the
 * NTTable value structure. Conversion remains target-type driven. */
void pvValidateTableColumns(const PvValue &pv, const mxArray *columns, PvaError &err);

/* Read the EPICS timestamp into seconds-past-epoch / nanoseconds (both 0 if
 * the structure has no timeStamp field). The MEX layer packs these into the
 * labca-style complex double (sec + i*nsec) via mglue's complexColumn. */
void pvTimeStampSecNsec(const PvValue &pv, double &secOut, double &nsecOut);

/* Marshal the alarm sub-structure to struct(severity,status,message), or an
 * all-zero struct if absent. */
mxArray *pvAlarmToMx(const PvValue &pv);

/* ---- introspection helpers (keep the MEX free of backend types) ------- */

/* Element count of the `value` field: 1 for a scalar/enum (or no value),
 * the array length for a scalar array. (Backs pvaGetNelem.) */
double pvValueNelem(const PvValue &pv);

/* NTEnum choice strings as a 1xK cell of char; an empty 1x0 cell when the
 * channel is not an enum. (Backs pvaGetEnumStrings.) */
mxArray *pvEnumChoicesToMx(const PvValue &pv);

/* The structure's type id, e.g. "epics:nt/NTScalar:1.0" ("" if none), and a
 * printable field-tree dump with current values. (Back pvaInfo.) */
std::string pvTypeId(const PvValue &pv);
std::string pvIntrospect(const PvValue &pv);

/* Read a scalar double sub-field by (possibly dotted) path, e.g.
 * "display.limitLow"; returns `dflt` if the field is absent. */
double getDoubleField(const PvValue &pv, const std::string &path, double dflt);

/* Read a string sub-field by path; returns "" if absent. */
std::string getStringField(const PvValue &pv, const std::string &path);

/* ---- helpers ---------------------------------------------------------- */

/* Sanitise a PV field name into a legal MATLAB struct field identifier. */
std::string mxFieldName(const std::string &pvName);

#ifndef LABPVA_USE_PVXS
/* ---- MATLAB -> PV write path (classic backend; the PVXS equivalent lands
 * with the put phase of the port) ---------------------------------------- */

/* Convert any PVField to MATLAB per the contract above (recursive). */
mxArray *pvFieldToMx(const epics::pvData::PVFieldPtr &field, PvaError &err);

/* Populate an existing target PVField from a MATLAB value, converting element
 * types as needed (the target's introspection is authoritative). */
void mxToPvField(const mxArray *mx, const epics::pvData::PVFieldPtr &target, PvaError &err);

/* Convenience for pvaPut value-only writes: set just the `value` field from a
 * MATLAB scalar/vector/string. Returns the field name actually written. */
std::string mxToPvValue(const mxArray *mx, const PvValue &pv, char typeReq, PvaError &err);

#else
/* ---- MATLAB -> PV write path (PVXS backend) ---------------------------
 * pvxs puts send only the MARKED fields of an argument Value. These build
 * that argument on the MATLAB thread from the channel's type template
 * (`fetched` -- pvaPutProto in pvaGlue.h -- supplies the server's type and,
 * for enums, the choice list): the argument
 * starts as fetched.cloneEmpty() (same type, nothing marked) and assigning
 * from the MATLAB data marks exactly the written fields. The glue then sends
 * it via pvaPutExec (pvaGlue.h). Returns an invalid Value with err set on
 * failure. */

/* pvaPut semantics: write only the `value` field (enum: a choice string or a
 * bounds-checked index; struct value: matched by sanitised field name). */
PvValue mxToPutArg(const mxArray *mx, const PvValue &fetched, char typeReq, PvaError &err);

/* pvaPutStructure semantics: `mx` mirrors the whole structure; only the fields
 * present in it are written (matched by sanitised name, recursive). */
PvValue mxToPutArgStructure(const mxArray *mx, const PvValue &fetched, PvaError &err);
#endif /* !LABPVA_USE_PVXS */

} // namespace labpva

#endif /* PVA_CONVERT_H */
