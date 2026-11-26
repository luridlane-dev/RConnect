#include "UsageEngine.h"

#include <QDateTime>
#include <QMap>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace UsageEngine {

namespace {

constexpr qint64 kMergeGapMs = qint64(kMergeGapSec) * 1000;
constexpr qint64 kSampleGapMs = qint64(kSampleGapSec) * 1000;
// Events and samples use the same server clock; allow a few seconds between them.
constexpr qint64 kSlackMs = 3000;
// Older samples say nothing about "now".
constexpr qint64 kRecentMs = 10 * 60 * 1000;
// A session not seen for this long while the server was checked is gone, whatever the log says.
constexpr qint64 kAbsentMs = 15 * 60 * 1000;

struct Span {
    qint64 a = 0, b = 0;
};

QList<Span> mergedSpans(QList<Span> spans)
{
    std::sort(spans.begin(), spans.end(), [](const Span &x, const Span &y) { return x.a < y.a; });
    QList<Span> out;
    out.reserve(spans.size());
    for (const Span &s : std::as_const(spans)) {
        if (s.b <= s.a)
            continue;
        if (!out.isEmpty() && s.a <= out.last().b)
            out.last().b = qMax(out.last().b, s.b);
        else
            out << s;
    }
    return out;
}

QString sessionKey(const QString &serverId, int sessionId)
{
    return serverId + QLatin1Char('|') + QString::number(sessionId);
}

// Samples that saw the session connected within [start, end].
bool sameAccount(const QString &first, const QString &second)
{
    return first.isEmpty() || second.isEmpty() || accountOf(first).compare(accountOf(second), Qt::CaseInsensitive) == 0;
}

// `samples` is sorted by `ms` ascending; binary search jumps straight to [minMs, maxMs].
QList<const Sample *> connectedWithin(const QList<Sample> &samples, qint64 minMs, qint64 maxMs, const QString &user)
{
    QList<const Sample *> out;
    if (samples.isEmpty() || maxMs < minMs)
        return out;
    auto it = std::lower_bound(samples.cbegin(), samples.cend(), minMs,
                               [](const Sample &s, qint64 t) { return s.ms < t; });
    for (; it != samples.cend() && it->ms <= maxMs; ++it)
        if (it->state == Sample::Connected && sameAccount(user, it->user))
            out << &(*it);
    return out;
}

// Splits [start, end] into active, away and unknown parts (rules in the header).
// Consecutive idle readings from the same stretch intersect their [t-(m+1)min, t-m*min]
// windows so last-input time is pinned much tighter than whole-minute truncation.
QList<Part> classify(qint64 start, qint64 end, const QList<const Sample *> &seen)
{
    QList<Part> parts;
    if (end <= start)
        return parts;
    if (std::none_of(seen.cbegin(), seen.cend(), [](const Sample *sample) { return sample->idleMin >= 0; }))
        return {{start, end, Kind::Unknown}};

    constexpr qint64 kMinuteMs = 60000;
    constexpr qint64 kJitterMs = 3000;

    struct IdleRun {
        qint64 lo = 0;
        qint64 hi = 0;
        qint64 lastSample = 0;
        int maxIdleMin = 0;
        bool open = false;
    };

    QList<Span> away;
    IdleRun run;
    auto flushRun = [&](qint64 endAt) {
        if (!run.open)
            return;
        run.open = false;
        const qint64 lastInput = qBound(start, (run.lo + run.hi) / 2, run.lastSample);
        const qint64 spanEnd = qBound(lastInput, endAt, end);
        if (run.maxIdleMin >= kAfkMinutes && spanEnd > lastInput)
            away << Span{lastInput, spanEnd};
    };

    qint64 lastActiveFloor = start;
    for (const Sample *s : seen) {
        if (s->idleMin < 0)
            continue;
        const qint64 t = s->ms;
        const int m = s->idleMin;
        const qint64 sLo = qMax(start, t - qint64(m + 1) * kMinuteMs);
        const qint64 sHi = qMax(start, t - qint64(m) * kMinuteMs);
        if (m == 0) {
            if (run.open) {
                const bool contiguous = t - run.lastSample <= kSampleGapMs;
                const qint64 resumeAt = contiguous ? (qMax(run.lastSample, t - kMinuteMs) + t) / 2 : run.lastSample;
                flushRun(resumeAt);
            }
            lastActiveFloor = qMax(lastActiveFloor, sLo);
            continue;
        }
        const qint64 clampedLo = qMax(sLo, lastActiveFloor);
        const qint64 clampedHi = qMax(clampedLo, sHi);
        const bool continues = run.open && (t - run.lastSample <= kSampleGapMs)
            && qMax(run.lo, clampedLo) <= qMin(run.hi, clampedHi) + kJitterMs;
        if (continues) {
            const qint64 nextLo = qMax(run.lo, clampedLo);
            const qint64 nextHi = qMin(run.hi, clampedHi);
            if (nextLo <= nextHi) {
                run.lo = nextLo;
                run.hi = nextHi;
            } else {
                run.lo = run.hi = (nextLo + nextHi) / 2;
            }
            run.lastSample = t;
            run.maxIdleMin = qMax(run.maxIdleMin, m);
        } else {
            if (run.open) {
                const bool contiguous = t - run.lastSample <= kSampleGapMs;
                const qint64 resumeAt = contiguous ? qMax(run.lastSample, (clampedLo + clampedHi) / 2) : run.lastSample;
                flushRun(resumeAt);
            }
            run = IdleRun{clampedLo, clampedHi, t, m, true};
        }
    }
    if (run.open) {
        const bool reachesEnd = end - run.lastSample <= kSampleGapMs;
        flushRun(reachesEnd ? end : run.lastSample);
    }
    away = mergedSpans(away);

    QList<Span> unknown;
    qint64 previous = start;
    for (const Sample *s : seen) {
        if (s->idleMin < 0)
            continue;
        const qint64 at = qBound(start, s->ms, end);
        if (at - previous > kSampleGapMs)
            unknown << Span{previous, at};
        previous = at;
    }
    if (end - previous > kSampleGapMs)
        unknown << Span{previous, end};

    QList<qint64> cuts{start, end};
    for (const QList<Span> *list : {&away, &unknown})
        for (const Span &s : *list)
            cuts << qBound(start, s.a, end) << qBound(start, s.b, end);
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

    qsizetype ai = 0;
    qsizetype ui = 0;
    for (qsizetype i = 0; i + 1 < cuts.size(); ++i) {
        Part p;
        p.start = cuts.at(i);
        p.end = cuts.at(i + 1);
        while (ai < away.size() && away.at(ai).b <= p.start)
            ++ai;
        while (ui < unknown.size() && unknown.at(ui).b <= p.start)
            ++ui;
        const bool isAway = ai < away.size() && away.at(ai).a <= p.start && p.end <= away.at(ai).b;
        const bool isUnknown = !isAway && ui < unknown.size() && unknown.at(ui).a <= p.start && p.end <= unknown.at(ui).b;
        p.kind = isAway ? Kind::Afk : (isUnknown ? Kind::Unknown : Kind::Active);
        if (!parts.isEmpty() && parts.last().kind == p.kind && parts.last().end == p.start)
            parts.last().end = p.end;
        else
            parts << p;
    }
    return parts;
}

struct Raw {
    qint64 start = 0, end = 0;
    bool startKnown = true, endKnown = true, open = false;
    qint64 endBound = 0;      // !endKnown: when the next connection began
    qint64 previousEvent = 0; // !startKnown: time of the session's previous event, or 0
    QString server, user, address, client;
};

QString clientOf(const QList<const Sample *> &seen, qint64 start, qint64 end)
{
    // Prefer samples taken strictly inside [start, end] so a slack-window sample from an adjacent
    // connection on the same session ID cannot steal attribution.
    for (qsizetype i = seen.size() - 1; i >= 0; --i) {
        const Sample *s = seen.at(i);
        if (s->ms >= start && s->ms <= end && !s->client.isEmpty())
            return s->client;
    }
    for (qsizetype i = seen.size() - 1; i >= 0; --i)
        if (!seen.at(i)->client.isEmpty())
            return seen.at(i)->client;
    return {};
}

bool sameComputer(const QString &a, const QString &b)
{
    return a.isEmpty() || b.isEmpty() || a.compare(b, Qt::CaseInsensitive) == 0;
}

} // namespace

bool Visit::inferred() const
{
    for (const Segment &s : segments)
        if (s.inferred)
            return true;
    return false;
}

QString accountOf(const QString &user)
{
    return user.mid(user.lastIndexOf(QLatin1Char('\\')) + 1);
}

QString personNameOf(const QString &alias)
{
    const QString trimmed = alias.trimmed();
    if (trimmed.isEmpty())
        return {};
    static const QRegularExpression suffix(QStringLiteral("(?:(?:\\s*[-_]\\s*|\\s+)#?\\d+|\\s*\\(\\d+\\))$"));
    QString base = trimmed;
    base.remove(suffix);
    base = base.trimmed();
    bool hasLetter = false;
    for (QChar ch : std::as_const(base))
        if (ch.isLetter()) {
            hasLetter = true;
            break;
        }
    return base.size() >= 2 && hasLetter ? base : trimmed;
}

QDate dayOf(qint64 ms, const QTimeZone &zone)
{
    return QDateTime::fromMSecsSinceEpoch(ms, zone).date();
}

qint64 nextDayStart(qint64 ms, const QTimeZone &zone)
{
    return QDateTime(dayOf(ms, zone).addDays(1), QTime(0, 0), zone).toMSecsSinceEpoch();
}

Report compute(const QList<Event> &allEvents, const QList<Sample> &allSamples, qint64 nowMs, const QTimeZone &zone,
               const QHash<QString, qint64> &serverNowMs)
{
    Report report;

    // Group by session
    QHash<QString, QList<Event>> events;
    QHash<QString, QList<Sample>> samples;
    QHash<QString, QString> serverNames;
    QHash<QString, qint64> serverBeat; // newest check per server
    for (const Event &e : allEvents) {
        events[sessionKey(e.serverId, e.sessionId)] << e;
        if (!e.server.isEmpty())
            serverNames[e.serverId] = e.server;
    }
    for (const Sample &s : allSamples) {
        if (s.state != Sample::Heartbeat)
            samples[sessionKey(s.serverId, s.sessionId)] << s;
        if (!s.server.isEmpty())
            serverNames[s.serverId] = s.server;
        serverBeat[s.serverId] = qMax(serverBeat.value(s.serverId), s.ms);
    }
    QStringList keys = events.keys() + samples.keys();
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    for (const QString &key : std::as_const(keys)) {
        QList<Event> ev = events.value(key);
        QList<Sample> sm = samples.value(key);
        std::sort(sm.begin(), sm.end(), [](const Sample &x, const Sample &y) { return x.ms < y.ms; });
        // Same time: ends sort before starts.
        std::sort(ev.begin(), ev.end(), [](const Event &x, const Event &y) {
            if (x.ms != y.ms)
                return x.ms < y.ms;
            const bool xStart = x.kind == Event::Logon || x.kind == Event::Reconnect;
            const bool yStart = y.kind == Event::Logon || y.kind == Event::Reconnect;
            if (xStart != yStart)
                return !xStart;
            return x.recordId < y.recordId;
        });

        const QString serverId = !ev.isEmpty() ? ev.first().serverId : sm.first().serverId;
        const int sessionId = !ev.isEmpty() ? ev.first().sessionId : sm.first().sessionId;
        const qint64 serverNow = serverNowMs.value(serverId, nowMs);

        // Connections from events
        QList<Raw> raws;
        bool on = false;
        Raw cur;
        qint64 previousEvent = 0;
        for (qsizetype i = 0; i < ev.size(); ++i) {
            const Event &e = ev.at(i);
            if (i > 0 && e.ms == ev.at(i - 1).ms && e.kind == ev.at(i - 1).kind && e.recordId == ev.at(i - 1).recordId)
                continue; // duplicate
            if (e.kind == Event::Logon || e.kind == Event::Reconnect) {
                if (on) {
                    if (e.ms - cur.start <= 2000 && e.address == cur.address && sameAccount(cur.user, e.user))
                        continue; // sign-in and reconnect of the same connection
                    cur.end = cur.endBound = e.ms;
                    cur.endKnown = false;
                    raws << cur;
                }
                cur = Raw();
                cur.start = e.ms;
                cur.server = e.server;
                cur.user = e.user;
                cur.address = e.address;
                cur.client = e.client;
                on = true;
            } else if (on && sameAccount(cur.user, e.user)) {
                cur.end = e.ms;
                if (cur.client.isEmpty() && !e.client.isEmpty())
                    cur.client = e.client;
                raws << cur;
                on = false;
            } else if (e.kind == Event::Disconnect) {
                // Disconnect with no start in the log.
                Raw r;
                r.end = e.ms;
                r.startKnown = false;
                r.previousEvent = previousEvent;
                r.server = e.server;
                r.user = e.user;
                r.address = e.address;
                r.client = e.client;
                raws << r;
            }
            previousEvent = e.ms;
        }
        if (on) {
            cur.open = true;
            cur.end = serverNow;
            raws << cur;
        }

        // Pass 1: fit connection start/end boundaries to the samples (sm is sorted by ms).
        const auto sampleFrom = [&sm](qint64 minMs) {
            return std::lower_bound(sm.cbegin(), sm.cend(), minMs,
                                    [](const Sample &s, qint64 t) { return s.ms < t; });
        };
        for (Raw &r : raws) {
            if (!r.startKnown) {
                // Connected from the first sample after the previous event.
                r.start = r.end;
                for (auto it = sampleFrom(r.previousEvent + 1); it != sm.cend() && it->ms <= r.end + kSlackMs; ++it)
                    if (it->state == Sample::Connected && sameAccount(r.user, it->user)) {
                        r.start = qMin(it->ms, r.end);
                        break;
                    }
            }
            if (!r.endKnown) {
                // No disconnect in the log: connected until the last sample that saw it.
                r.end = r.start;
                for (auto it = sampleFrom(r.start - kSlackMs); it != sm.cend() && it->ms <= r.endBound; ++it)
                    if (it->state == Sample::Connected && sameAccount(r.user, it->user))
                        r.end = qMax(r.end, it->ms);
            } else if (r.open) {
                const Sample *last = sm.isEmpty() ? nullptr : &sm.last();
                // Last time quser showed it connected.
                qint64 seenUntil = r.start;
                for (auto it = sampleFrom(r.start - kSlackMs); it != sm.cend(); ++it)
                    if (it->state == Sample::Connected && sameAccount(r.user, it->user))
                        seenUntil = qMax(seenUntil, it->ms);
                const bool disconnected = last && last->ms > r.start + kSlackMs
                    && (last->state != Sample::Connected || !sameAccount(r.user, last->user));
                const bool absent = serverBeat.value(serverId) > seenUntil + kAbsentMs;
                r.endKnown = false;
                r.end = seenUntil;
                r.open = !disconnected && !absent && serverNow - seenUntil <= kRecentMs;
                if (disconnected || absent) {
                    // quser says it is over or no longer lists it, the log has no end.
                    if (disconnected && serverNow - last->ms <= kRecentMs)
                        report.mismatches << Mismatch{serverId, serverNames.value(serverId), sessionId, false, last->ms};
                }
            }
            r.end = qMax(r.end, r.start);
        }

        // Pass 2: match samples without crossing adjacent connections on the same session ID.
        const qsizetype firstSessionVisit = report.visits.size();
        for (qsizetype ri = 0; ri < raws.size(); ++ri) {
            const Raw &r = raws.at(ri);
            Segment seg;
            seg.start = r.start;
            seg.end = r.end;
            seg.startKnown = r.startKnown;
            seg.endKnown = r.endKnown;
            seg.open = r.open;
            qint64 minSampleMs = seg.start - kSlackMs;
            if (ri > 0)
                minSampleMs = qMax(minSampleMs, raws.at(ri - 1).end);
            qint64 maxSampleMs = seg.end + kSlackMs;
            if (ri + 1 < raws.size())
                maxSampleMs = qMin(maxSampleMs, raws.at(ri + 1).start);
            const QList<const Sample *> seen = connectedWithin(sm, minSampleMs, maxSampleMs, r.user);
            // Security 4778/4779 records the exact client computer name for this connection; prefer it over
            // asynchronous samples when present.
            seg.client = !r.client.isEmpty() ? r.client : clientOf(seen, seg.start, seg.end);
            seg.parts = classify(seg.start, seg.end, seen);
            if (!r.startKnown && seg.start == seg.end && seen.isEmpty())
                continue; // just a stray disconnect
            Visit v;
            v.serverId = serverId;
            v.server = r.server.isEmpty() ? serverNames.value(serverId) : r.server;
            v.user = accountOf(r.user);
            v.address = r.address;
            v.client = seg.client;
            v.sessionId = sessionId;
            v.segments << seg;
            report.visits << v;
        }
        const qsizetype loggedVisitsEnd = report.visits.size();

        // Connections only quser knows about: merge known spans for this session and sweep with two pointers.
        QList<Span> known;
        known.reserve(loggedVisitsEnd - firstSessionVisit);
        for (qsizetype vi = firstSessionVisit; vi < loggedVisitsEnd; ++vi) {
            const Visit &v = report.visits.at(vi);
            known << Span{v.start() - kSlackMs, v.end() + kSlackMs};
        }
        known = mergedSpans(std::move(known));

        bool grouping = false;
        Sample first, last;
        auto flush = [&] {
            if (!grouping)
                return;
            grouping = false;
            Segment seg;
            seg.start = first.ms;
            seg.end = last.ms;
            seg.startKnown = seg.endKnown = false;
            seg.inferred = true;
            // Open if it is the newest sample, recent, and the server has not moved past it without listing it.
            seg.open = last.ms == sm.last().ms && serverNow - last.ms <= kRecentMs
                && serverBeat.value(serverId) <= last.ms + kAbsentMs;
            const QList<const Sample *> seen = connectedWithin(sm, seg.start - kSlackMs, seg.end + kSlackMs, last.user);
            seg.client = clientOf(seen, seg.start, seg.end);
            seg.parts = classify(seg.start, seg.end, seen);
            Visit v;
            v.serverId = serverId;
            v.server = serverNames.value(serverId);
            v.user = accountOf(last.user);
            v.client = seg.client;
            v.sessionId = sessionId;
            v.segments << seg;
            report.visits << v;
        };
        qsizetype ki = 0;
        for (const Sample &s : std::as_const(sm)) {
            while (ki < known.size() && known.at(ki).b < s.ms)
                ++ki;
            const bool inLog = ki < known.size() && s.ms >= known.at(ki).a;
            if (s.state != Sample::Connected || inLog) {
                flush();
                continue;
            }
            if (grouping && (s.ms - last.ms > kSampleGapMs || !sameAccount(s.user, last.user)))
                flush();
            if (!grouping) {
                first = s;
                grouping = true;
            }
            last = s;
        }
        flush();

        // Newest sample says connected but the log has no connection.
        if (!sm.isEmpty() && serverNow - sm.last().ms <= kRecentMs && sm.last().state == Sample::Connected) {
            const qint64 lastMs = sm.last().ms;
            bool inLog = false;
            for (const Span &k : std::as_const(known)) {
                if (lastMs >= k.a && lastMs <= k.b) {
                    inLog = true;
                    break;
                }
            }
            if (!inLog)
                report.mismatches << Mismatch{serverId, serverNames.value(serverId), sessionId, true, lastMs};
        }
    }

    // A visit with no measurement has only the IP address. When every named visit from that address on that server
    // agrees on one computer name, the unnamed ones are that computer too, so one person is not split in two rows
    // (one with the name, one without). Two different names behind one address (a shared line) fill nothing.
    {
        QHash<QString, QString> nameOf; // "server|address" -> the one name, or empty when they disagree
        for (const Visit &v : std::as_const(report.visits)) {
            if (v.client.isEmpty() || v.address.isEmpty())
                continue;
            const QString key = v.serverId + QLatin1Char('|') + v.address;
            if (!nameOf.contains(key))
                nameOf.insert(key, v.client);
            else if (nameOf.value(key).compare(v.client, Qt::CaseInsensitive) != 0)
                nameOf[key] = QString();
        }
        for (Visit &v : report.visits) {
            if (!v.client.isEmpty() || v.address.isEmpty())
                continue;
            const QString name = nameOf.value(v.serverId + QLatin1Char('|') + v.address);
            if (name.isEmpty())
                continue;
            v.client = name;
            for (Segment &seg : v.segments)
                if (seg.client.isEmpty())
                    seg.client = name;
        }
    }

    // Merge close connections into visits
    {
        std::sort(report.visits.begin(), report.visits.end(), [](const Visit &x, const Visit &y) {
            if (x.serverId != y.serverId)
                return x.serverId < y.serverId;
            if (x.sessionId != y.sessionId)
                return x.sessionId < y.sessionId;
            return x.start() < y.start();
        });
        QList<Visit> visits;
        for (Visit &v : report.visits) {
            if (!visits.isEmpty()) {
                Visit &p = visits.last();
                const Segment &before = p.segments.last();
                const Segment &after = v.segments.first();
                if (p.serverId == v.serverId && p.sessionId == v.sessionId && !before.inferred && !after.inferred
                    && before.endKnown && after.start - before.end <= kMergeGapMs && after.start >= before.end
                    && sameAccount(p.user, v.user) && p.address == v.address && sameComputer(p.client, v.client)) {
                    p.segments << after;
                    if (!v.client.isEmpty())
                        p.client = v.client;
                    continue;
                }
            }
            visits << v;
        }
        report.visits = visits;
    }

    // Split by local day (cache day boundaries to avoid redundant timezone lookups inside inner loops)
    QMap<QString, DayGroup> groups;
    QDate cachedDay;
    qint64 cachedDayStart = 0;
    qint64 cachedDayEnd = 0;
    auto dayBoundsFor = [&](qint64 ms, QDate &outDay, qint64 &outEnd) {
        if (cachedDay.isValid() && ms >= cachedDayStart && ms < cachedDayEnd) {
            outDay = cachedDay;
            outEnd = cachedDayEnd;
            return;
        }
        outDay = dayOf(ms, zone);
        cachedDay = outDay;
        cachedDayStart = QDateTime(outDay, QTime(0, 0), zone).toMSecsSinceEpoch();
        cachedDayEnd = QDateTime(outDay.addDays(1), QTime(0, 0), zone).toMSecsSinceEpoch();
        outEnd = cachedDayEnd;
    };

    for (int vi = 0; vi < report.visits.size(); ++vi) {
        const Visit &v = report.visits.at(vi);
        QDate visitStartDay;
        qint64 unusedEnd = 0;
        dayBoundsFor(v.start(), visitStartDay, unusedEnd);
        QMap<QDate, DayVisit> slices;
        auto sliceOf = [&](const QDate &day, qint64 start) -> DayVisit & {
            DayVisit &d = slices[day];
            if (d.parts.isEmpty() && d.start == 0) {
                d.visit = vi;
                d.start = start;
                d.end = d.start;
            }
            return d;
        };
        for (const Segment &seg : v.segments) {
            QDate segStartDay;
            qint64 segStartDayEnd = 0;
            dayBoundsFor(seg.start, segStartDay, segStartDayEnd);
            DayVisit &starting = sliceOf(segStartDay, seg.start);
            if (seg.open && seg.start == seg.end)
                starting.open = true;
            for (const Part &p : seg.parts) {
                for (qint64 at = p.start; at < p.end;) {
                    QDate day;
                    qint64 dayEnd = 0;
                    dayBoundsFor(at, day, dayEnd);
                    const qint64 cut = qMin(p.end, dayEnd);
                    DayVisit &d = sliceOf(day, at);
                    d.parts << Part{at, cut, p.kind};
                    d.end = qMax(d.end, cut);
                    d.fromPreviousDay |= segStartDay < day;
                    d.intoNextDay |= seg.end > dayEnd;
                    d.open |= seg.open && cut == seg.end && cut < dayEnd;
                    switch (p.kind) {
                    case Kind::Active:  d.activeMs += cut - at; break;
                    case Kind::Afk:     d.afkMs += cut - at; break;
                    case Kind::Unknown: d.unknownMs += cut - at; break;
                    }
                    at = cut;
                }
            }
            if (seg.open) {
                QDate endDay, prevDay;
                dayBoundsFor(seg.end, endDay, unusedEnd);
                dayBoundsFor(seg.end - 1, prevDay, unusedEnd);
                if (endDay > prevDay)
                    sliceOf(endDay, seg.end).open = true;
            }
        }
        const QString keySuffix = QLatin1Char('|') + v.serverId + QLatin1Char('|')
                                + v.client.toLower() + QLatin1Char('|') + v.address;
        for (auto it = slices.begin(); it != slices.end(); ++it) {
            DayVisit &d = it.value();
            const QDate day = it.key();

            const QString key = day.toString(Qt::ISODate) + keySuffix;
            DayGroup &g = groups[key];
            if (g.visits.isEmpty()) {
                g.day = day;
                g.serverId = v.serverId;
                g.server = v.server;
                g.client = v.client;
                g.address = v.address;
                g.firstIn = d.start;
                g.lastOut = d.end;
            }
            if (!v.client.isEmpty())
                g.client = v.client;
            if (!v.user.isEmpty() && !g.users.contains(v.user, Qt::CaseInsensitive))
                g.users << v.user;
            if (visitStartDay == day)
                ++g.entries;
            g.firstIn = qMin(g.firstIn, d.start);
            g.lastOut = qMax(g.lastOut, d.end);
            if (d.open)
                g.inside = true;
            g.activeMs += d.activeMs;
            g.afkMs += d.afkMs;
            g.unknownMs += d.unknownMs;
            g.uncertain |= v.inferred() || (!v.startKnown() && !d.fromPreviousDay)
                           || (!v.endKnown() && !d.intoNextDay && !d.open);
            g.visits << d;
        }
    }
    for (const DayGroup &g : std::as_const(groups))
        report.days << g;
    std::sort(report.days.begin(), report.days.end(), [](const DayGroup &x, const DayGroup &y) {
        if (x.day != y.day)
            return x.day > y.day;
        if (x.server != y.server)
            return x.server.compare(y.server, Qt::CaseInsensitive) < 0;
        if (x.client != y.client)
            return x.client.compare(y.client, Qt::CaseInsensitive) < 0;
        return x.address < y.address;
    });
    for (DayGroup &g : report.days)
        std::sort(g.visits.begin(), g.visits.end(), [](const DayVisit &x, const DayVisit &y) { return x.start < y.start; });
    return report;
}

QList<Part> mergeParts(const QList<Part> &parts)
{
    struct Edge {
        qint64 at = 0;
        int kind = 0;
        int delta = 0;
    };
    QList<Edge> edges;
    edges.reserve(parts.size() * 2);
    for (const Part &p : parts) {
        if (p.end <= p.start)
            continue;
        edges << Edge{p.start, int(p.kind), 1} << Edge{p.end, int(p.kind), -1};
    }
    std::sort(edges.begin(), edges.end(), [](const Edge &x, const Edge &y) { return x.at < y.at; });

    QList<Part> merged;
    int open[3] = {};
    qint64 previous = 0;
    for (const Edge &e : std::as_const(edges)) {
        if (e.at > previous) {
            Kind kind;
            bool has = true;
            if (open[int(Kind::Active)] > 0)
                kind = Kind::Active;
            else if (open[int(Kind::Unknown)] > 0)
                kind = Kind::Unknown;
            else if (open[int(Kind::Afk)] > 0)
                kind = Kind::Afk;
            else
                has = false;
            if (has) {
                if (!merged.isEmpty() && merged.last().kind == kind && merged.last().end == previous)
                    merged.last().end = e.at;
                else
                    merged << Part{previous, e.at, kind};
            }
        }
        open[e.kind] += e.delta;
        previous = e.at;
    }
    return merged;
}

Time unionOf(const QList<Part> &parts)
{
    Time time;
    for (const Part &p : mergeParts(parts)) {
        const qint64 span = p.end - p.start;
        switch (p.kind) {
        case Kind::Active:  time.activeMs += span; break;
        case Kind::Unknown: time.unknownMs += span; break;
        case Kind::Afk:     time.afkMs += span; break;
        }
    }
    return time;
}

QList<Person> people(const Report &report, const QDate &from, const QDate &to, const WhoOf &who, Breakdown by)
{
    struct Acc {
        Person person;
        QList<Part> parts;
        QList<QList<Part>> shareParts;
        QHash<QString, int> shareIndex;
        QSet<QDate> days;
        qint64 summedTotalMs = 0;
    };
    auto note = [](QStringList &list, const QString &text) {
        if (!text.isEmpty() && !list.contains(text, Qt::CaseInsensitive))
            list << text;
    };

    QHash<QString, int> index;
    QList<Acc> accs;
    for (const DayGroup &g : report.days) {
        if (g.day < from || g.day > to)
            continue;
        for (const DayVisit &dv : g.visits) {
            const Visit &v = report.visits.at(dv.visit);
            const Who w = who(v);
            int pi = index.value(w.key, -1);
            if (pi < 0) {
                pi = int(accs.size());
                index.insert(w.key, pi);
                accs << Acc();
                accs.last().person.key = w.key;
                accs.last().person.name = w.name;
            }
            Acc &a = accs[pi];
            Person &p = a.person;

            Entry e;
            e.visit = dv.visit;
            e.day = g.day;
            e.slice = dv;
            e.uncertain = v.inferred() || (!v.startKnown() && !dv.fromPreviousDay)
                          || (!v.endKnown() && !dv.intoNextDay && !dv.open);

            const QString shareKey = by == Breakdown::ByServer ? v.serverId : g.day.toString(Qt::ISODate);
            int si = a.shareIndex.value(shareKey, -1);
            if (si < 0) {
                si = int(p.shares.size());
                a.shareIndex.insert(shareKey, si);
                Share s;
                if (by == Breakdown::ByServer) {
                    s.serverId = v.serverId;
                    s.server = v.server;
                } else {
                    s.day = g.day;
                }
                p.shares << s;
                a.shareParts << QList<Part>();
            }
            Share &s = p.shares[si];
            s.entries << e;
            s.lastEnd = qMax(s.lastEnd, dv.end);
            s.open |= dv.open;
            s.uncertain |= e.uncertain;
            a.shareParts[si] += dv.parts;

            a.parts += dv.parts;
            a.summedTotalMs += dv.totalMs();
            if (!dv.parts.isEmpty() || dv.open)
                a.days.insert(g.day);
            p.lastEnd = qMax(p.lastEnd, dv.end);
            p.open |= dv.open;
            p.uncertain |= e.uncertain;
            note(p.computers, v.client);
            note(p.addresses, v.address);
            note(p.accounts, v.user);
            note(p.servers, v.server);
        }
    }

    QList<Person> out;
    out.reserve(accs.size());
    for (Acc &a : accs) {
        Person &p = a.person;
        p.time = unionOf(a.parts);
        p.overlapMs = qMax<qint64>(0, a.summedTotalMs - p.time.totalMs());
        p.days = int(a.days.size());
        for (qsizetype i = 0; i < p.shares.size(); ++i) {
            Share &s = p.shares[i];
            s.timeline = mergeParts(a.shareParts.at(i));
            s.time = {};
            for (const Part &part : std::as_const(s.timeline)) {
                const qint64 span = part.end - part.start;
                switch (part.kind) {
                case Kind::Active:  s.time.activeMs += span; break;
                case Kind::Unknown: s.time.unknownMs += span; break;
                case Kind::Afk:     s.time.afkMs += span; break;
                }
            }
            std::sort(s.entries.begin(), s.entries.end(), [](const Entry &x, const Entry &y) {
                return x.slice.start < y.slice.start;
            });
        }
        std::sort(p.shares.begin(), p.shares.end(), [by](const Share &x, const Share &y) {
            if (by == Breakdown::ByDay)
                return x.day > y.day;
            if (x.time.netMs() != y.time.netMs())
                return x.time.netMs() > y.time.netMs();
            if (x.time.totalMs() != y.time.totalMs())
                return x.time.totalMs() > y.time.totalMs();
            return x.server.compare(y.server, Qt::CaseInsensitive) < 0;
        });
        out << p;
    }
    std::sort(out.begin(), out.end(), [](const Person &x, const Person &y) {
        if (x.time.netMs() != y.time.netMs())
            return x.time.netMs() > y.time.netMs();
        if (x.time.totalMs() != y.time.totalMs())
            return x.time.totalMs() > y.time.totalMs();
        return x.name.compare(y.name, Qt::CaseInsensitive) < 0;
    });
    return out;
}

} // namespace UsageEngine
