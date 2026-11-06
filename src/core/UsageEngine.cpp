#include "UsageEngine.h"

#include <QDateTime>
#include <QMap>

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

bool covers(const QList<Span> &spans, qint64 a, qint64 b)
{
    for (const Span &s : spans)
        if (s.a <= a && b <= s.b)
            return true;
    return false;
}

QString sessionKey(const QString &serverId, int sessionId)
{
    return serverId + QLatin1Char('|') + QString::number(sessionId);
}

// Samples that saw the session connected within [start, end].
QList<const Sample *> connectedWithin(const QList<Sample> &samples, qint64 start, qint64 end)
{
    QList<const Sample *> out;
    for (const Sample &s : samples)
        if (s.state == Sample::Connected && s.ms >= start - kSlackMs && s.ms <= end + kSlackMs)
            out << &s;
    return out;
}

// Splits [start, end] into active, away and unknown parts (rules in the header).
QList<Part> classify(qint64 start, qint64 end, const QList<const Sample *> &seen)
{
    QList<Part> parts;
    if (end <= start)
        return parts;

    QList<Span> away;
    for (const Sample *s : seen) {
        if (s->idleMin < kAfkMinutes)
            continue;
        // Idle m whole minutes: last input was m to m+1 minutes ago.
        const qint64 from = qMax(start, s->ms - qint64(s->idleMin) * 60000 - qint64(kIdleMiddleSec) * 1000);
        away << Span{from, qMin(s->ms, end)};
    }
    away = mergedSpans(away);

    QList<Span> unknown;
    qint64 previous = start;
    for (const Sample *s : seen) {
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

    for (qsizetype i = 0; i + 1 < cuts.size(); ++i) {
        Part p;
        p.start = cuts.at(i);
        p.end = cuts.at(i + 1);
        p.kind = covers(away, p.start, p.end) ? Kind::Afk : (covers(unknown, p.start, p.end) ? Kind::Unknown : Kind::Active);
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
    QString server, user, address;
};

QString clientOf(const QList<const Sample *> &seen)
{
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

QDate dayOf(qint64 ms, const QTimeZone &zone)
{
    return QDateTime::fromMSecsSinceEpoch(ms, zone).date();
}

qint64 nextDayStart(qint64 ms, const QTimeZone &zone)
{
    return QDateTime(dayOf(ms, zone).addDays(1), QTime(0, 0), zone).toMSecsSinceEpoch();
}

Report compute(const QList<Event> &allEvents, const QList<Sample> &allSamples, qint64 nowMs, const QTimeZone &zone)
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
                    if (e.ms - cur.start <= 2000 && e.address == cur.address)
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
                on = true;
            } else if (on) {
                cur.end = e.ms;
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
                raws << r;
            }
            previousEvent = e.ms;
        }
        if (on) {
            cur.open = true;
            cur.end = nowMs;
            raws << cur;
        }

        // Fit connections to the samples
        QList<Segment> segments;
        for (Raw &r : raws) {
            if (!r.startKnown) {
                // Connected from the first sample after the previous event.
                r.start = r.end;
                for (const Sample &s : std::as_const(sm))
                    if (s.state == Sample::Connected && s.ms > r.previousEvent && s.ms <= r.end + kSlackMs) {
                        r.start = qMin(s.ms, r.end);
                        break;
                    }
            }
            if (!r.endKnown) {
                // No disconnect in the log: connected until the last sample that saw it.
                r.end = r.start;
                for (const Sample &s : std::as_const(sm))
                    if (s.state == Sample::Connected && s.ms >= r.start - kSlackMs && s.ms <= r.endBound)
                        r.end = qMax(r.end, s.ms);
            } else if (r.open) {
                const Sample *last = sm.isEmpty() ? nullptr : &sm.last();
                // Last time quser showed it connected.
                qint64 seenUntil = r.start;
                for (const Sample &s : std::as_const(sm))
                    if (s.state == Sample::Connected && s.ms >= r.start - kSlackMs)
                        seenUntil = qMax(seenUntil, s.ms);
                const bool disconnected = last && last->ms > r.start + kSlackMs && last->state != Sample::Connected;
                const bool absent = serverBeat.value(serverId) > seenUntil + kAbsentMs;
                if (disconnected || absent) {
                    // quser says it is over or no longer lists it, the log has no end.
                    r.open = false;
                    r.endKnown = false;
                    r.end = seenUntil;
                    if (disconnected && nowMs - last->ms <= kRecentMs)
                        report.mismatches << Mismatch{serverId, serverNames.value(serverId), sessionId, false, last->ms};
                } else {
                    r.end = qMax(r.start, nowMs);
                }
            }
            r.end = qMax(r.end, r.start);

            Segment seg;
            seg.start = r.start;
            seg.end = r.end;
            seg.startKnown = r.startKnown;
            seg.endKnown = r.endKnown;
            seg.open = r.open;
            const QList<const Sample *> seen = connectedWithin(sm, seg.start, seg.end);
            seg.client = clientOf(seen);
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

        // Connections only quser knows about
        QList<Span> known;
        for (const Visit &v : std::as_const(report.visits))
            if (v.serverId == serverId && v.sessionId == sessionId)
                known << Span{v.start() - kSlackMs, v.end() + kSlackMs};
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
            // Open if it is the newest sample and recent.
            seg.open = last.ms == sm.last().ms && nowMs - last.ms <= kRecentMs;
            const QList<const Sample *> seen = connectedWithin(sm, seg.start, seg.end);
            seg.client = clientOf(seen);
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
        for (const Sample &s : std::as_const(sm)) {
            bool inLog = false;
            for (const Span &k : std::as_const(known))
                inLog |= s.ms >= k.a && s.ms <= k.b;
            if (s.state != Sample::Connected || inLog) {
                flush();
                continue;
            }
            if (grouping && s.ms - last.ms > kSampleGapMs)
                flush();
            if (!grouping) {
                first = s;
                grouping = true;
            }
            last = s;
        }
        flush();

        // Newest sample says connected but the log has no connection.
        if (!sm.isEmpty() && nowMs - sm.last().ms <= kRecentMs && sm.last().state == Sample::Connected) {
            bool inLog = false;
            for (const Visit &v : std::as_const(report.visits))
                if (v.serverId == serverId && v.sessionId == sessionId && !v.inferred())
                    inLog |= sm.last().ms >= v.start() - kSlackMs && sm.last().ms <= v.end() + kSlackMs;
            if (!inLog)
                report.mismatches << Mismatch{serverId, serverNames.value(serverId), sessionId, true, sm.last().ms};
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
                    && p.address == v.address && sameComputer(p.client, v.client)) {
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

    // Split by local day
    QMap<QString, DayGroup> groups;
    for (int vi = 0; vi < report.visits.size(); ++vi) {
        const Visit &v = report.visits.at(vi);
        QMap<QDate, DayVisit> slices;
        auto sliceOf = [&](const QDate &day) -> DayVisit & {
            DayVisit &d = slices[day];
            if (d.parts.isEmpty() && d.start == 0) {
                d.visit = vi;
                d.start = qMax(v.start(), QDateTime(day, QTime(0, 0), zone).toMSecsSinceEpoch());
                d.end = d.start;
            }
            return d;
        };
        for (const Segment &seg : v.segments) {
            sliceOf(dayOf(seg.start, zone)); // list zero-length visits too
            for (const Part &p : seg.parts) {
                for (qint64 at = p.start; at < p.end;) {
                    const qint64 cut = qMin(p.end, nextDayStart(at, zone));
                    DayVisit &d = sliceOf(dayOf(at, zone));
                    d.parts << Part{at, cut, p.kind};
                    d.end = qMax(d.end, cut);
                    switch (p.kind) {
                    case Kind::Active:  d.activeMs += cut - at; break;
                    case Kind::Afk:     d.afkMs += cut - at; break;
                    case Kind::Unknown: d.unknownMs += cut - at; break;
                    }
                    at = cut;
                }
            }
        }
        for (auto it = slices.begin(); it != slices.end(); ++it) {
            DayVisit &d = it.value();
            const QDate day = it.key();
            const qint64 dayStart = QDateTime(day, QTime(0, 0), zone).toMSecsSinceEpoch();
            const qint64 dayEnd = nextDayStart(dayStart, zone);
            d.fromPreviousDay = v.start() < dayStart;
            d.intoNextDay = v.end() > dayEnd;
            if (d.start < dayStart)
                d.start = dayStart;

            const QString key = day.toString(Qt::ISODate) + QLatin1Char('|') + v.serverId + QLatin1Char('|')
                                + v.client.toLower() + QLatin1Char('|') + v.address;
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
            if (dayOf(v.start(), zone) == day)
                ++g.entries;
            g.firstIn = qMin(g.firstIn, d.start);
            g.lastOut = qMax(g.lastOut, d.end);
            if (v.open() && !d.intoNextDay)
                g.inside = true;
            g.activeMs += d.activeMs;
            g.afkMs += d.afkMs;
            g.unknownMs += d.unknownMs;
            g.uncertain |= v.inferred() || (!v.startKnown() && !d.fromPreviousDay) || (!v.endKnown() && !d.intoNextDay);
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

} // namespace UsageEngine
