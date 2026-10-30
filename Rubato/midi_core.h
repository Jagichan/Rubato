// midi_core.h - Platform independent MIDI data model, SMF reader/writer,
// tempo map and note helpers.  No Windows dependencies.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <deque>
#include <algorithm>
#include <cmath>

namespace mc {

// ---------------------------------------------------------------- data model
struct Event {
    uint32_t tick = 0;
    uint8_t  status = 0;          // channel status (0x80..0xEF), 0xF0/0xF7 sysex, 0xFF meta
    uint8_t  d1 = 0, d2 = 0;      // data bytes; for meta events d1 = meta type
    std::vector<uint8_t> data;    // sysex / meta payload

    bool IsMeta()  const { return status == 0xFF; }
    bool IsSysex() const { return status == 0xF0 || status == 0xF7; }
    bool IsChannel() const { return status >= 0x80 && status < 0xF0; }
    bool IsNoteOn()  const { return (status & 0xF0) == 0x90 && d2 > 0; }
    bool IsNoteOff() const { return (status & 0xF0) == 0x80 || ((status & 0xF0) == 0x90 && d2 == 0); }
    int  Channel() const { return IsChannel() ? (status & 0x0F) : -1; }
};

struct Track {
    std::vector<Event> events;
    uint32_t endTick = 0;         // position of End-Of-Track meta event
};

// Phrase recognition settings (set by the Phrase Finder effect, read by the phrase-aware effects below it).
// Lengths are in sixteenth notes, as in the Phrase Finder dialog.
// (The member names keep the "E" suffix of the old eighth-note version.)
struct PhraseCfg {
    double minE = 12, maxE = 20;  // shortest / longest phrase (1/16 notes)
    double pauseE = 4;            // a rest of at least this length (1/16 notes) always ends a phrase
    double sens = 40;             // % : how strongly pitch jumps / velocity drops attract a phrase break
    int    prefer = 3;            // 1 = dynamics, 2 = pitch, 3 = both
};

struct Song {
    int format = 1;
    int ppq = 480;
    double bendRange = 2.0;       // pitch-bend range in semitones (runtime setting, not stored in the file)
    mutable PhraseCfg phrase;     // runtime setting used while a chain is processed
    std::vector<Track> tracks;

    uint32_t EndTick() const {
        uint32_t e = 0;
        for (const Track& t : tracks) {
            e = std::max(e, t.endTick);
            if (!t.events.empty()) e = std::max(e, t.events.back().tick);
            for (const Event& ev : t.events) e = std::max(e, ev.tick);
        }
        return e;
    }
};

// Ordering of simultaneous events: meta, note-off, other, note-on
inline int Priority(const Event& e) {
    if (e.IsMeta())    return 0;
    if (e.IsNoteOff()) return 1;
    if (e.IsNoteOn())  return 3;
    return 2;
}

inline void SortTrack(Track& t) {
    std::stable_sort(t.events.begin(), t.events.end(),
        [](const Event& a, const Event& b) {
            if (a.tick != b.tick) return a.tick < b.tick;
            return Priority(a) < Priority(b);
        });
}

// ------------------------------------------------------------- SMF reading
class ByteReader {
public:
    ByteReader(const uint8_t* d, size_t n) : d_(d), n_(n), p_(0) {}
    bool   Eof()  const { return p_ >= n_; }
    size_t Left() const { return n_ - p_; }
    const uint8_t* Cur() const { return d_ + p_; }
    bool U8(uint8_t& v)   { if (p_ >= n_) return false; v = d_[p_++]; return true; }
    bool U16(uint16_t& v) { uint8_t a, b; if (!U8(a) || !U8(b)) return false; v = (uint16_t)((a << 8) | b); return true; }
    bool U32(uint32_t& v) { uint8_t a, b, c, d; if (!U8(a) || !U8(b) || !U8(c) || !U8(d)) return false;
                            v = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d; return true; }
    bool VLQ(uint32_t& v) {
        v = 0;
        for (int i = 0; i < 4; i++) {
            uint8_t b; if (!U8(b)) return false;
            v = (v << 7) | (b & 0x7F);
            if (!(b & 0x80)) return true;
        }
        return false;
    }
    bool Bytes(std::vector<uint8_t>& out, size_t len) {
        if (len > Left()) return false;
        out.assign(d_ + p_, d_ + p_ + len); p_ += len; return true;
    }
    bool Skip(size_t len) { if (len > Left()) return false; p_ += len; return true; }
private:
    const uint8_t* d_; size_t n_; size_t p_;
};

inline void ParseTrack(const uint8_t* d, size_t n, Track& t) {
    ByteReader r(d, n);
    uint32_t tick = 0;
    uint8_t running = 0;
    bool gotEnd = false;
    while (!r.Eof()) {
        uint32_t delta;
        if (!r.VLQ(delta)) break;
        tick += delta;
        uint8_t b;
        if (!r.U8(b)) break;

        Event e; e.tick = tick;
        if (b == 0xFF) {                              // meta event
            uint8_t type; uint32_t len;
            if (!r.U8(type) || !r.VLQ(len)) break;
            std::vector<uint8_t> data;
            if (!r.Bytes(data, len)) break;
            running = 0;
            if (type == 0x2F) { t.endTick = tick; gotEnd = true; break; }
            e.status = 0xFF; e.d1 = type; e.data = std::move(data);
        } else if (b == 0xF0 || b == 0xF7) {          // sysex
            uint32_t len;
            if (!r.VLQ(len)) break;
            std::vector<uint8_t> data;
            if (!r.Bytes(data, len)) break;
            running = 0;
            e.status = b; e.data = std::move(data);
        } else {                                      // channel / system common
            uint8_t status, d1;
            if (b & 0x80) {
                if (b >= 0xF1) {                      // system common/realtime: skip
                    int skip = (b == 0xF2) ? 2 : ((b == 0xF1 || b == 0xF3) ? 1 : 0);
                    if (!r.Skip(skip)) break;
                    continue;
                }
                status = b; running = b;
                if (!r.U8(d1)) break;
            } else {
                if (!running) break;                  // corrupt data
                status = running; d1 = b;
            }
            int hi = status & 0xF0;
            e.status = status; e.d1 = d1;
            if (hi != 0xC0 && hi != 0xD0) {
                if (!r.U8(e.d2)) break;
            }
        }
        t.events.push_back(std::move(e));
    }
    if (!gotEnd && !t.events.empty()) t.endTick = t.events.back().tick;
}

inline bool ReadSMF(const std::vector<uint8_t>& buf, Song& song, std::wstring& err) {
    song = Song();
    // locate "MThd" (allows for a RIFF RMID wrapper)
    size_t base = std::string::npos;
    size_t lim = buf.size() >= 14 ? std::min<size_t>(buf.size() - 14, 64) : 0;
    for (size_t i = 0; buf.size() >= 14 && i <= lim; i++)
        if (memcmp(buf.data() + i, "MThd", 4) == 0) { base = i; break; }
    if (base == std::string::npos) { err = L"This is not a Standard MIDI File (MThd not found)."; return false; }

    ByteReader r(buf.data() + base, buf.size() - base);
    r.Skip(4);
    uint32_t hlen; uint16_t fmt, ntrk, div;
    if (!r.U32(hlen) || !r.U16(fmt) || !r.U16(ntrk) || !r.U16(div) || hlen < 6) {
        err = L"Corrupt MIDI header."; return false;
    }
    r.Skip(hlen - 6);
    if (div & 0x8000) { err = L"SMPTE time division is not supported."; return false; }
    if (div == 0)     { err = L"Invalid time division (0 ticks per quarter note)."; return false; }
    song.format = fmt;
    song.ppq = div;

    while (r.Left() >= 8) {
        std::vector<uint8_t> id; uint32_t len = 0;
        r.Bytes(id, 4); r.U32(len);
        size_t avail = std::min<size_t>(len, r.Left());
        if (memcmp(id.data(), "MTrk", 4) == 0) {
            Track t;
            ParseTrack(r.Cur(), avail, t);
            song.tracks.push_back(std::move(t));
        }
        r.Skip(avail);
    }
    if (song.tracks.empty()) { err = L"The file contains no tracks."; return false; }
    return true;
}

// ------------------------------------------------------------- SMF writing
inline void PutU16(std::vector<uint8_t>& o, uint32_t v) { o.push_back((uint8_t)(v >> 8)); o.push_back((uint8_t)v); }
inline void PutU32(std::vector<uint8_t>& o, uint32_t v) { PutU16(o, v >> 16); PutU16(o, v & 0xFFFF); }
inline void PutVLQ(std::vector<uint8_t>& o, uint32_t v) {
    uint8_t tmp[5]; int n = 0;
    tmp[n++] = v & 0x7F;
    while (v >>= 7) tmp[n++] = (uint8_t)((v & 0x7F) | 0x80);
    while (n--) o.push_back(tmp[n]);
}

inline std::vector<uint8_t> WriteSMF(const Song& in) {
    Song s = in;
    std::vector<uint8_t> out;
    int fmt = s.format;
    if (s.tracks.size() > 1 && fmt == 0) fmt = 1;
    if (s.tracks.size() <= 1 && fmt == 1) fmt = 0;

    out.insert(out.end(), { 'M','T','h','d' });
    PutU32(out, 6); PutU16(out, fmt); PutU16(out, (uint32_t)s.tracks.size()); PutU16(out, s.ppq);

    for (Track& t : s.tracks) {
        SortTrack(t);
        std::vector<uint8_t> body;
        uint32_t last = 0;
        for (const Event& e : t.events) {
            PutVLQ(body, e.tick - last); last = e.tick;
            if (e.IsMeta()) {
                body.push_back(0xFF); body.push_back(e.d1);
                PutVLQ(body, (uint32_t)e.data.size());
                body.insert(body.end(), e.data.begin(), e.data.end());
            } else if (e.IsSysex()) {
                body.push_back(e.status);
                PutVLQ(body, (uint32_t)e.data.size());
                body.insert(body.end(), e.data.begin(), e.data.end());
            } else {
                body.push_back(e.status); body.push_back(e.d1);
                int hi = e.status & 0xF0;
                if (hi != 0xC0 && hi != 0xD0) body.push_back(e.d2);
            }
        }
        uint32_t endT = std::max(t.endTick, last);
        PutVLQ(body, endT - last);
        body.push_back(0xFF); body.push_back(0x2F); body.push_back(0x00);

        out.insert(out.end(), { 'M','T','r','k' });
        PutU32(out, (uint32_t)body.size());
        out.insert(out.end(), body.begin(), body.end());
    }
    return out;
}

// Writes the song as a format 0 file: all tracks are merged into one single track.  Simultaneous events keep the
// usual order (meta, note-off, other, note-on).  Per-track names / instrument names (meta 0x03 / 0x04) cannot be kept
// in a single track, so only the very first track name is written; markers, tempo, time signature and every
// other meta event stay.
inline std::vector<uint8_t> WriteSMF0(const Song& in) {
    Song m;
    m.ppq = in.ppq; m.format = 0; m.bendRange = in.bendRange;
    Track one;
    bool haveName = false;
    for (const Track& t : in.tracks) {
        for (const Event& e : t.events) {
            if (e.IsMeta() && (e.d1 == 0x03 || e.d1 == 0x04)) {
                if (e.d1 == 0x03 && !haveName) { haveName = true; one.events.push_back(e); }
                continue;
            }
            one.events.push_back(e);
        }
        one.endTick = std::max(one.endTick, t.endTick);
    }
    m.tracks.push_back(std::move(one));
    return WriteSMF(m);          // exactly one track and format 0
}

// Inserts "pitch-bend sensitivity" RPN messages at tick 0 for every channel that
// contains pitch-bend data, so the bend range the effects assumed is also what the
// receiving synth uses (used for playback and when exporting).
inline void AddBendRangeRPN(Song& s) {
    int semis = (int)s.bendRange;
    int cents = (int)std::floor((s.bendRange - semis) * 100.0 + 0.5);
    for (Track& t : s.tracks) {
        bool used[16] = {};
        for (const Event& e : t.events)
            if (e.IsChannel() && (e.status & 0xF0) == 0xE0) used[e.status & 0x0F] = true;
        std::vector<Event> pre;
        for (int ch = 0; ch < 16; ch++) {
            if (!used[ch]) continue;
            const int msgs[6][2] = { {101,0}, {100,0}, {6,semis}, {38,cents}, {101,127}, {100,127} };
            for (auto& m : msgs) {
                Event e; e.tick = 0; e.status = (uint8_t)(0xB0 | ch); e.d1 = (uint8_t)m[0]; e.d2 = (uint8_t)m[1];
                pre.push_back(e);
            }
        }
        if (!pre.empty()) t.events.insert(t.events.begin(), pre.begin(), pre.end());
    }
}

// --------------------------------------------------------------- tempo map
struct TempoSeg { uint32_t tick; uint32_t uspq; double ms; };

inline std::vector<TempoSeg> BuildTempoMap(const Song& s) {
    std::vector<std::pair<uint32_t, uint32_t>> v;
    for (const Track& t : s.tracks)
        for (const Event& e : t.events)
            if (e.IsMeta() && e.d1 == 0x51 && e.data.size() == 3) {
                uint32_t us = ((uint32_t)e.data[0] << 16) | ((uint32_t)e.data[1] << 8) | e.data[2];
                if (us) v.push_back({ e.tick, us });
            }
    std::stable_sort(v.begin(), v.end(),
        [](const std::pair<uint32_t, uint32_t>& a, const std::pair<uint32_t, uint32_t>& b) { return a.first < b.first; });

    std::vector<TempoSeg> m;
    m.push_back({ 0, 500000, 0.0 });
    for (auto& p : v) {
        if (p.first == m.back().tick) { m.back().uspq = p.second; continue; }
        const TempoSeg& l = m.back();
        double ms = l.ms + (double)(p.first - l.tick) * l.uspq / s.ppq / 1000.0;
        m.push_back({ p.first, p.second, ms });
    }
    return m;
}

inline double TickToMs(const std::vector<TempoSeg>& m, int ppq, uint32_t tick) {
    size_t i = m.size() - 1;
    while (i > 0 && m[i].tick > tick) i--;
    return m[i].ms + (double)(tick - m[i].tick) * m[i].uspq / ppq / 1000.0;
}

inline uint32_t MsToTick(const std::vector<TempoSeg>& m, int ppq, double ms) {
    if (ms <= 0) return 0;
    size_t i = m.size() - 1;
    while (i > 0 && m[i].ms > ms) i--;
    double t = m[i].tick + (ms - m[i].ms) * 1000.0 * ppq / m[i].uspq;
    return (uint32_t)(t + 0.5);
}

inline void GetTimeSig(const Song& s, int& num, int& den) {
    num = 4; den = 4;
    uint32_t best = 0xFFFFFFFFu;
    for (const Track& t : s.tracks)
        for (const Event& e : t.events)
            if (e.IsMeta() && e.d1 == 0x58 && e.data.size() >= 2 && e.tick < best) {
                best = e.tick; num = e.data[0] ? e.data[0] : 4; den = 1 << std::min<int>(e.data[1], 6);
            }
}

// ------------------------------------------------------- format 0 -> tracks
// A format 0 file keeps every channel in one single track.  To work with it track by track, the events
// are distributed: track 0 keeps the conductor data (tempo, time signature, sysex, text ... everything that
// is not a channel message), and every MIDI channel that is used gets a track of its own.  The song
// becomes format 1.  Returns true when the song was split.
inline bool SplitFormat0(Song& s) {
    if (s.format != 0 || s.tracks.size() != 1) return false;
    const Track& src = s.tracks[0];
    bool used[16] = {};
    for (const Event& e : src.events) if (e.IsChannel()) used[e.status & 0x0F] = true;
    int nUsed = 0; for (int c = 0; c < 16; c++) if (used[c]) nUsed++;
    if (nUsed == 0) return false;

    std::vector<Track> out(1 + (size_t)nUsed);
    int slot[16]; int k = 1;
    for (int c = 0; c < 16; c++) slot[c] = used[c] ? k++ : -1;
    for (const Event& e : src.events) {
        if (e.IsChannel()) out[(size_t)slot[e.status & 0x0F]].events.push_back(e);
        else               out[0].events.push_back(e);
    }
    for (int c = 0; c < 16; c++) {
        if (!used[c]) continue;
        Track& t = out[(size_t)slot[c]];
        Event nm; nm.tick = 0; nm.status = 0xFF; nm.d1 = 0x03;                // track name
        std::string name = (c == 9) ? std::string("Drums (channel 10)") : "Channel " + std::to_string(c + 1);
        nm.data.assign(name.begin(), name.end());
        t.events.insert(t.events.begin(), nm);
    }
    for (Track& t : out) t.endTick = src.endTick;
    s.tracks = std::move(out);
    s.format = 1;
    return true;
}

// ---------------------------------------------------------------- note view
struct Note { uint32_t start, end; uint8_t ch, key, vel, offvel; };

// Splits a track into paired notes and "everything else".
inline void ExtractNotes(const Track& t, std::vector<Note>& notes, std::vector<Event>& others) {
    uint32_t last = 0;
    for (const Event& e : t.events) last = std::max(last, e.tick);
    last = std::max(last, t.endTick);

    const uint32_t OPEN = 0xFFFFFFFFu;
    std::vector<std::deque<size_t>> open(16 * 128);
    for (const Event& e : t.events) {
        if (e.IsNoteOn()) {
            Note n{ e.tick, OPEN, (uint8_t)(e.status & 0x0F), (uint8_t)(e.d1 & 0x7F), e.d2, 64 };
            notes.push_back(n);
            open[n.ch * 128 + n.key].push_back(notes.size() - 1);
        } else if (e.IsNoteOff()) {
            auto& q = open[(e.status & 0x0F) * 128 + (e.d1 & 0x7F)];
            if (!q.empty()) {
                Note& n = notes[q.front()]; q.pop_front();
                n.end = std::max(e.tick, n.start); n.offvel = e.d2;
            }
        } else {
            others.push_back(e);
        }
    }
    for (Note& n : notes) {
        if (n.end == OPEN) n.end = std::max(last, n.start + 1);
        if (n.end <= n.start) n.end = n.start + 1;
    }
}

inline void RebuildTrack(Track& t, const std::vector<Note>& notes, const std::vector<Event>& others) {
    t.events = others;
    for (const Note& n : notes) {
        Event on;  on.tick = n.start; on.status = 0x90 | n.ch; on.d1 = n.key; on.d2 = n.vel ? n.vel : 1;
        Event off; off.tick = n.end;  off.status = 0x80 | n.ch; off.d1 = n.key; off.d2 = n.offvel;
        t.events.push_back(on); t.events.push_back(off);
    }
    SortTrack(t);
    for (const Event& e : t.events) t.endTick = std::max(t.endTick, e.tick);
}

} // namespace mc
