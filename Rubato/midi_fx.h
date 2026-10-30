// midi_fx.h - MIDI FX (effects) that transform a Track.  Platform independent.
// (c) 2026, O.Jagannatha Rao (JagiChan)
#pragma once
#include <memory>
#include <random>
#include "midi_core.h"

namespace mc {

enum FxType { FX_TRANSPOSE = 0, FX_QUANTIZE, FX_HUMANIZE, FX_VELOCITY, FX_AUTOPHRASE, FX_HUMANCC, FX_BREATH, FX_STRUM, FX_COUNT };

const int FX_MAXP = 10;   // maximum parameters per effect

// Describes an effect and its numeric parameters for the UI and for presets.
struct FxDesc {
    const wchar_t* id;          // stable id used in preset files
    const wchar_t* name;
    int            np;
    int            seedIdx;     // index of the "seed" parameter, or -1
    const wchar_t* label[FX_MAXP];
    double         def[FX_MAXP];
    const wchar_t* hint;        // how the effect works (shown in the description box)
    const wchar_t* phint[FX_MAXP];   // what each parameter does (shown while its box has the focus)
};

inline const FxDesc& GetFxDesc(int t) {
    static const FxDesc d[FX_COUNT] = {
        { L"transpose", L"Transpose", 2, -1,
          { L"Semitones (+/-)", L"Skip drums (1/0)" }, { 0, 1 },
          L"Moves every note up or down by whole semitones (12 = one octave). Notes pushed outside the MIDI range are removed. "
          L"Drums (channel 10) can be left alone.",
          { L"Semitones: 12 = one octave up, -12 = one octave down.",
            L"1 = leave the drum channel (10) untouched, 0 = transpose it as well." } },

        { L"quantize", L"Quantizer", 6, -1,
          { L"Grid: 1/N note", L"Strength %", L"Start offset (ticks)", L"Shift onward %", L"Duration grid 1/N", L"Reduce dur (ticks)" },
          { 16, 100, 0, 0, 0, 0 },
          L"Snaps note starts to a rhythmic grid. Strength 100% puts notes exactly on the grid; lower values move them only part of the way "
          L"and keep some of the original feel. Start offset moves the whole grid, Shift onward delays every second grid point (swing), "
          L"and the duration settings tidy the note lengths.",
          { L"Grid size: 16 = sixteenth notes, 8 = eighth notes, 4 = quarter notes, 32 = thirty-second notes.",
            L"100 = notes land exactly on the grid, 50 = halfway, 0 = no change.",
            L"Moves the whole grid by this many MIDI ticks (positive = later). Small values (2-6) give a laid-back feel.",
            L"Delays every second grid point by this % of the grid size (swing / shuffle). 0 = straight.",
            L"Rounds note lengths to this grid (16 = sixteenth). 0 = leave the lengths alone.",
            L"Shortens every note by this many ticks. A value of 1 removes overlaps between neighbouring notes." } },

        { L"humanize", L"Humanize (timing/velocity)", 4, 3,
          { L"Timing +/- ms", L"Velocity +/-", L"Length +/- %", L"Seed (0=auto)" }, { 10, 5, 3, 0 },
          L"Adds the small random imperfections of a live player to the start time (in milliseconds), the velocity and the length of each note. "
          L"The random values follow a bell curve: most notes stay close to the original and a few drift further. "
          L"The same seed always gives the same result; \"New variation\" draws another one.",
          { L"Largest random shift of a note start in milliseconds. Typical: legato wind 1-4, guitar 6-12, ensembles 10-18.",
            L"Largest random change of the velocity (loudness) of a note.",
            L"Largest random change of the length of a note, in percent.",
            L"0 = automatic (stable for this effect). Any other number gives its own repeatable variation." } },

        { L"velocity", L"Velocity", 2, -1,
          { L"Scale %", L"Offset (+/-)" }, { 100, 0 },
          L"Scales and then shifts all note velocities: new velocity = old x Scale% + Offset. Use it to bring a flat or too quiet/loud "
          L"recording to the level a preset expects before humanizing.",
          { L"100 = unchanged, 50 = half as strong, 120 = 20% stronger.",
            L"Added after scaling. Raw recordings at velocity 82 become 96 with an offset of +14." } },

        { L"autophrase", L"Phrase Finder", 5, -1,
          { L"Min length (/16)", L"Max length (/16)", L"Pause >= (/16)", L"Sensitivity %", L"Prefer 1/2/3" },
          { 12, 20, 4, 40, 3 },
          L"Finds the musical phrases - the places where a player would take a breath or lift the bow - and hands them to the effects BELOW it in "
          L"the chain (Human Pitch/Mod/Expr, Breath & Phrase Dynamics). A phrase is between Min and Max long, measured in sixteenth notes. "
          L"It ends at a rest of at least 'Pause', otherwise at the best break inside the allowed length, preferring long notes, big pitch "
          L"jumps and velocity drops. The phrase starts are shown as small red marks on the track strip. Put it first in the chain.",
          { L"Shortest phrase in sixteenth notes (12 = three beats).",
            L"Longest phrase in sixteenth notes (20 = five beats). The phrase is cut at the best place before this length.",
            L"A rest of at least this many sixteenth notes always ends a phrase.",
            L"How strongly big pitch jumps and velocity drops attract a phrase break, 0-100.",
            L"What to look at when choosing a break: 1 = velocity drops, 2 = pitch jumps, 3 = both." } },

        { L"humancc", L"Human Pitch / Mod / Expr", 10, 9,
          { L"Drift (cents)", L"Vibrato (cents)", L"Scoop-in (cents)", L"Scoop on 0-3", L"Glide (c/semitone)",
            L"Fall-off (cents)", L"Modulation max", L"Long note (ms)", L"Expression %", L"Seed (0=auto)" },
          { 5, 8, 60, 2, 0, 0, 40, 450, 0, 0 },
          L"Draws the pitch-wheel and modulation movement of a live player. Drift: slow tuning wander. Vibrato: pitch vibrato that grows on long "
          L"notes. Scoop-in: a note starts slightly flat and slides up into pitch - 'Scoop on' chooses which notes (wind players scoop long notes "
          L"approached from below, guitarists get a pick dip on every note). Glide: finger/fret slide into upward leaps. Fall-off: the pitch drops "
          L"at the end of a phrase. Modulation: a vibrato-depth bell on long notes, for sounds that use the mod wheel. "
          L"The pitch-bend range of your synth is set in the box at the bottom right.",
          { L"Slow random tuning wander in cents (100 cents = 1 semitone). 3-6 is natural.",
            L"Depth of pitch vibrato on long notes, in cents. It fades in after a short delay. 0 = none.",
            L"How flat a note starts, in cents, before sliding up into pitch. About 60 for a wind scoop or a guitar pick dip.",
            L"Which notes scoop: 0 = none, 1 = first note of each phrase, 2 = long notes approached from below (winds), 3 = every note (plucked strings).",
            L"Extra slide into upward leaps of 3+ semitones, in cents per semitone of the leap (guitar fret glide about 20, violin 12). 0 = off.",
            L"Pitch drop on the last long note of a phrase, in cents (sax / trumpet 40-80).",
            L"Peak of the modulation-wheel (CC1) bell on long notes, 0-127. 0 = off.",
            L"Only notes at least this long (milliseconds) get vibrato and modulation.",
            L"Random expression (CC11) wobble in percent. 0 = leave CC11 alone (use Breath & Phrase Dynamics for expression).",
            L"0 = automatic (stable for this effect). Any other number gives its own repeatable variation." } },

        { L"breath", L"Breath & Phrase Dynamics", 9, 8,
          { L"Breath depth %", L"Phrase arc %", L"Note pulse %", L"Turbulence %", L"Breath gap (ms)",
            L"Gap dip %", L"Velocity arc (+/-)", L"Output 1/2/3", L"Seed (0=auto)" },
          { 70, 40, 70, 20, 76, 70, 20, 3, 0 },
          L"Shapes the loudness the way a wind player or bow does. Every note gets a breath attack that relaxes and swells toward the next note; "
          L"every phrase rises and falls; and a breath is taken between phrases: the last note of the phrase is shortened (Breath gap) and the "
          L"loudness dips during the gap. Velocity arc applies the same phrase shape to the note velocities. "
          L"Output: 1 = Expression (CC11), 2 = Breath (CC2), 3 = both. Phrases come from the Phrase Finder above it in the chain.",
          { L"How far the loudness moves, in percent of the controller range. 100 matches the reference flute (values from about 90 to 127).",
            L"How much the loudness rises and falls across each phrase.",
            L"Strength of the attack / relax / swell shape inside every note.",
            L"Airy noise and a small 'chiff' at note starts. Only written to Breath (CC2).",
            L"The last note of every phrase is shortened so that this many milliseconds are left for the breath. 0 = keep note lengths.",
            L"How far the loudness drops during the breath gap (70 = down to about a third, as in the reference flute).",
            L"How much the note velocities follow the phrase arc: softer at the start and end, strongest in the middle. 0 = keep velocities.",
            L"1 = Expression CC11, 2 = Breath CC2, 3 = both.",
            L"0 = automatic (stable for this effect). Any other number gives its own repeatable variation." } },

        { L"strum", L"Strum / Chord spread", 6, 5,
          { L"Spread (ms/note)", L"Direction 1/2/3", L"Chord window (ms)", L"Velocity taper %", L"Random (ms)", L"Seed (0=auto)" },
          { 8, 3, 25, 12, 3, 0 },
          L"Spreads the notes of a chord in time like a strummed or arpeggiated instrument (guitar, harp, accordion, piano). Notes that start "
          L"within the chord window count as one chord and are delayed one after the other. Direction picks down-strokes (low to high), "
          L"up-strokes (high to low) or alternating strokes; Velocity taper makes the later notes a little softer.",
          { L"Delay between neighbouring chord notes, in milliseconds. Guitar 6-14, harp 10-20, piano 3-6.",
            L"1 = down-stroke (low to high), 2 = up-stroke (high to low), 3 = alternate.",
            L"Notes starting within this many milliseconds of each other are treated as one chord.",
            L"How much softer the last note of the strum is than the first, in percent.",
            L"Extra random delay per note in milliseconds, so the strum is not mechanical.",
            L"0 = automatic (stable for this effect). Any other number gives its own repeatable variation." } },
    };
    return d[t];
}

// Allowed range and spinner step of every effect parameter (the edit boxes of the chain dialog, preset files, project
// files and the effects themselves all use these limits, so a value can never leave its range).
struct FxLimit { double lo, hi, step; };

inline FxLimit GetFxLimit(int type, int k) {
    static const FxLimit L[FX_COUNT][FX_MAXP] = {
        /* transpose  */ { {-48, 48, 1}, {0, 1, 1} },
        /* quantize   */ { {1, 128, 1}, {0, 100, 1}, {-480, 480, 1}, {0, 100, 1}, {0, 128, 1}, {0, 480, 1} },
        /* humanize   */ { {0, 250, 1}, {0, 127, 1}, {0, 100, 1}, {0, 999999, 1} },
        /* velocity   */ { {0, 400, 1}, {-127, 127, 1} },
        /* autophrase */ { {1, 128, 1}, {1, 256, 1}, {0.5, 64, 0.5}, {0, 100, 1}, {1, 3, 1} },
        /* humancc    */ { {0, 100, 1}, {0, 100, 1}, {0, 300, 1}, {0, 3, 1}, {0, 150, 1}, {0, 300, 1}, {0, 127, 1}, {50, 5000, 10}, {0, 100, 1}, {0, 999999, 1} },
        /* breath     */ { {0, 100, 1}, {0, 100, 1}, {0, 100, 1}, {0, 100, 1}, {0, 1000, 5}, {0, 100, 1}, {0, 127, 1}, {1, 3, 1}, {0, 999999, 1} },
        /* strum      */ { {0, 200, 1}, {1, 3, 1}, {1, 500, 1}, {0, 100, 1}, {0, 200, 1}, {0, 999999, 1} },
    };
    if (type < 0 || type >= FX_COUNT || k < 0 || k >= FX_MAXP) return FxLimit{ -1e9, 1e9, 1 };
    const FxLimit l = L[type][k];
    if (l.hi == 0 && l.lo == 0 && l.step == 0) return FxLimit{ -1e9, 1e9, 1 };   // unused slot
    return l;
}

inline double ClampParam(int type, int k, double v) {
    const FxLimit l = GetFxLimit(type, k);
    if (!(v == v)) v = l.lo;                       // NaN
    return v < l.lo ? l.lo : (v > l.hi ? l.hi : v);
}

struct IEffect {
    virtual ~IEffect() {}
    virtual void Process(Track& t, const Song& s) = 0;
};

inline int RoundI(double v) { return (int)std::floor(v + 0.5); }
inline int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double C01(double v) { return v < 0 ? 0.0 : (v > 1 ? 1.0 : v); }
inline double Ease(double x) { x = C01(x); return (1.0 - std::cos(x * 3.14159265358979323846)) * 0.5; }
const double PI_ = 3.14159265358979323846;

inline void SeedRng(std::mt19937& r, unsigned seed) {
    if (seed == 0) { std::random_device rd; r.seed(rd()); } else r.seed(seed);
}

// Controller resolution: about 8 ms, like the reference files (8 ticks at 480 PPQ and 131 BPM)
inline uint32_t StepTicks(const std::vector<TempoSeg>& tm, int ppq) {
    double uspq = tm.empty() ? 500000.0 : (double)tm[0].uspq;
    return (uint32_t)std::max(1, RoundI(8.0 * ppq * 1000.0 / uspq));
}

// --------------------------------------------------------- phrase detection
struct Phrase { size_t first, last; uint32_t t0, t1; };

// notes: one channel, sorted by start.  A phrase is between cfg.minE and cfg.maxE sixteenth notes long.
// Rests of at least cfg.pauseE always end a phrase.  Inside a continuous line the break points are chosen
// together (dynamic programming) so that the whole line is cut at the best places - long notes, rests,
// pitch jumps and velocity drops - while keeping the phrase lengths close to the allowed range.
inline void DetectPhrases(const std::vector<Note>& n, int ppq, const PhraseCfg& cfg, std::vector<Phrase>& out) {
    out.clear();
    if (n.empty()) return;
    const double E = ppq / 2.0;                       // one eighth note in ticks (unit of the break scoring below)
    const double U = ppq / 4.0;                       // one sixteenth note in ticks (unit of the user's settings)
    const double minT = std::max(0.5, cfg.minE) * U;
    const double maxT = std::max(std::max(0.5, cfg.minE), cfg.maxE) * U;
    const long pauseT = (long)(std::max(0.25, cfg.pauseE) * U);
    const double sens = std::min(100.0, std::max(0.0, cfg.sens)) / 100.0;

    std::vector<uint32_t> d;
    for (const Note& x : n) d.push_back(x.end - x.start);
    std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
    const double med = std::max(1.0, (double)d[d.size() / 2]);
    const size_t N = n.size();

    // quality of a phrase break after note i (i+1 exists)
    auto boundary = [&](size_t i) {
        double gap = std::max(0.0, (double)((long)n[i + 1].start - (long)n[i].end));
        double sc = gap / E * 2.0 + (double)(n[i].end - n[i].start) / med;
        if (cfg.prefer & 2) sc += sens * std::abs((int)n[i + 1].key - (int)n[i].key) / 4.0;
        if (cfg.prefer & 1) sc += sens * std::max(0, (int)n[i].vel - (int)n[i + 1].vel) / 16.0;
        return sc;
    };

    size_t segA = 0;
    for (size_t i = 0; i < N; i++) {
        const bool segEnd = (i + 1 == N) || ((long)n[i + 1].start - (long)n[i].end >= pauseT);
        if (!segEnd) continue;
        const size_t a = segA, b = i;                 // one run of notes without a rest: cut it by dynamic programming
        segA = i + 1;
        const size_t m = b - a + 1;
        std::vector<double> dp(m, -1e18);
        std::vector<size_t> from(m, 0);
        for (size_t k = 0; k < m; k++) {
            const size_t ie = a + k;
            const double sc = (ie == b) ? 3.0 : boundary(ie);
            for (size_t kj = k + 1; kj-- > 0; ) {                         // phrase = notes a+kj .. a+k
                const double len = (double)n[ie].end - (double)n[a + kj].start;
                if (len > maxT * 1.7 && kj < k) break;
                const double prevDp = kj == 0 ? 0.0 : dp[kj - 1];
                if (prevDp < -1e17) continue;
                double pen = len < minT ? (minT - len) / E : (len > maxT ? (len - maxT) / E : 0.0);
                if (ie == b && len < minT) pen *= 0.25;                  // a short last phrase is natural
                const double val = prevDp + sc - pen * 1.2 - 0.5;
                if (val > dp[k]) { dp[k] = val; from[k] = kj; }
            }
        }
        std::vector<Phrase> seg;
        for (size_t k = m; k-- > 0; ) {
            const size_t kj = from[k];
            seg.push_back({ a + kj, a + k, n[a + kj].start, std::max(n[a + k].end, n[a + kj].start + 1) });
            if (kj == 0) break;
            k = kj;                                                       // loop's k-- continues with kj-1
        }
        std::reverse(seg.begin(), seg.end());
        out.insert(out.end(), seg.begin(), seg.end());
    }
}

// smooth random curve in [-1,1] (cosine-interpolated value noise)
struct Noise {
    std::vector<double> k; uint32_t base = 0, step = 1;
    void Init(std::mt19937& r, uint32_t b, uint32_t e, uint32_t st) {
        base = b; step = std::max<uint32_t>(1, st);
        std::uniform_real_distribution<double> d(-1.0, 1.0);
        k.resize((size_t)((e > b ? e - b : 0) / step) + 3);
        for (double& x : k) x = d(r);
    }
    double At(uint32_t t) const {
        double p = (double)(t - base) / step;
        size_t i = (size_t)p; double f = Ease(p - (double)i);
        size_t j = std::min(i + 1, k.size() - 1); i = std::min(i, k.size() - 1);
        return k[i] * (1.0 - f) + k[j] * f;
    }
};

// Shared helper: per channel list of note indices sorted by start.
inline void ChannelNotes(const std::vector<Note>& all, int ch, std::vector<size_t>& idx) {
    idx.clear();
    for (size_t i = 0; i < all.size(); i++) if (all[i].ch == ch) idx.push_back(i);
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return all[a].start < all[b].start; });
}

// ----------------------------------------------------------------- Transpose
class TransposeFx : public IEffect {
public:
    TransposeFx(int semi, bool skipDrums) : semi_(semi), skip_(skipDrums) {}
    void Process(Track& t, const Song&) override {
        std::vector<Note> notes; std::vector<Event> others;
        ExtractNotes(t, notes, others);
        std::vector<Note> out;
        for (Note n : notes) {
            if (!(skip_ && n.ch == 9)) {
                int k = n.key + semi_;
                if (k < 0 || k > 127) continue;       // out of range -> note removed
                n.key = (uint8_t)k;
            }
            out.push_back(n);
        }
        RebuildTrack(t, out, others);
    }
private:
    int semi_; bool skip_;
};

// ----------------------------------------------------------------- Quantizer
class QuantizeFx : public IEffect {
public:
    QuantizeFx(int division, double strengthPct, double offsetTicks, double swingPct, int durDiv, double reduceTicks)
        : div_(division < 1 ? 1 : division), str_(std::min(100.0, std::max(0.0, strengthPct)) / 100.0),
          off_(offsetTicks), swing_(std::min(100.0, std::max(0.0, swingPct)) / 100.0),
          durDiv_(durDiv < 0 ? 0 : durDiv), red_(std::max(0.0, reduceTicks)) {}
    void Process(Track& t, const Song& s) override {
        const double grid = std::max(1.0, std::floor(s.ppq * 4.0 / div_ + 0.5));
        const double dgrid = durDiv_ > 0 ? std::max(1.0, std::floor(s.ppq * 4.0 / durDiv_ + 0.5)) : 0.0;
        std::vector<Note> notes; std::vector<Event> others;
        ExtractNotes(t, notes, others);
        for (Note& n : notes) {
            double len = (double)(n.end - n.start);
            double slot = std::floor(n.start / grid + 0.5);
            double q = slot * grid;
            if (swing_ > 0 && ((long long)slot & 1)) q += grid * swing_;     // "shift onward"
            q += off_;
            double ns = n.start + (q - n.start) * str_;
            double nl = len;
            if (dgrid > 0) {
                double ql = std::max(dgrid, std::floor(len / dgrid + 0.5) * dgrid);
                nl = len + (ql - len) * str_;
            }
            nl -= red_;
            uint32_t st = (uint32_t)std::max(0.0, std::floor(ns + 0.5));
            uint32_t ln = (uint32_t)std::max(1.0, std::floor(nl + 0.5));
            n.start = st; n.end = st + ln;
        }
        RebuildTrack(t, notes, others);
    }
private:
    int div_; double str_, off_, swing_; int durDiv_; double red_;
};

// ------------------------------------------------------------------ Humanize
class HumanizeFx : public IEffect {
public:
    HumanizeFx(double timeMs, int velRange, double lenPct, unsigned seed)
        : tt_(std::fabs(timeMs)), vv_(std::abs(velRange)), lp_(std::fabs(lenPct)) { SeedRng(rng_, seed); }
    void Process(Track& t, const Song& s) override {
        std::vector<Note> notes; std::vector<Event> others;
        ExtractNotes(t, notes, others);
        std::vector<TempoSeg> tm = BuildTempoMap(s);
        std::normal_distribution<double> nd(0.0, 1.0);
        auto bell = [&](double range) { double v = nd(rng_) * range * 0.5; return std::max(-range, std::min(range, v)); };
        for (Note& n : notes) {
            double len = (double)(n.end - n.start) * (1.0 + bell(lp_) / 100.0);
            uint32_t ln = (uint32_t)std::max(1.0, std::floor(len + 0.5));
            double ms = TickToMs(tm, s.ppq, n.start) + bell(tt_);
            n.start = MsToTick(tm, s.ppq, std::max(0.0, ms));
            n.end = n.start + ln;
            n.vel = (uint8_t)Clamp(n.vel + RoundI(bell(vv_)), 1, 127);
        }
        RebuildTrack(t, notes, others);
    }
private:
    double tt_; int vv_; double lp_; std::mt19937 rng_;
};

// ------------------------------------------------------------------ Velocity
class VelocityFx : public IEffect {
public:
    VelocityFx(double scalePct, int offset) : sc_(scalePct / 100.0), off_(offset) {}
    void Process(Track& t, const Song&) override {
        for (Event& e : t.events)
            if (e.IsNoteOn())
                e.d2 = (uint8_t)Clamp(RoundI(e.d2 * sc_) + off_, 1, 127);
    }
private:
    double sc_; int off_;
};

// ----------------------------------------------------------------- Phrase Finder
// Does not change the track: it sets the phrase recognition settings for the effects below it.
class PhraseFinderFx : public IEffect {
public:
    PhraseFinderFx(double minE, double maxE, double pauseE, double sens, int prefer) {
        cfg_.minE = std::max(1.0, minE); cfg_.maxE = std::max(cfg_.minE, maxE);
        cfg_.pauseE = std::max(0.5, pauseE); cfg_.sens = std::min(100.0, std::max(0.0, sens));
        cfg_.prefer = Clamp(prefer, 1, 3);
    }
    void Process(Track&, const Song& s) override { s.phrase = cfg_; }
private:
    PhraseCfg cfg_;
};

inline PhraseCfg PhraseCfgFromParams(const double* p) {
    PhraseCfg c;
    c.minE = std::max(1.0, p[0]); c.maxE = std::max(c.minE, p[1]); c.pauseE = std::max(0.5, p[2]);
    c.sens = std::min(100.0, std::max(0.0, p[3])); c.prefer = Clamp(RoundI(p[4]), 1, 3);
    return c;
}

// ------------------------------------------------------- Human Pitch / Mod / Expr
class HumanControllersFx : public IEffect {
public:
    HumanControllersFx(double drift, double vib, double scoop, int scoopMode, double glide, double fall,
                       int modMax, double longMs, double exprPct, unsigned seed)
        : drift_(std::max(0.0, drift)), vib_(std::max(0.0, vib)), scoop_(std::max(0.0, scoop)),
          mode_(Clamp(scoopMode, 0, 3)), glide_(std::max(0.0, glide)), fall_(std::max(0.0, fall)),
          mod_(Clamp(modMax, 0, 127)), longMs_(std::max(50.0, longMs)),
          expr_(std::min(100.0, std::max(0.0, exprPct)) / 100.0) { SeedRng(rng_, seed); }

    void Process(Track& t, const Song& s) override {
        range_ = std::min(24.0, std::max(0.5, s.bendRange));
        cfg_ = s.phrase;
        doBend_ = drift_ > 0 || vib_ > 0 || (scoop_ > 0 && mode_ > 0) || glide_ > 0 || fall_ > 0;
        doExpr_ = expr_ > 0; doMod_ = mod_ > 0;
        if (!doBend_ && !doExpr_ && !doMod_) return;

        std::vector<Note> notes; std::vector<Event> others;
        ExtractNotes(t, notes, others);
        std::vector<TempoSeg> tempo = BuildTempoMap(s);
        std::vector<Event> gen;
        bool handled[16] = {};
        std::vector<size_t> idx;
        for (int ch = 0; ch < 16; ch++) {
            if (ch == 9) continue;
            ChannelNotes(notes, ch, idx);
            if (idx.empty()) continue;
            std::vector<Note> cn;
            for (size_t i : idx) cn.push_back(notes[i]);
            handled[ch] = true;
            GenChannel(ch, cn, s.ppq, tempo, gen);
        }
        std::vector<Event> kept;
        for (const Event& e : others) {
            if (e.IsChannel() && handled[e.status & 0x0F]) {
                int hi = e.status & 0xF0;
                if (doBend_ && hi == 0xE0) continue;
                if (hi == 0xB0 && ((doExpr_ && e.d1 == 11) || (doMod_ && e.d1 == 1))) continue;
            }
            kept.push_back(e);
        }
        kept.insert(kept.end(), gen.begin(), gen.end());
        RebuildTrack(t, notes, kept);
    }

private:
    struct PerNote { double msS, msE, level, phase, delay, dipC, dipMs, fall, modStart, modPeak; };

    void GenChannel(int ch, const std::vector<Note>& cn, int ppq, const std::vector<TempoSeg>& tempo, std::vector<Event>& out) {
        const uint32_t first = cn.front().start;
        uint32_t last = 0;
        for (const Note& n : cn) last = std::max(last, n.end);
        if (last <= first) return;

        const uint32_t step = StepTicks(tempo, ppq);
        std::uniform_real_distribution<double> u01(0.0, 1.0);
        Noise drift, phrase;
        drift.Init(rng_, first, last, (uint32_t)(ppq * 3));
        phrase.Init(rng_, first, last, (uint32_t)(ppq * 2));
        const double vibRate = 4.8 + 1.4 * u01(rng_);          // Hz

        const size_t N = cn.size();
        std::vector<PerNote> pn(N);
        std::vector<Phrase> ph;
        DetectPhrases(cn, ppq, cfg_, ph);
        std::vector<char> isFirst(N, 0), isLast(N, 0);
        for (const Phrase& f : ph) { isFirst[f.first] = 1; isLast[f.last] = 1; }

        for (size_t i = 0; i < N; i++) {
            PerNote& p = pn[i];
            p.msS = TickToMs(tempo, ppq, cn[i].start);
            p.msE = TickToMs(tempo, ppq, cn[i].end);
            const double len = p.msE - p.msS;
            p.level = u01(rng_);
            p.phase = 2.0 * PI_ * u01(rng_);
            p.delay = 200.0 + 120.0 * u01(rng_);
            p.dipC = 0; p.dipMs = 60; p.fall = 0; p.modStart = 0; p.modPeak = 0;
            const int interval = i > 0 ? (int)cn[i].key - (int)cn[i - 1].key : 0;

            if (scoop_ > 0) {
                double rnd = 0.85 + 0.3 * u01(rng_);
                if (mode_ == 1 && isFirst[i] && len >= 150.0) p.dipC = scoop_ * rnd;
                else if (mode_ == 2 && len >= 380.0 && (isFirst[i] || interval >= 1)) p.dipC = scoop_ * rnd;
                else if (mode_ == 3) { p.dipC = scoop_ * rnd; p.dipMs = 45; }
            }
            if (glide_ > 0 && interval >= 3 && len >= 120.0) {                 // finger / fret slide into an upward leap
                double g = std::min(150.0, glide_ * interval);
                if (g > p.dipC) { p.dipC = g; p.dipMs = 150; }
            }
            if (fall_ > 0 && isLast[i] && len >= 300.0) p.fall = fall_ * (0.7 + 0.3 * u01(rng_));
            if (mod_ > 0 && len >= longMs_) {                                  // bell of modulation on long notes
                p.modStart = std::max(150.0, len * (0.36 + 0.1 * u01(rng_)));
                p.modPeak = mod_ * (0.7 + 0.3 * u01(rng_));
            }
        }

        int lastBend = -1, lastExpr = -1, lastMod = -1;
        auto emit = [&](uint32_t tick, int hi, int d1, int d2) {
            Event e; e.tick = tick; e.status = (uint8_t)(hi | ch); e.d1 = (uint8_t)d1; e.d2 = (uint8_t)d2;
            out.push_back(e);
        };

        size_t nxt = 0;
        for (uint32_t tk = first; tk < last; tk += step) {
            while (nxt < N && cn[nxt].start <= tk) nxt++;
            int cur = -1;
            for (size_t j = nxt, c = 0; j > 0 && c < 16; j--, c++)
                if (cn[j - 1].end > tk) { cur = (int)(j - 1); break; }

            const double ms = TickToMs(tempo, ppq, tk);
            double ramp = 0, wave = 0, frac = 0, gesture = 0, modv = 0;
            if (cur >= 0) {
                const PerNote& p = pn[cur];
                const double len = p.msE - p.msS, el = ms - p.msS;
                frac = C01(el / std::max(1.0, len));
                if (vib_ > 0 && len >= longMs_) ramp = C01((el - p.delay) / 400.0) * C01((p.msE - ms) / 150.0);
                wave = std::sin(2.0 * PI_ * vibRate * ms / 1000.0 + p.phase);
                if (p.dipC > 0 && el < p.dipMs) gesture -= p.dipC * (1.0 - Ease(el / p.dipMs));
                if (p.fall > 0 && ms > p.msE - 140.0) { double x = C01((ms - (p.msE - 140.0)) / 140.0); gesture -= p.fall * x * x; }
                if (p.modPeak > 0 && el >= p.modStart) {
                    double u = el - p.modStart;
                    if (u < 90.0) modv = p.modPeak * Ease(u / 90.0);
                    else { double rem = len - p.modStart - 90.0; modv = rem > 1.0 ? p.modPeak * (0.5 + 0.5 * std::cos(PI_ * C01((u - 90.0) / rem))) : 0.0; }
                }
            }
            if (doBend_) {
                double cents = drift_ * drift.At(tk) + vib_ * ramp * wave + gesture;
                int v = Clamp(RoundI(8192.0 + cents * 8192.0 / (range_ * 100.0)), 0, 16383);
                const bool moving = std::fabs(gesture) > 0.5 || std::fabs(cents) > 40.0;
                if (lastBend < 0 || std::abs(v - lastBend) >= (moving ? 1 : 14)) { emit(tk, 0xE0, v & 0x7F, v >> 7); lastBend = v; }
            }
            if (doExpr_ && (cur >= 0 || lastExpr < 0)) {
                double level = 0.55 * 0.5 * (1.0 + phrase.At(tk)) + 0.45 * (cur >= 0 ? pn[cur].level : 0.5);
                double drop = 127.0 * expr_ * level * (1.0 - 0.35 * std::sin(PI_ * frac));
                int v = Clamp(RoundI(127.0 - drop), 1, 127);
                if (v != lastExpr) { emit(tk, 0xB0, 11, v); lastExpr = v; }
            }
            if (doMod_) {
                int v = Clamp(RoundI(modv), 0, 127);
                if (v != lastMod) { emit(tk, 0xB0, 1, v); lastMod = v; }
            }
        }
        if (doBend_ && lastBend != 8192) emit(last, 0xE0, 0, 64);        // leave the channel neutral
        if (doMod_  && lastMod != 0)     emit(last, 0xB0, 1, 0);
        if (doExpr_ && lastExpr != 127)  emit(last, 0xB0, 11, 127);
    }

    double drift_, vib_, scoop_; int mode_; double glide_, fall_; int mod_; double longMs_, expr_;
    double range_ = 2.0; PhraseCfg cfg_;
    bool doBend_ = false, doExpr_ = false, doMod_ = false;
    std::mt19937 rng_;
};

// ------------------------------------------------- Breath & Phrase Dynamics
class BreathFx : public IEffect {
public:
    BreathFx(double depthPct, double arcPct, double pulsePct, double turbPct, double gapMs, double gapDipPct,
             double velArc, int output, unsigned seed)
        : depth_(std::min(100.0, std::max(0.0, depthPct)) / 100.0),
          arc_(std::min(100.0, std::max(0.0, arcPct)) / 100.0),
          pulse_(std::min(100.0, std::max(0.0, pulsePct)) / 100.0),
          turb_(std::min(100.0, std::max(0.0, turbPct)) / 100.0),
          gap_(std::max(0.0, gapMs)), gapDip_(std::min(100.0, std::max(0.0, gapDipPct)) / 100.0),
          velArc_(std::max(0.0, velArc)), outExpr_((output & 1) != 0), outBreath_((output & 2) != 0) { SeedRng(rng_, seed); }

    void Process(Track& t, const Song& s) override {
        const bool doCC = depth_ > 0 && (outExpr_ || outBreath_);
        if (!doCC && gap_ <= 0 && velArc_ <= 0) return;
        cfg_ = s.phrase;

        std::vector<Note> notes; std::vector<Event> others;
        ExtractNotes(t, notes, others);
        std::vector<TempoSeg> tempo = BuildTempoMap(s);
        std::vector<Event> gen;
        bool handled[16] = {};
        std::vector<size_t> idx;
        std::uniform_real_distribution<double> u01(0.0, 1.0);
        for (int ch = 0; ch < 16; ch++) {
            if (ch == 9) continue;
            ChannelNotes(notes, ch, idx);
            if (idx.empty()) continue;
            std::vector<Note> cn;
            for (size_t i : idx) cn.push_back(notes[i]);
            std::vector<Phrase> ph;
            DetectPhrases(cn, s.ppq, cfg_, ph);

            if (velArc_ > 0) {                                                // phrase-shaped velocities
                for (const Phrase& f : ph)
                    for (size_t i = f.first; i <= f.last; i++) {
                        double x = C01((double)(cn[i].start - f.t0) / (double)std::max<uint32_t>(1, f.t1 - f.t0));
                        double a = std::sin(PI_ * std::pow(x, 1.2)) - 0.7 * x * x * x - 0.45;   // zero-mean: start low, peak mid, end lowest
                        double v = notes[idx[i]].vel + velArc_ * a + (u01(rng_) - 0.5) * velArc_ * 0.4;
                        notes[idx[i]].vel = (uint8_t)Clamp(RoundI(v), 1, 127);
                    }
            }
            if (gap_ > 0) TrimForBreaths(notes, idx, cn, ph, s.ppq, tempo);   // modifies note ends
            if (doCC) {
                cn.clear();
                for (size_t i : idx) cn.push_back(notes[i]);
                handled[ch] = true;
                GenChannel(ch, cn, ph, s.ppq, tempo, gen);
            }
        }
        std::vector<Event> kept;
        for (const Event& e : others) {
            if (e.IsChannel() && handled[e.status & 0x0F] && (e.status & 0xF0) == 0xB0 &&
                ((outExpr_ && e.d1 == 11) || (outBreath_ && e.d1 == 2))) continue;
            kept.push_back(e);
        }
        kept.insert(kept.end(), gen.begin(), gen.end());
        RebuildTrack(t, notes, kept);
    }

private:
    void TrimForBreaths(std::vector<Note>& notes, const std::vector<size_t>& idx, const std::vector<Note>& cn,
                        const std::vector<Phrase>& ph, int ppq, const std::vector<TempoSeg>& tempo) {
        for (size_t p = 0; p + 1 < ph.size(); p++) {
            Note& n = notes[idx[ph[p].last]];
            const uint32_t nextStart = cn[ph[p + 1].first].start;
            const double msS = TickToMs(tempo, ppq, n.start), msE = TickToMs(tempo, ppq, n.end);
            const double msNext = TickToMs(tempo, ppq, nextStart);
            if (msNext - msE >= gap_) continue;                   // there is already a breath
            double newEnd = msNext - gap_;
            double minEnd = msS + std::max(60.0, 0.55 * (msE - msS));
            newEnd = std::max(newEnd, minEnd);
            if (newEnd < msE) n.end = std::max(n.start + 1, MsToTick(tempo, ppq, newEnd));
        }
    }

    // loudness 0..1 inside a note: attack peak, relax, then swell toward the next note
    double Pulse(double el, double len) const {
        const double dip = 0.28 * pulse_;
        if (len < 150.0) return 1.0 - dip * 0.8 * (el > 30.0 ? 1.0 : el / 30.0);   // fast notes stay lower and flat
        if (el < 50.0) return 1.0;
        if (el < 80.0) return 1.0 - dip * Ease((el - 50.0) / 30.0);
        if (el < 130.0) return 1.0 - dip;
        double u = C01((el - 130.0) / std::max(80.0, len - 130.0));
        return 1.0 - dip * (1.0 - u);
    }

    void GenChannel(int ch, const std::vector<Note>& cn, const std::vector<Phrase>& ph, int ppq,
                    const std::vector<TempoSeg>& tempo, std::vector<Event>& out) {
        const uint32_t first = cn.front().start;
        uint32_t last = 0;
        int lo = 127, hi = 0;
        for (const Note& n : cn) { last = std::max(last, n.end); lo = std::min<int>(lo, n.key); hi = std::max<int>(hi, n.key); }
        if (last <= first) return;

        const uint32_t step = StepTicks(tempo, ppq);
        Noise fast; fast.Init(rng_, first, last, (uint32_t)std::max(1, ppq / 8));

        const size_t N = cn.size();
        std::vector<int> phraseOf(N, 0);
        std::vector<double> pMs0(ph.size()), pMs1(ph.size());
        for (size_t p = 0; p < ph.size(); p++) {
            for (size_t i = ph[p].first; i <= ph[p].last; i++) phraseOf[i] = (int)p;
            pMs0[p] = TickToMs(tempo, ppq, ph[p].t0);
            pMs1[p] = TickToMs(tempo, ppq, ph[p].t1);
        }
        std::vector<double> msS(N), msE(N), pitchN(N);
        for (size_t i = 0; i < N; i++) {
            msS[i] = TickToMs(tempo, ppq, cn[i].start);
            msE[i] = TickToMs(tempo, ppq, cn[i].end);
            pitchN[i] = hi > lo ? (double)(cn[i].key - lo) / (double)(hi - lo) : 0.5;
        }

        int lastE = -1, lastB = -1;
        auto emit = [&](uint32_t tick, int d1, int d2) {
            Event e; e.tick = tick; e.status = (uint8_t)(0xB0 | ch); e.d1 = (uint8_t)d1; e.d2 = (uint8_t)d2;
            out.push_back(e);
        };
        auto toCC = [&](double level) { return Clamp(RoundI(127.0 * (1.0 - depth_ * (1.0 - C01(level)))), 1, 127); };

        size_t nxt = 0;
        for (uint32_t tk = first; tk < last; tk += step) {
            while (nxt < N && cn[nxt].start <= tk) nxt++;
            int cur = -1;
            for (size_t j = nxt, c = 0; j > 0 && c < 16; j--, c++)
                if (cn[j - 1].end > tk) { cur = (int)(j - 1); break; }
            const double ms = TickToMs(tempo, ppq, tk);

            double level = 1.0, el = 0;
            if (cur < 0) {
                const bool startedAny = nxt > 0;
                if (!startedAny) { cur = 0; }
                else {
                    // between two notes: a dip while a breath is taken at a phrase boundary, otherwise hold
                    const size_t prev = nxt - 1;
                    if (nxt < N && phraseOf[nxt] != phraseOf[prev] && gapDip_ > 0) {
                        double gl = msS[nxt] - msE[prev];
                        double u = C01((ms - msE[prev]) / std::max(20.0, std::min(gl, 160.0)));
                        double tri = 1.0 - std::fabs(2.0 * u - 1.0);
                        level = 1.0 - gapDip_ * tri;
                        if (ms - msE[prev] > 160.0) level = 1.0;
                    } else if (lastE >= 0 || lastB >= 0) continue;
                }
            }
            if (cur >= 0) {
                const size_t pi = (size_t)phraseOf[cur];
                double x = C01((ms - pMs0[pi]) / std::max(1.0, pMs1[pi] - pMs0[pi]));
                double arc = std::sin(PI_ * std::pow(x, 1.357));
                double lp = C01(0.55 + 0.45 * arc + 0.2 * (pitchN[cur] - 0.5));
                double lPhrase = 1.0 - arc_ * 0.5 * (1.0 - lp);
                el = ms - msS[cur];
                level = lPhrase * Pulse(el, msE[cur] - msS[cur]);
            }
            if (outExpr_) {
                int v = toCC(level);
                if (v != lastE) { emit(tk, 11, v); lastE = v; }
            }
            if (outBreath_) {
                double air = cur >= 0 ? turb_ * (0.05 * fast.At(tk) + 0.10 * std::exp(-std::max(0.0, el) / 45.0)) : 0.0;
                int v = toCC(level + air);
                if (v != lastB) { emit(tk, 2, v); lastB = v; }
            }
        }
        if (outExpr_ && lastE != 127) emit(last, 11, 127);
        if (outBreath_ && lastB != 127) emit(last, 2, 127);
    }

    double depth_, arc_, pulse_, turb_, gap_, gapDip_, velArc_;
    bool outExpr_, outBreath_;
    PhraseCfg cfg_;
    std::mt19937 rng_;
};

// ----------------------------------------------------------------- Strum
class StrumFx : public IEffect {
public:
    StrumFx(double spreadMs, int dir, double windowMs, double taperPct, double randMs, unsigned seed)
        : spread_(std::max(0.0, spreadMs)), dir_(Clamp(dir, 1, 3)), win_(std::max(1.0, windowMs)),
          taper_(std::min(100.0, std::max(0.0, taperPct)) / 100.0), rand_(std::max(0.0, randMs)) { SeedRng(rng_, seed); }

    void Process(Track& t, const Song& s) override {
        std::vector<Note> notes; std::vector<Event> others;
        ExtractNotes(t, notes, others);
        std::vector<TempoSeg> tm = BuildTempoMap(s);
        std::uniform_real_distribution<double> u01(0.0, 1.0);
        bool flip = false;
        std::vector<size_t> idx;
        for (int ch = 0; ch < 16; ch++) {
            if (ch == 9) continue;
            ChannelNotes(notes, ch, idx);
            size_t i = 0;
            while (i < idx.size()) {
                const double s0 = TickToMs(tm, s.ppq, notes[idx[i]].start);
                size_t j = i + 1;
                while (j < idx.size() && TickToMs(tm, s.ppq, notes[idx[j]].start) - s0 <= win_) j++;
                const size_t m = j - i;
                if (m >= 2) {
                    std::vector<size_t> g(idx.begin() + (long)i, idx.begin() + (long)j);
                    std::sort(g.begin(), g.end(), [&](size_t a, size_t b) { return notes[a].key < notes[b].key; });
                    bool down = dir_ == 1 || (dir_ == 3 && !flip);
                    if (!down) std::reverse(g.begin(), g.end());
                    flip = !flip;
                    for (size_t k = 0; k < m; k++) {
                        Note& n = notes[g[k]];
                        const uint32_t len = n.end - n.start;
                        double delay = (double)k * spread_ + (k > 0 ? rand_ * u01(rng_) : 0.0);
                        double ms = TickToMs(tm, s.ppq, n.start) + delay;
                        uint32_t ns = MsToTick(tm, s.ppq, ms);
                        n.start = ns;
                        if (n.end <= n.start + 1) n.end = n.start + std::max<uint32_t>(2, len / 2);
                        double f = 1.0 - taper_ * (double)k / (double)(m - 1);
                        n.vel = (uint8_t)Clamp(RoundI(n.vel * f), 1, 127);
                    }
                }
                i = j;
            }
        }
        RebuildTrack(t, notes, others);
    }
private:
    double spread_; int dir_; double win_, taper_, rand_; std::mt19937 rng_;
};

// ------------------------------------------------------------------- factory
inline std::unique_ptr<IEffect> MakeEffect(int type, const double* p) {
    switch (type) {
    case FX_TRANSPOSE: return std::unique_ptr<IEffect>(new TransposeFx(RoundI(p[0]), p[1] != 0));
    case FX_QUANTIZE:  return std::unique_ptr<IEffect>(new QuantizeFx(RoundI(p[0]), p[1], p[2], p[3], RoundI(p[4]), p[5]));
    case FX_HUMANIZE:  return std::unique_ptr<IEffect>(new HumanizeFx(p[0], RoundI(p[1]), p[2], (unsigned)RoundI(p[3])));
    case FX_VELOCITY:  return std::unique_ptr<IEffect>(new VelocityFx(p[0], RoundI(p[1])));
    case FX_AUTOPHRASE: return std::unique_ptr<IEffect>(new PhraseFinderFx(p[0], p[1], p[2], p[3], RoundI(p[4])));
    case FX_HUMANCC:   return std::unique_ptr<IEffect>(new HumanControllersFx(p[0], p[1], p[2], RoundI(p[3]), p[4], p[5], RoundI(p[6]), p[7], p[8], (unsigned)RoundI(p[9])));
    case FX_BREATH:    return std::unique_ptr<IEffect>(new BreathFx(p[0], p[1], p[2], p[3], p[4], p[5], p[6], RoundI(p[7]), (unsigned)RoundI(p[8])));
    case FX_STRUM:     return std::unique_ptr<IEffect>(new StrumFx(p[0], RoundI(p[1]), p[2], p[3], p[4], (unsigned)RoundI(p[5])));
    }
    return nullptr;
}

// --------------------------------------------------------------- FX chains
enum FxRangeMode { RANGE_ALL = 0, RANGE_EXC = 1, RANGE_USE = 2 };

// One effect inside a track's chain, as shown in the "MFX Chain" dialog.
struct FxInstance {
    int    type = 0;
    double p[FX_MAXP] = {};
    bool   enabled = true;        // "Mute" column: false = bypassed
    int    mode = RANGE_ALL;      // "All" = whole track, "Exclude" = everything except From..To, "Use" = only From..To
    int    fromBar = 1;           // "From": first bar of the range (inclusive)
    int    toBar = 4;             // "To":   last bar of the range (inclusive)
    int    seed = 1;              // used when the effect's seed parameter is 0 ("auto")
};
typedef std::vector<FxInstance> FxChain;

// Runs an effect only on the part of a track whose notes start in [t0, t1) and
// whose other events lie in [t0, t1); everything else is left untouched.
inline void ProcessRange(Track& t, const Song& ctx, IEffect& fx, uint32_t t0, uint32_t t1) {
    if (t1 <= t0) return;
    std::vector<Note> notes; std::vector<Event> others;
    ExtractNotes(t, notes, others);
    std::vector<Note> inN, outN; std::vector<Event> inO, outO;
    for (const Note& n : notes) ((n.start >= t0 && n.start < t1) ? inN : outN).push_back(n);
    for (const Event& e : others) ((e.tick >= t0 && e.tick < t1) ? inO : outO).push_back(e);
    if (inN.empty() && inO.empty()) return;
    Track tmp;
    RebuildTrack(tmp, inN, inO);
    fx.Process(tmp, ctx);
    std::vector<Note> n2; std::vector<Event> o2;
    ExtractNotes(tmp, n2, o2);
    outN.insert(outN.end(), n2.begin(), n2.end());
    outO.insert(outO.end(), o2.begin(), o2.end());
    RebuildTrack(t, outN, outO);
}

// Applies a whole chain to a track (non-destructive callers work on a copy).
inline void ApplyChain(Track& t, const Song& ctx, const FxChain& chain) {
    ctx.phrase = PhraseCfg();                     // a Phrase Finder in the chain replaces these defaults
    int num, den; GetTimeSig(ctx, num, den);
    const uint32_t barTicks = (uint32_t)std::max(1, ctx.ppq * 4 / std::max(1, den) * num);
    for (const FxInstance& it : chain) {
        if (!it.enabled || it.type < 0 || it.type >= FX_COUNT) continue;
        double p[FX_MAXP];
        for (int k = 0; k < FX_MAXP; k++) p[k] = it.p[k];
        for (int k = 0; k < GetFxDesc(it.type).np; k++) p[k] = ClampParam(it.type, k, p[k]);   // never outside min / max
        const FxDesc& d = GetFxDesc(it.type);
        if (d.seedIdx >= 0 && RoundI(p[d.seedIdx]) == 0) p[d.seedIdx] = it.seed;   // stable "auto" seed
        std::unique_ptr<IEffect> fx = MakeEffect(it.type, p);
        if (!fx) continue;
        if (it.type == FX_AUTOPHRASE || it.mode == RANGE_ALL) { fx->Process(t, ctx); continue; }
        const int a = std::max(1, std::min(it.fromBar, it.toBar));      // From / To may be typed in either order
        const int b = std::max(1, std::max(it.fromBar, it.toBar));
        const uint32_t t0 = (uint32_t)(a - 1) * barTicks;
        const uint32_t t1 = (uint32_t)b * barTicks;                      // end of bar "To"
        if (it.mode == RANGE_USE) ProcessRange(t, ctx, *fx, t0, t1);
        else { ProcessRange(t, ctx, *fx, 0, t0); ProcessRange(t, ctx, *fx, t1, 0xFFFFFFFFu); }
    }
}

// Phrase settings that the chain will use (first enabled Phrase Finder, else the defaults).
inline PhraseCfg ChainPhraseCfg(const FxChain& chain) {
    for (const FxInstance& it : chain)
        if (it.enabled && it.type == FX_AUTOPHRASE) return PhraseCfgFromParams(it.p);
    return PhraseCfg();
}

// Phrase settings in force for the phrase-aware effects (Human Pitch/Mod/Expr, Breath): a Phrase Finder only
// affects the effects BELOW it, so the settings are those of the nearest enabled Phrase Finder above the last
// phrase-aware effect (the defaults when there is none) - exactly what ApplyChain hands to that effect.
inline PhraseCfg ChainPhraseCfgInOrder(const FxChain& chain) {
    PhraseCfg cur, used; bool any = false;
    for (const FxInstance& it : chain) {
        if (!it.enabled) continue;
        if (it.type == FX_AUTOPHRASE) cur = PhraseCfgFromParams(it.p);
        else if (it.type == FX_HUMANCC || it.type == FX_BREATH) { used = cur; any = true; }
    }
    return any ? used : cur;
}

} // namespace mc
