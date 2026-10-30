// player.h - Real-time MIDI playback through the Windows winmm MIDI output API.
// (c) 2026, O.Jagannatha Rao (JagiChan)
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include "midi_core.h"

#define WM_PLAYER_DONE (WM_APP + 1)

class Player {
public:
    Player() : hThread_(NULL), hOut_(NULL), stop_(false), running_(false), pos_(0.0),
               startMs_(0.0), endMs_(0.0), notify_(NULL), loopStart_(0), end_(0) {
        InitializeCriticalSection(&cs_);
    }
    ~Player() { Stop(); DeleteCriticalSection(&cs_); }

    // dev: device id, or MIDI_MAPPER.  audible[i] == false mutes track i.
    // Plays from startTick to endTick (0 = end of the song).  With loop, playback jumps back to loopStartTick.
    // While a loop is running, SetLoop(false) leaves the loop: the music then simply continues to the end of the song.
    bool Start(const mc::Song& song, UINT dev, const std::vector<bool>& audible,
               uint32_t startTick, uint32_t loopStartTick, uint32_t endTick,
               bool loop, HWND notify, std::wstring& err)
    {
        Stop();
        song_ = song; aud_ = audible; loopStart_ = loopStartTick; end_ = endTick;
        loop_ = loop; contToEnd_ = false; notify_ = notify;
        BuildSchedule(startTick);

        MMRESULT rc = midiOutOpen(&hOut_, dev, 0, 0, CALLBACK_NULL);
        if (rc != MMSYSERR_NOERROR) {
            wchar_t buf[256] = L"";
            midiOutGetErrorTextW(rc, buf, 256);
            err = std::wstring(L"Cannot open MIDI output: ") + buf;
            hOut_ = NULL;
            return false;
        }
        stop_ = false; running_ = true; pos_ = startMs_;
        hThread_ = CreateThread(NULL, 0, ThreadProc, this, 0, NULL);
        if (!hThread_) {
            midiOutClose(hOut_); hOut_ = NULL; running_ = false;
            err = L"Cannot create playback thread.";
            return false;
        }
        return true;
    }

    void Stop() {
        if (hThread_) {
            stop_ = true;
            WaitForSingleObject(hThread_, 3000);
            CloseHandle(hThread_);
            hThread_ = NULL;
        }
    }

    // Sends a short message immediately (used to change pan / volume while playing).
    void SendLive(DWORD msg) {
        EnterCriticalSection(&cs_);
        if (hOut_ && running_.load()) midiOutShortMsg(hOut_, msg);
        LeaveCriticalSection(&cs_);
    }

    // Switches looping on / off while playing.  Switching it off while the loop is running makes playback
    // continue past the loop end to the end of the song instead of stopping or jumping back.
    void SetLoop(bool on) {
        if (!on && loop_.load() && running_.load()) contToEnd_ = true;
        loop_ = on;
    }

    bool   IsPlaying() const { return running_.load(); }
    double PosMs() const     { return pos_.load(); }

private:
    struct PEv {
        double ms; int prio;
        uint32_t msg;                  // packed short message (0 + empty sysex == dead)
        std::vector<uint8_t> sysex;    // complete sysex bytes if non-empty
    };

    void BuildSchedule(uint32_t startTick) {
        pre_.clear(); sched_.clear();
        const mc::Song& song = song_;
        std::vector<mc::TempoSeg> tempo = mc::BuildTempoMap(song);

        struct Raw { uint32_t tick; int prio; size_t seq; const mc::Event* e; };
        std::vector<Raw> raw;
        size_t seq = 0;
        for (size_t ti = 0; ti < song.tracks.size(); ti++) {
            if (ti < aud_.size() && !aud_[ti]) continue;
            for (const mc::Event& e : song.tracks[ti].events)
                if (e.IsChannel() || e.IsSysex()) {
                    // everything is scheduled, also behind the end of a selection: Run() stops at the selection
                    // end - or, when the loop was switched off, simply keeps playing to the end of the song
                    raw.push_back({ e.tick, mc::Priority(e), seq++, &e });
                }
        }
        std::sort(raw.begin(), raw.end(), [](const Raw& a, const Raw& b) {
            if (a.tick != b.tick) return a.tick < b.tick;
            if (a.prio != b.prio) return a.prio < b.prio;
            return a.seq < b.seq;
        });

        startMs_ = mc::TickToMs(tempo, song.ppq, startTick);
        std::map<uint32_t, size_t> chase;     // last controller/program state before start

        for (const Raw& r : raw) {
            const mc::Event& e = *r.e;
            PEv p; p.prio = r.prio; p.msg = 0;
            if (e.IsSysex()) {
                if (e.status == 0xF0) p.sysex.push_back(0xF0);
                p.sysex.insert(p.sysex.end(), e.data.begin(), e.data.end());
                if (p.sysex.empty()) continue;
            } else {
                p.msg = (uint32_t)e.status | ((uint32_t)e.d1 << 8) | ((uint32_t)e.d2 << 16);
            }
            if (r.tick < startTick) {
                int hi = e.status & 0xF0;
                if (e.IsChannel() && (hi == 0x80 || hi == 0x90)) continue;
                p.ms = 0;
                if (e.IsChannel()) {
                    uint32_t key = e.status | (hi == 0xB0 ? ((uint32_t)e.d1 << 8) : 0u);
                    auto it = chase.find(key);
                    if (it != chase.end()) { pre_[it->second].msg = 0; pre_[it->second].sysex.clear(); }
                    chase[key] = pre_.size();
                }
                pre_.push_back(std::move(p));
            } else {
                p.ms = mc::TickToMs(tempo, song.ppq, r.tick) - startMs_;
                sched_.push_back(std::move(p));
            }
        }
        selEndMs_ = (end_ > 0) ? mc::TickToMs(tempo, song.ppq, end_) - startMs_ : -1.0;   // -1 = no selection
        endMs_ = mc::TickToMs(tempo, song.ppq, song.EndTick()) - startMs_;
        if (endMs_ < 0) endMs_ = 0;
        if (!sched_.empty()) endMs_ = std::max(endMs_, sched_.back().ms);
    }

    static DWORD WINAPI ThreadProc(LPVOID p) { static_cast<Player*>(p)->Run(); return 0; }

    void Send(const PEv& e) {
        if (!e.sysex.empty()) {
            std::vector<uint8_t> buf = e.sysex;
            MIDIHDR h; memset(&h, 0, sizeof h);
            h.lpData = (LPSTR)buf.data();
            h.dwBufferLength = (DWORD)buf.size();
            h.dwBytesRecorded = (DWORD)buf.size();
            if (midiOutPrepareHeader(hOut_, &h, sizeof h) == MMSYSERR_NOERROR) {
                if (midiOutLongMsg(hOut_, &h, sizeof h) == MMSYSERR_NOERROR) {
                    for (int i = 0; i < 500 && !(h.dwFlags & MHDR_DONE); i++) Sleep(1);
                }
                midiOutUnprepareHeader(hOut_, &h, sizeof h);
            }
        } else if (e.msg) {
            EnterCriticalSection(&cs_);
            midiOutShortMsg(hOut_, e.msg);
            LeaveCriticalSection(&cs_);
        }
    }

    void AllNotesOff() {
        for (int ch = 0; ch < 16; ch++) {
            midiOutShortMsg(hOut_, (DWORD)(0xB0 | ch) | (64u << 8));    // sustain off
            midiOutShortMsg(hOut_, (DWORD)(0xB0 | ch) | (123u << 8));   // all notes off
        }
    }

    void Run() {
        timeBeginPeriod(1);
        LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
        bool firstPass = true, again = false;
        do {
            if (!firstPass) BuildSchedule(loopStart_);               // later passes start at the loop start
            firstPass = false;
            bool hitSelEnd = false;
            for (const PEv& e : pre_) Send(e);
            LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
            auto nowMs = [&]() {
                LARGE_INTEGER t; QueryPerformanceCounter(&t);
                return (double)(t.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
            };
            size_t i = 0;
            while (!stop_ && i < sched_.size()) {
                double now = nowMs();
                pos_ = startMs_ + now;
                // end of the selection reached: stop / loop here - unless the loop was left, then play on
                if (selEndMs_ >= 0 && now >= selEndMs_ && !contToEnd_) { hitSelEnd = true; break; }
                double due = sched_[i].ms;
                if (now >= due) { Send(sched_[i]); ++i; }
                else if (due - now > 2.0) Sleep(1);
                else SwitchToThread();
            }
            while (!stop_ && !hitSelEnd) {      // wait for the end of the song / selection
                double now = nowMs();
                pos_ = startMs_ + now;
                if (selEndMs_ >= 0 && now >= selEndMs_ && !contToEnd_) { hitSelEnd = true; break; }
                if (now >= endMs_) break;
                Sleep(1);
            }
            AllNotesOff();
            again = !stop_ && loop_.load();     // a selection that is not looped is played once
        } while (again);

        EnterCriticalSection(&cs_);
        HMIDIOUT h = hOut_; hOut_ = NULL;
        LeaveCriticalSection(&cs_);
        midiOutReset(h);
        midiOutClose(h);
        timeEndPeriod(1);
        running_ = false;
        if (notify_) PostMessage(notify_, WM_PLAYER_DONE, 0, 0);
    }

    HANDLE   hThread_;
    HMIDIOUT hOut_;
    std::atomic<bool>   stop_, running_;
    std::atomic<double> pos_;
    double startMs_, endMs_, selEndMs_ = -1.0;
    std::atomic<bool> loop_{ false }, contToEnd_{ false };
    HWND   notify_;
    uint32_t loopStart_, end_;
    mc::Song song_;
    std::vector<bool> aud_;
    CRITICAL_SECTION cs_;
    std::vector<PEv> pre_, sched_;
};
