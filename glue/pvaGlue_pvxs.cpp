/* pvaGlue_pvxs.cpp - see pvaGlue.h. PVXS-backend implementation.
 *
 * Same interface as pvaGlue_pvac.cpp (the classic pvaClient backend), built on
 * the PVXS client library instead: one process-wide pvxs::client::Context
 * created from the EPICS_PVA_* environment, a channel registry of
 * client::Connect handles (backs pvaChannels / pvaIsConnected), monitors via
 * client::Subscription with the same drain-keep-latest cache semantics, and
 * puts through a per-channel cached put Operation (see the write section: the
 * argument Value is built on the MATLAB thread -- pvaConvert.h -- so no
 * callback ever touches MATLAB memory).
 *
 * NOTE: PVXS implements pvAccess only -- there is no Channel Access provider,
 * so pvaSetProvider('ca') is refused (use labca for record.FIELD access).
 */

/* Enables pvxs's "expert" client API (Operation::reExecPut, PutBuilder's
 * autoExec/onInit) -- the put cache in the write section below needs it. Must
 * precede every pvxs header, since pvxs/version.h latches the gate on its
 * first inclusion. Where the API is missing the write path still compiles and
 * falls back to one operation per put (see LABPVA_PVXS_CACHED_PUT). */
#define PVXS_ENABLE_EXPERT_API

#include "pvaGlue.h"

#include <pvxs/client.h>
#include <pvxs/log.h>

#include <epicsEvent.h>
#include <epicsTime.h>

#include <cctype>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

using namespace pvxs;

namespace labpva {

void backendGuardPvxs() {}   /* link guard -- see pvaGlue.h */

/* ---- configuration state -------------------------------------------- */

static double g_timeout = 5.0;
static bool   g_debug   = false;

bool pvaSetProvider(const std::string &p)
{
    /* Accept only the pvAccess token; refusing 'ca' here lets the (backend-
     * neutral) MEX raise a clear error instead of silently misbehaving. */
    return p.empty() || p == "pva";
}

std::string pvaGetProvider() { return "pva"; }

void   pvaSetTimeout(double s) { if (s > 0) g_timeout = s; }
double pvaGetTimeout()         { return g_timeout; }

void pvaSetDebug(bool on)
{
    g_debug = on;
    logger_level_set("pvxs.*", on ? Level::Debug : Level::Err);
}
bool pvaGetDebug() { return g_debug; }

/* ---- the single client context --------------------------------------- */

static client::Context &context()
{
    /* Created lazily from the EPICS_PVA_* environment on first use; lives for
     * the whole MATLAB session (the MEX are mexLock'd, so the library -- and
     * the context's worker threads -- stay mapped until process exit). */
    static bool once = false;
    if (!once) { logger_config_env(); once = true; }
    static client::Context ctxt(client::Context::fromEnv());
    return ctxt;
}

/* ---- pvRequest handling ----------------------------------------------- */

static std::string stripWs(const std::string &s)
{
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
        if (!std::isspace((unsigned char)s[i])) o.push_back(s[i]);
    return o;
}

/* Apply labpva's request convention to a pvxs builder. PVXS's pvRequest()
 * string parser does NOT treat "field(a,b,c)" as a field list (its documented
 * form is repeated field() clauses), and silently falls back to the whole
 * structure -- verified live. So translate the common "field(a,b,...)" shape
 * into the builder's .field() calls; anything more exotic is passed through
 * pvRequest() verbatim. Empty / "field()" means the whole structure. */
template<class Builder>
static void applyRequest(Builder &b, const std::string &request)
{
    std::string r = stripWs(request);
    if (r.empty() || r == "field()" || r == "field") return;
    if (r.compare(0, 6, "field(") == 0 && r[r.size() - 1] == ')' &&
        r.find('(', 6) == std::string::npos) {
        std::string list = r.substr(6, r.size() - 7);
        size_t pos = 0;
        while (pos < list.size()) {
            size_t c = list.find(',', pos);
            if (c == std::string::npos) c = list.size();
            if (c > pos) b.field(list.substr(pos, c - pos));
            pos = c + 1;
        }
    } else {
        b.pvRequest(request);              /* general pvRequest expression */
    }
}

/* Does a monitor subscribed with `monReq` carry every field a get for `getReq`
 * asks for? A whole-structure monitor (`field()` / empty) covers any request;
 * otherwise we only trust an exact (whitespace-insensitive) request match. */
static bool monitorCovers(const std::string &monReq, const std::string &getReq)
{
    std::string m = stripWs(monReq);
    if (m.empty() || m == "field()" || m == "field") return true;
    return m == stripWs(getReq);
}

/* ---- channel registry ------------------------------------------------ */

/* Every channel labpva touches is recorded as a client::Connect handle so
 * pvaChannels can list it and pvaIsConnected can report live state. (PVXS
 * also caches channels inside the context; Connect additionally pins the
 * channel open for the session, matching the pvac backend's behaviour.) */
static std::map<std::string, std::shared_ptr<client::Connect> > g_channels;

static void recordChannel(const std::string &name)
{
    if (g_channels.find(name) == g_channels.end())
        g_channels[name] = context().connect(name).exec();
}

std::vector<std::string> pvaChannelNames()
{
    std::vector<std::string> out;
    out.reserve(g_channels.size());
    for (std::map<std::string, std::shared_ptr<client::Connect> >::const_iterator
             it = g_channels.begin(); it != g_channels.end(); ++it)
        out.push_back(it->first);
    return out;
}

bool pvaChannelConnected(const std::string &name)
{
    std::map<std::string, std::shared_ptr<client::Connect> >::iterator it =
        g_channels.find(name);
    return it != g_channels.end() && it->second && it->second->connected();
}

/* ---- monitor registry ------------------------------------------------ */

struct MonEntry {
    std::shared_ptr<client::Subscription> sub;
    std::shared_ptr<epicsEvent>           evt;      /* signalled by the FIFO
                                                       not-empty callback     */
    PvValue                               latest;   /* most recent polled sample */
    std::string                           request;  /* subscription's pvRequest  */
};

static std::map<std::string, MonEntry> g_monitors;

void pvaMonitorSet(const std::string &name, const std::string &request, PvaError &err)
{
    try {
        /* Tear down any existing monitor on this name before re-subscribing,
         * so we don't leave the previous server-side subscription running. */
        std::map<std::string, MonEntry>::iterator old = g_monitors.find(name);
        if (old != g_monitors.end()) {
            if (old->second.sub) { try { old->second.sub->cancel(); } catch (...) {} }
            g_monitors.erase(old);
        }

        MonEntry e;
        e.request = request;
        e.evt.reset(new epicsEvent());
        std::shared_ptr<epicsEvent> evt(e.evt);

        client::MonitorBuilder b(context().monitor(name));
        applyRequest(b, request);
        /* Mask connection-state events so pop() yields data only; the event
         * callback runs on a PVXS worker thread and must therefore do nothing
         * but signal (it must never touch MATLAB memory). NOTE: subscribing is
         * asynchronous -- a nonexistent PV does not fail here; its poll/wait
         * simply never reports a sample. */
        b.maskConnected(true).maskDisconnected(true);
        b.event([evt](client::Subscription &) { evt->signal(); });
        e.sub = b.exec();

        g_monitors[name] = e;
        recordChannel(name);
    } catch (std::exception &ex) {
        err.err = PVA_NOTCONNECTED;
        err.msg = std::string("pvaSetMonitor '") + name + "': " + ex.what();
        pvaSetLastError(err.err, err.msg);
    }
}

/* Drain the subscription queue keeping the newest sample. */
static bool drainQueue(MonEntry &e)
{
    bool got = false;
    while (true) {
        Value v(e.sub->pop());
        if (!v) break;
        e.latest = v;
        got = true;
    }
    return got;
}

bool pvaMonitorPoll(const std::string &name, PvaError &err)
{
    std::map<std::string, MonEntry>::iterator it = g_monitors.find(name);
    if (it == g_monitors.end()) {
        err.err = PVA_NOMONITOR;
        err.msg = "no monitor set on '" + name + "'";
        return false;
    }
    try {
        return drainQueue(it->second);
    } catch (std::exception &ex) {         /* e.g. RemoteError */
        err.err = PVA_FAILURE;
        err.msg = std::string("pvaNewMonitorValue '") + name + "': " + ex.what();
        pvaSetLastError(err.err, err.msg);
        return false;
    }
}

bool pvaMonitorWait(const std::string &name, double timeout, PvaError &err)
{
    std::map<std::string, MonEntry>::iterator it = g_monitors.find(name);
    if (it == g_monitors.end()) {
        err.err = PVA_NOMONITOR;
        err.msg = "no monitor set on '" + name + "'";
        return false;
    }
    MonEntry &e = it->second;
    double t = timeout > 0 ? timeout : g_timeout;
    try {
        epicsTime deadline = epicsTime::getCurrent() + t;
        while (true) {
            if (drainQueue(e)) return true;
            double remaining = deadline - epicsTime::getCurrent();
            if (remaining <= 0) return false;                 /* timed out */
            if (!e.evt->wait(remaining)) return false;        /* timed out */
        }
    } catch (std::exception &ex) {
        err.err = PVA_FAILURE;
        err.msg = std::string("pvaNewMonitorWait '") + name + "': " + ex.what();
        pvaSetLastError(err.err, err.msg);
        return false;
    }
}

PvValue pvaMonitorLatest(const std::string &name)
{
    std::map<std::string, MonEntry>::iterator it = g_monitors.find(name);
    return it == g_monitors.end() ? PvValue() : it->second.latest;
}

bool pvaMonitorActive(const std::string &name)
{
    return g_monitors.find(name) != g_monitors.end();
}

std::vector<std::string> pvaMonitorNames()
{
    std::vector<std::string> out;
    out.reserve(g_monitors.size());
    for (std::map<std::string, MonEntry>::const_iterator it = g_monitors.begin();
         it != g_monitors.end(); ++it)
        out.push_back(it->first);
    return out;
}

/* ---- write ------------------------------------------------------------ */

/* Two paths, both sending an argument Value built on the MATLAB thread:
 *
 *  - the CACHED path (below): one long-lived put Operation per channel, the
 *    analogue of pvaClient's put cache (PvaClientChannel::put() hands back a
 *    connected PvaClientPut, so a repeated put is a single round trip). A pvxs
 *    Operation is one-shot unless created with .autoExec(false), which leaves
 *    it Idle after each execution to be re-run with reExecPut(). Caching it
 *    pays the operation INIT once per channel instead of once per put, and the
 *    INIT reply is also the server's type description -- which is all a put
 *    argument must be built against, so the pre-put get the MEX layer used to
 *    do (one more operation, two more round trips) disappears as well. A warm
 *    pvaPut therefore costs one round trip, matching the classic backend.
 *
 *  - the ONE-SHOT path (pvaPutExec's tail): a fresh Operation per put, used
 *    when the cached one is unusable -- no INIT yet, a no-wait put still in
 *    flight on that channel, a disconnected channel, or a pvxs too old for the
 *    expert API. Correct, just 2 round trips.
 *
 * Neither path ever touches MATLAB memory from a pvxs worker thread. */

#if defined(PVXS_EXPERT_API_ENABLED) && PVXS_VERSION >= VERSION_INT(1,3,0,0)
#  define LABPVA_PVXS_CACHED_PUT
#endif

/* A pvxs Operation is cancelled when its handle is dropped, so a fire-and-
 * forget put (pvaPutNoWait) must park its handle until completion. The result
 * callback (on a PVXS worker thread) only records the key of a finished put;
 * the handle itself is destroyed later, on the MATLAB thread, by reapDonePuts
 * -- never from inside its own callback. (Only the one-shot path needs this:
 * a cached operation is owned by the registry, so nothing can cancel it.) */
static std::mutex g_putLock;
static std::map<unsigned long long, std::shared_ptr<client::Operation> > g_pendingPuts;
static std::vector<unsigned long long> g_donePuts;
static unsigned long long g_putSeq = 0;

/* Bound on parked no-wait puts: a pvaPutNoWait to a PV that never answers
 * (nonexistent name, IOC down) never completes and would otherwise accumulate
 * live operations for the whole session. Beyond the cap the OLDEST pending put
 * is cancelled and dropped. */
static const size_t MAX_PENDING_PUTS = 256;

static void reapDonePuts()
{
    std::vector<std::shared_ptr<client::Operation> > graveyard;
    {
        std::lock_guard<std::mutex> G(g_putLock);
        std::vector<unsigned long long> keep;
        for (size_t i = 0; i < g_donePuts.size(); ++i) {
            std::map<unsigned long long,
                     std::shared_ptr<client::Operation> >::iterator it =
                g_pendingPuts.find(g_donePuts[i]);
            if (it != g_pendingPuts.end()) {
                graveyard.push_back(it->second);
                g_pendingPuts.erase(it);
            } else {
                /* completion callback fired before the handle was parked --
                 * keep the key for the next sweep */
                keep.push_back(g_donePuts[i]);
            }
        }
        g_donePuts.swap(keep);
    }
    /* graveyard destructs here, outside the lock, on the MATLAB thread; the
     * operations are already complete so destruction cancels nothing */
}

#ifdef LABPVA_PVXS_CACHED_PUT

/* State shared with the PVXS worker threads that run a cached operation's
 * onInit and put-result callbacks. Held by shared_ptr so a callback that fires
 * after the entry has been dropped still has somewhere safe to write. Those
 * callbacks only take this lock, copy a Value / an error string, and signal --
 * no MATLAB memory, no blocking, nothing destroyed. */
struct PutSlot {
    std::mutex   lock;
    epicsEvent   initEvt;   /* signalled by onInit                            */
    epicsEvent   doneEvt;   /* signalled when an issued put completes          */
    Value        proto;     /* server's type description, from the INIT reply  */
    unsigned long gen;      /* bumped on every (re)INIT -- see argGen below    */
    bool         busy;      /* a reExecPut is in flight                        */
    bool         invalid;   /* the operation must be rebuilt before reuse     */
    bool         failed;    /* it reported an error ...                        */
    std::string  msg;       /* ... with this message                           */
    PutSlot() : gen(0), busy(false), invalid(false), failed(false) {}
};

struct PutEntry {
    std::shared_ptr<client::Operation> op;
    std::shared_ptr<PutSlot>           slot;
    /* What the MEX layer builds its argument against: `slot->proto` as-is,
     * except for an enum channel, where it is a value-carrying fetch (the
     * choice list is needed to map a choice string to an index, and a type
     * description does not carry it). Derived on the MATLAB thread and tagged
     * with the generation it came from, so an IOC restart that re-INITs with a
     * different type rebuilds it instead of writing against a stale layout. */
    Value         argProto;
    unsigned long argGen;
    PutEntry() : argGen(0) {}
};

static std::map<std::string, PutEntry> g_putOps;

/* Forget a channel's cached operation; the next put rebuilds it. */
static void dropPutEntry(const std::string &name)
{
    std::map<std::string, PutEntry>::iterator it = g_putOps.find(name);
    if (it == g_putOps.end()) return;
    std::shared_ptr<client::Operation> op(it->second.op);
    g_putOps.erase(it);
    /* op destructs here, on the MATLAB thread, cancelling the operation */
}

static bool valueIsEnum(const Value &v)
{
    if (!v || v.type().code != TypeCode::Struct) return false;
    try { return v.id() == "enum_t"; } catch (std::exception &) { return false; }
}

/* Read the channel's current value through the cached put operation itself
 * (reExecGet on a Put issues the pvAccess put-get subcommand, exactly what
 * pvaClient's put does when it connects). Using the put's own operation
 * guarantees the returned Value has the layout reExecPut will send. Blocks on
 * the MATLAB thread; an invalid Value means "unavailable". */
static Value putOpGet(PutEntry &e, double timeout)
{
    std::shared_ptr<epicsEvent> evt(new epicsEvent());
    std::shared_ptr<Value>      out(new Value());
    std::shared_ptr<PutSlot>    slot(e.slot);
    try {
        e.op->reExecGet([evt, out, slot](client::Result &&r) {
            try { *out = r(); }
            catch (std::exception &) {
                std::lock_guard<std::mutex> G(slot->lock);
                slot->invalid = true;
            }
            evt->signal();
        });
    } catch (std::exception &) {
        std::lock_guard<std::mutex> G(slot->lock);
        slot->invalid = true;
        return Value();
    }
    if (!evt->wait(timeout)) {
        std::lock_guard<std::mutex> G(slot->lock);
        slot->invalid = true;
        return Value();                    /* also covers "was not Idle" */
    }
    if (!*out) {
        std::lock_guard<std::mutex> G(slot->lock);
        slot->invalid = true;
    }
    return *out;
}

/* The cached put operation for `name` with its INIT complete, creating it on
 * first use. NULL means "use the one-shot path" -- the channel did not answer
 * in time, or an enum's choice list could not be read. */
static PutEntry *readyPutEntry(const std::string &name)
{
    std::map<std::string, PutEntry>::iterator it = g_putOps.find(name);
    if (it != g_putOps.end()) {
        bool invalid;
        {
            std::lock_guard<std::mutex> G(it->second.slot->lock);
            invalid = it->second.slot->invalid;
        }
        if (invalid) {
            dropPutEntry(name);
            it = g_putOps.end();
        }
    }
    if (it == g_putOps.end()) {
        PutEntry e;
        e.slot.reset(new PutSlot());
        std::shared_ptr<PutSlot> slot(e.slot);
        try {
            e.op = context().put(name)
                       .fetchPresent(false)   /* the argument is already built */
                       .autoExec(false)       /* we drive it with reExecPut    */
                       .onInit([slot](const Value &proto) {
                            std::lock_guard<std::mutex> G(slot->lock);
                            slot->proto = proto;
                            ++slot->gen;
                            slot->initEvt.signal();
                        })
                       /* never called while autoExec is false (reExecPut
                        * installs its own), but PutBuilder wants a builder */
                       .build([](Value &&v) -> Value { return v; })
                       .exec();
        } catch (std::exception &) { return NULL; }
        it = g_putOps.insert(std::make_pair(name, e)).first;
    }
    PutEntry &e = it->second;

    /* Wait out the INIT round trip -- the one this scheme pays per channel
     * (and again after a reconnect). */
    Value         proto;
    unsigned long gen = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        {
            std::lock_guard<std::mutex> G(e.slot->lock);
            proto = e.slot->proto;
            gen   = e.slot->gen;
        }
        if (proto) break;
        if (!e.slot->initEvt.wait(g_timeout)) break;
    }
    if (!proto) return NULL;

    if (!e.argProto || e.argGen != gen) {
        Value ap(proto);
        if (valueIsEnum(proto["value"])) {
            ap = putOpGet(e, g_timeout);       /* once per channel: the choices */
            if (!ap) return NULL;
        }
        e.argProto = ap;
        e.argGen   = gen;
    }
    return &e;
}

/* Send `arg` through the cached operation. False means "not usable now" and the
 * caller should take the one-shot path; err is set only on a real failure. */
static bool cachedPutTry(const std::string &name, const Value &arg, bool wait,
                         PvaError &err)
{
    std::map<std::string, PutEntry>::iterator it = g_putOps.find(name);
    if (it == g_putOps.end()) return false;
    std::shared_ptr<PutSlot> slot(it->second.slot);
    {
        std::lock_guard<std::mutex> G(slot->lock);
        if (!slot->proto || slot->busy || slot->invalid) return false;
        slot->busy   = true;                   /* only we clear it, in the cb */
        slot->failed = false;
        slot->msg.clear();
    }
    /* pvxs silently ignores a reExec on an operation that is not Idle, which is
     * what a channel in the middle of a reconnect looks like. Checking first
     * keeps that case on the one-shot path (which waits for the connection)
     * instead of burning a timeout here. */
    if (!pvaChannelConnected(name)) {
        {
            std::lock_guard<std::mutex> G(slot->lock);
            slot->invalid = true;
            slot->busy = false;
        }
        dropPutEntry(name);
        return false;
    }
    try {
        it->second.op->reExecPut(arg, [slot](client::Result &&r) {
            std::lock_guard<std::mutex> G(slot->lock);
            try { r(); }
            catch (std::exception &ex) {
                slot->invalid = true;
                slot->failed = true;
                slot->msg = ex.what();
            }
            slot->busy = false;
            slot->doneEvt.signal();
        });
    } catch (std::exception &ex) {
        std::lock_guard<std::mutex> G(slot->lock);
        slot->busy = false;
        slot->invalid = true;
        err.err = PVA_FAILURE;
        err.msg = std::string("pvaPut '") + name + "': " + ex.what();
        pvaSetLastError(err.err, err.msg);
        return true;
    }
    if (!wait) return true;   /* fire and forget; busy clears on completion */

    /* Wait for OUR put: `busy` is the predicate (nothing else can have issued
     * one while it was set), so a leftover doneEvt signal from an earlier
     * no-wait put on this channel just costs a harmless extra loop. */
    bool timedOut = false;
    epicsTime deadline = epicsTime::getCurrent() + g_timeout;
    for (;;) {
        {
            std::lock_guard<std::mutex> G(slot->lock);
            if (!slot->busy) break;
        }
        double remaining = deadline - epicsTime::getCurrent();
        if (remaining <= 0) { timedOut = true; break; }
        slot->doneEvt.wait(remaining);
    }
    if (timedOut) {
        /* The put did not happen: report it, and drop the operation so the next
         * call rebuilds rather than inheriting whatever went wrong. */
        {
            std::lock_guard<std::mutex> G(slot->lock);
            slot->invalid = true;
        }
        dropPutEntry(name);
        err.err = PVA_FAILURE;
        err.msg = std::string("pvaPut '") + name + "': timeout";
        pvaSetLastError(err.err, err.msg);
        return true;
    }
    bool        failed;
    std::string msg;
    {
        std::lock_guard<std::mutex> G(slot->lock);
        failed = slot->failed;
        msg    = slot->msg;
    }
    if (failed) {
        err.err = PVA_FAILURE;
        err.msg = std::string("pvaPut '") + name + "': " + msg;
        pvaSetLastError(err.err, err.msg);
    }
    return true;
}

#endif /* LABPVA_PVXS_CACHED_PUT */

PvValue pvaPutPrototype(const std::string &name, PvaError &err)
{
    reapDonePuts();
#ifdef LABPVA_PVXS_CACHED_PUT
    PutEntry *e = readyPutEntry(name);
    if (e) {
        recordChannel(name);
        return e->argProto;
    }
#endif
    /* No cached operation (channel silent, or a pvxs without the expert API):
     * fall back to a fresh whole-structure get for the type. pvaPutExec then
     * also falls back to a one-shot put, whose pvRequest is likewise the whole
     * structure, so the argument's layout still matches what is sent. */
    PvValue v = pvaGet(name, "field()", err);
    if (err.err != PVA_OK) {
        /* The caller is putting, not getting: report it as a put failure (the
         * classic backend's pvaPutPrepare says "pvaPut '<name>': ..." too). */
        const std::string tag("pvaGet '");
        if (err.msg.compare(0, tag.size(), tag) == 0)
            err.msg = "pvaPut '" + err.msg.substr(tag.size());
        pvaSetLastError(err.err, err.msg);
    }
    return v;
}

void pvaPutExec(const std::string &name, const PvValue &arg, bool wait, PvaError &err)
{
    reapDonePuts();
#ifdef LABPVA_PVXS_CACHED_PUT
    if (cachedPutTry(name, arg, wait, err)) return;
#endif
    try {
        PvValue argCopy(arg);
        /* fetchPresent(false): the argument was pre-built on the MATLAB thread
         * against the channel's type (pvaPutPrototype + pvaConvert.h
         * mxToPutArg*), so no server fetch is needed and the build callback --
         * which runs on a PVXS worker thread -- only hands back the captured
         * Value (it never touches MATLAB memory). Only the MARKED fields of
         * the argument are sent. */
        if (wait) {
            std::shared_ptr<client::Operation> op(
                context().put(name)
                         .fetchPresent(false)
                         .build([argCopy](Value &&) -> Value { return argCopy; })
                         .exec());
            op->wait(g_timeout);           /* throws on error / timeout */
        } else {
            unsigned long long key;
            {
                std::lock_guard<std::mutex> G(g_putLock);
                key = ++g_putSeq;
            }
            std::shared_ptr<client::Operation> op(
                context().put(name)
                         .fetchPresent(false)
                         .build([argCopy](Value &&) -> Value { return argCopy; })
                         .result([key](client::Result &&) {
                             std::lock_guard<std::mutex> G(g_putLock);
                             g_donePuts.push_back(key);
                         })
                         .exec());
            std::shared_ptr<client::Operation> evicted;
            {
                std::lock_guard<std::mutex> G(g_putLock);
                if (g_pendingPuts.size() >= MAX_PENDING_PUTS) {
                    evicted = g_pendingPuts.begin()->second;
                    g_pendingPuts.erase(g_pendingPuts.begin());
                }
                g_pendingPuts[key] = op;
            }
            /* evicted (if any) destructs here, outside the lock, on the MATLAB
             * thread -- cancelling the stalest never-completed put */
        }
        recordChannel(name);
    } catch (std::exception &e) {
        err.err = PVA_FAILURE;
        err.msg = std::string("pvaPut '") + name + "': " + e.what();
        pvaSetLastError(err.err, err.msg);
    }
}

/* ---- clear ------------------------------------------------------------ */

void pvaClear(const std::string &name)
{
    reapDonePuts();
    if (name.empty()) {
        for (std::map<std::string, MonEntry>::iterator it = g_monitors.begin();
             it != g_monitors.end(); ++it) {
            if (it->second.sub) { try { it->second.sub->cancel(); } catch (...) {} }
        }
        g_monitors.clear();
    } else {
        std::map<std::string, MonEntry>::iterator it = g_monitors.find(name);
        if (it != g_monitors.end()) {
            if (it->second.sub) { try { it->second.sub->cancel(); } catch (...) {} }
            g_monitors.erase(it);
        }
    }
}

/* ---- read ------------------------------------------------------------ */

/* Does this cached sample carry a plain (scalar / scalar-array / enum) `value`
 * -- i.e. exactly what smart-pvaGet's bare-value fast path returns? For a rich
 * PV (NTNDArray/NTTable/custom group) or a monitor whose request excluded
 * `value`, this is false and pvaGet must read fresh: serving the cache would
 * return a PARTIAL structure (e.g. an image missing dimension/codec). */
static bool cachedValueIsPlain(const Value &latest)
{
    Value v = latest["value"];
    if (!v) return false;
    TypeCode tc = v.type();
    if (tc.code == TypeCode::Struct) {
        try { return v.id() == "enum_t"; } catch (std::exception &) { return false; }
    }
    return tc.code != TypeCode::StructA && tc.code != TypeCode::Union &&
           tc.code != TypeCode::UnionA  && tc.code != TypeCode::Any &&
           tc.code != TypeCode::AnyA;
}

PvValue pvaGet(const std::string &name, const std::string &request, PvaError &err,
               bool useMonitorCache, bool requireWholeMonitor)
{
    /* Same cache contract as the pvac backend: an active, polled monitor
     * serves the value read (and covered structure reads) with no round-trip.
     * The value verb (requireWholeMonitor=false) additionally requires the
     * cached sample to carry a plain `value` -- otherwise smart-pvaGet would
     * hand back the whole (possibly partial) monitored subset. */
    if (useMonitorCache) {
        std::map<std::string, MonEntry>::iterator it = g_monitors.find(name);
        if (it != g_monitors.end() && it->second.latest &&
            (requireWholeMonitor
                 ? monitorCovers(it->second.request, request)
                 : (monitorCovers(it->second.request, request) ||
                    cachedValueIsPlain(it->second.latest))))
            return it->second.latest;
    }

    try {
        client::GetBuilder b(context().get(name));
        applyRequest(b, request);
        Value v(b.exec()->wait(g_timeout));
        recordChannel(name);
        return v;
    } catch (std::exception &e) {
        err.err = PVA_NOTCONNECTED;
        err.msg = std::string("pvaGet '") + name + "': " + e.what();
        pvaSetLastError(err.err, err.msg);
        return PvValue();
    }
}

} // namespace labpva
