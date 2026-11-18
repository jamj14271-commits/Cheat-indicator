#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace geode::prelude;
using Clock = std::chrono::steady_clock;
using SysClock = std::chrono::system_clock;

namespace {
    // ================================================================ data
    struct SpeedInfo { float val; float ups; const char* name; };
    constexpr SpeedInfo SPEEDS[] = {
        {0.7f, 251.16f, "0.5x"},
        {0.9f, 311.58f, "1x"},
        {1.1f, 387.42f, "2x"},
        {1.3f, 468.00f, "3x"},
        {1.6f, 576.00f, "4x"},
    };

    // Known cheat mod IDs (from memory; verify on the Geode index or add
    // more in the "extra-hack-ids" setting).
    constexpr const char* HACK_IDS[] = {
        "absolllute.megahack",
        "tobyadd.gdh",
        "zilko.xdbot",
        "eclipse.eclipse-menu",
    };

    constexpr int TIME_WARP_TRIGGER_ID = 1935;

    struct Cfg {
        bool enabled = true;
        float size = 5.f;
        float opacity = 1.f;
        std::string corner = "top-left";
        float tpsLimit = 240.f;
        float redSec = 5.f;
        float yellowSec = 3.f;
        bool checkTimescale = true;
        bool noclip = true;
        bool scanMods = true;
        bool saveReport = true;
        bool debug = false;
        std::string extraIds;
    } g_cfg;

    void loadCfg() {
        auto m = Mod::get();
        g_cfg.enabled = m->getSettingValue<bool>("enabled");
        g_cfg.size = static_cast<float>(m->getSettingValue<double>("dot-size"));
        g_cfg.opacity = static_cast<float>(m->getSettingValue<double>("dot-opacity"));
        g_cfg.corner = m->getSettingValue<std::string>("dot-corner");
        g_cfg.tpsLimit = static_cast<float>(m->getSettingValue<double>("tps-limit"));
        g_cfg.redSec = static_cast<float>(m->getSettingValue<double>("red-seconds"));
        g_cfg.yellowSec = static_cast<float>(m->getSettingValue<double>("yellow-seconds"));
        g_cfg.checkTimescale = m->getSettingValue<bool>("check-timescale");
        g_cfg.noclip = m->getSettingValue<bool>("check-noclip");
        g_cfg.scanMods = m->getSettingValue<bool>("scan-mods");
        g_cfg.saveReport = m->getSettingValue<bool>("save-report");
        g_cfg.debug = m->getSettingValue<bool>("debug-log");
        g_cfg.extraIds = m->getSettingValue<std::string>("extra-hack-ids");
    }

    // ============================================================== events
    // Green = nothing suspicious, Yellow = anomaly / not enough evidence,
    // Red = high-confidence violation.
    enum class Sev { None = 0, Yellow = 1, Red = 2 };

    struct Rec { Sev sev = Sev::None; float percent = 0.f; int count = 0; std::string det; };
    struct Active { Sev sev = Sev::None; Clock::time_point until; };

    std::map<std::string, Rec> g_attempt;    // this attempt (goes in the report)
    std::map<std::string, Rec> g_session;    // whole level session
    std::map<std::string, Active> g_active;  // drives the light, expires over time
    std::set<std::string> g_hackMods;

    float g_percent = 0.f;
    int g_levelId = -1;
    bool g_levelHasTimeWarp = false;

    void raise(std::string const& det, std::string const& reason, Sev sev) {
        std::string key = det + ": " + reason;
        float secs = sev == Sev::Red ? g_cfg.redSec : g_cfg.yellowSec;
        auto until = Clock::now() + std::chrono::milliseconds(static_cast<int>(secs * 1000.f));
        auto& a = g_active[key];
        if (static_cast<int>(sev) > static_cast<int>(a.sev)) a.sev = sev;
        a.until = until;
        for (auto* m : {&g_attempt, &g_session}) {
            auto& r = (*m)[key];
            if (r.count == 0) r.percent = g_percent;
            r.count++;
            r.det = det;
            if (static_cast<int>(sev) > static_cast<int>(r.sev)) r.sev = sev;
        }
        log::warn("[CI] {} {} ({}%)", sev == Sev::Red ? "RED" : "YELLOW", key, g_percent);
    }

    Sev currentLight() {
        auto now = Clock::now();
        Sev s = Sev::None;
        for (auto it = g_active.begin(); it != g_active.end();) {
            if (it->second.until <= now) {
                it = g_active.erase(it);
            } else {
                if (static_cast<int>(it->second.sev) > static_cast<int>(s)) s = it->second.sev;
                ++it;
            }
        }
        return s;
    }

    bool hasSev(std::map<std::string, Rec> const& m, Sev s) {
        for (auto& [k, r] : m) if (r.sev == s) return true;
        return false;
    }

    // ======================================================== detector state
    // Each detector owns its own counters. Nothing here is shared between
    // detectors, so one detector cannot reset or disable another.
    int g_steps = 0;            // processCommands calls
    int g_frames = 0;           // update() calls
    int g_halfSteps = 0;        // half ticks (debug only)
    float g_stepDtSum = 0.f;    // sum of the dt given to processCommands
    float g_sumDt = 0.f;        // sum of the dt given to update()
    Clock::time_point g_lastUpdate;
    bool g_hasUpdate = false;
    Clock::time_point g_attemptStart;

    struct TimeWin {  // used by TPS + clock detectors
        Clock::time_point t;
        SysClock::time_point sys;
        int steps = 0, frames = 0, half = 0;
        float stepDt = 0.f, dt = 0.f;
        float tw = 1.f, ts = 1.f;
        bool valid = false;
    } g_timeWin;

    struct MotionWin {  // used by physics detector
        Clock::time_point t;
        float x = 0.f, speed = 0.f, expect = 1.f;
        bool valid = false;
    } g_motionWin;

    int g_badSpeedFrames = 0;   // speed detector
    int g_physStrikes = 0;      // physics detector
    int g_tpsStrikes = 0;       // TPS detector
    int g_clockStrikes = 0;     // clock detector
    int g_scaleFrames = 0;      // time-scale detector
    int g_mismatchFrames = 0;   // time-scale detector (warp mismatch)
    int g_noclipHits = 0;       // noclip detector

    struct Input { int step; bool down; int button; bool p1; };
    std::vector<Input> g_inputs;
    constexpr size_t MAX_INPUTS = 30000;

    // ------------------------------------------------------ suspicious mods
    void scanMods() {
        if (!g_cfg.scanMods) return;
        std::set<std::string> ids(std::begin(HACK_IDS), std::end(HACK_IDS));
        for (auto& s : utils::string::split(g_cfg.extraIds, ",")) {
            if (!s.empty()) ids.insert(s);
        }
        for (auto mod : Loader::get()->getAllMods()) {
            if (!mod->isLoaded()) continue;
            if (ids.count(mod->getID())) {
                g_hackMods.insert(mod->getID());
                // A loaded mod is NOT proof of cheating: Yellow only.
                raise("Mods", "suspicious mod loaded: " + mod->getID(), Sev::Yellow);
            }
        }
    }

    void scanTimeWarp(PlayLayer* pl) {
        if (g_levelHasTimeWarp || !pl->m_objects) return;
        for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
            if (obj && obj->m_objectID == TIME_WARP_TRIGGER_ID) {
                g_levelHasTimeWarp = true;
                break;
            }
        }
    }

    // ======================================================== click analysis
    struct Stats { int presses = 0; int tap1 = 0; float cv = 1.f; };

    Stats calcStats() {
        Stats st;
        std::map<int, int> lastDown;
        std::vector<int> pressSteps;
        for (auto& in : g_inputs) {
            int key = (in.p1 ? 0 : 100) + in.button;
            if (in.down) {
                lastDown[key] = in.step;
                if (in.button == 1) pressSteps.push_back(in.step);
                st.presses++;
            } else if (lastDown.count(key)) {
                if (in.step - lastDown[key] <= 1) st.tap1++;
                lastDown.erase(key);
            }
        }
        if (pressSteps.size() >= 3) {
            std::vector<float> iv;
            for (size_t i = 1; i < pressSteps.size(); i++)
                iv.push_back(static_cast<float>(pressSteps[i] - pressSteps[i - 1]));
            float mean = 0.f;
            for (float v : iv) mean += v;
            mean /= iv.size();
            float var = 0.f;
            for (float v : iv) var += (v - mean) * (v - mean);
            var /= iv.size();
            if (mean > 0.f) st.cv = std::sqrt(var) / mean;
        }
        return st;
    }

    // ================================================================ report
    std::string esc(std::string const& s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') o += '\\';
            if (c == '\n' || c == '\r') continue;
            o += c;
        }
        return o;
    }

    uint64_t fnv(std::string const& s) {
        uint64_t h = 1469598103934665603ULL;
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
        return h;
    }

    void writeFlags(std::ostringstream& o, std::map<std::string, Rec> const& m) {
        o << "[";
        bool first = true;
        for (auto& [k, r] : m) {
            if (!first) o << ",";
            first = false;
            o << "{\"detector\":\"" << esc(r.det) << "\",\"reason\":\"" << esc(k)
              << "\",\"level\":\"" << (r.sev == Sev::Red ? "red" : "yellow")
              << "\",\"percent\":" << r.percent << ",\"count\":" << r.count << "}";
        }
        o << "]";
    }

    void finalizeRun(PlayLayer* pl) {
        loadCfg();
        if (!g_cfg.enabled) return;
        g_percent = 100.f;

        // Click detector (evaluated once, on completion)
        auto st = calcStats();
        bool tapBad = st.tap1 >= 15;
        bool cvBad = st.presses >= 20 && st.cv < 0.02f;
        if (tapBad) raise("Clicks", "many 1-step taps (macro/bot?)", Sev::Yellow);
        if (cvBad) raise("Clicks", "unnaturally regular click rhythm (bot?)", Sev::Yellow);
        if (tapBad && cvBad) raise("Clicks", "bot-like click pattern", Sev::Red);

        bool invalid = pl->m_isPracticeMode || pl->m_isTestMode;
        std::string verdict = "clean";
        if (invalid) verdict = "invalid-practice-or-test";
        else if (hasSev(g_attempt, Sev::Red)) verdict = "suspicious";
        else if (!g_attempt.empty() || !g_session.empty() || !g_hackMods.empty()) verdict = "review";

        std::string name = pl->m_level->m_levelName;
        int id = pl->m_level->m_levelID.value();
        auto now = SysClock::to_time_t(SysClock::now());

        std::ostringstream body;
        body << "\"level_id\":" << id
             << ",\"level_name\":\"" << esc(name) << "\""
             << ",\"time\":" << static_cast<long long>(now)
             << ",\"verdict\":\"" << verdict << "\""
             << ",\"level_has_time_warp\":" << (g_levelHasTimeWarp ? "true" : "false")
             << ",\"attempt_flags\":";
        writeFlags(body, g_attempt);
        body << ",\"session_flags\":";
        writeFlags(body, g_session);
        body << ",\"suspicious_mods\":[";
        bool first = true;
        for (auto& m : g_hackMods) {
            if (!first) body << ",";
            first = false;
            body << "\"" << esc(m) << "\"";
        }
        body << "],\"presses\":" << st.presses
             << ",\"tap1\":" << st.tap1
             << ",\"interval_cv\":" << st.cv
             << ",\"steps\":" << g_steps
             << ",\"inputs\":\"";
        for (auto& in : g_inputs)
            body << in.step << "," << (in.down ? 1 : 0) << "," << in.button << ","
                 << (in.p1 ? 1 : 0) << ";";
        body << "\"";

        std::string content = body.str();
        std::string sig = fmt::format("{:016x}", fnv("cheat-indicator-v1" + content));
        std::string json = "{" + content + ",\"sig\":\"" + sig + "\"}";

        if (g_cfg.saveReport) {
            auto dir = Mod::get()->getSaveDir() / "reports";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            auto path = dir / fmt::format("{}_{}.json", id, static_cast<long long>(now));
            auto r = utils::file::writeString(path, json);
            if (r.isErr()) log::error("Could not write report: {}", r.unwrapErr());
            else log::info("Report saved: {}", path.string());
        }

        if (verdict == "clean") {
            Notification::create("Clean run: no suspicious activity detected", NotificationIcon::Success)->show();
        } else if (verdict == "invalid-practice-or-test") {
            Notification::create("Practice/test mode: not a valid run", NotificationIcon::Warning)->show();
        } else {
            Notification::create(
                fmt::format("Run flagged ({}): {} warning(s) this attempt",
                    verdict, g_attempt.size()),
                NotificationIcon::Warning)->show();
        }
    }

    // ================================================================= reset
    void resetWindows() {
        g_timeWin.valid = false;
        g_motionWin.valid = false;
        g_physStrikes = 0;
        g_tpsStrikes = 0;
        g_clockStrikes = 0;
    }

    void onNewAttempt(PlayLayer* pl) {
        loadCfg();
        int id = pl->m_level->m_levelID.value();
        bool newLevel = id != g_levelId;
        if (newLevel) {
            g_levelId = id;
            g_session.clear();
            g_hackMods.clear();
            g_levelHasTimeWarp = false;
        }
        // Evidence belongs to the attempt that produced it: the light and the
        // per-attempt counters start clean. The report keeps session history.
        g_attempt.clear();
        g_active.clear();
        g_inputs.clear();
        g_steps = 0;
        g_halfSteps = 0;
        g_noclipHits = 0;
        g_badSpeedFrames = 0;
        g_scaleFrames = 0;
        g_mismatchFrames = 0;
        g_hasUpdate = false;
        g_attemptStart = Clock::now();
        resetWindows();
        scanTimeWarp(pl);
        // Mod scan runs once per level session, never on every attempt.
        if (g_cfg.enabled && newLevel) scanMods();
    }

    // ==================================================================== UI
    void drawDot(CCDrawNode* dot, Sev sev) {
        auto win = CCDirector::get()->getWinSize();
        float m = g_cfg.size + 7.f;
        float x = m, y = win.height - m;
        auto const& c = g_cfg.corner;
        if (c == "top-right") { x = win.width - m; y = win.height - m; }
        else if (c == "bottom-left") { x = m; y = m; }
        else if (c == "bottom-right") { x = win.width - m; y = m; }
        dot->setPosition({x, y});
        dot->clear();
        float a = g_cfg.opacity;
        ccColor4F col = {0.f, 1.f, 0.f, a};
        if (sev == Sev::Yellow) col = {1.f, 0.85f, 0.f, a};
        else if (sev == Sev::Red) col = {1.f, 0.f, 0.f, a};
        dot->drawDot({0, 0}, g_cfg.size, col);
    }

    CCDrawNode* getDot(PlayLayer* pl) {
        auto id = "cheat-dot"_spr;
        if (auto n = pl->m_uiLayer->getChildByID(id)) return static_cast<CCDrawNode*>(n);
        auto dot = CCDrawNode::create();
        dot->setID(id);
        pl->m_uiLayer->addChild(dot, 9999);
        drawDot(dot, Sev::None);
        return dot;
    }

    // ============================================================ detectors
    // 1) Speed detector: player speed must be one of 0.5x/1x/2x/3x/4x for a
    //    sustained number of frames (single glitch frames are ignored).
    void detectSpeed(bool active, SpeedInfo const* match) {
        if (!active || match) { g_badSpeedFrames = 0; return; }
        g_badSpeedFrames++;
        if (g_badSpeedFrames == 15) raise("Speed", "player speed is not 0.5x/1x/2x/3x/4x", Sev::Yellow);
        else if (g_badSpeedFrames % 60 == 0) raise("Speed", "player speed is not 0.5x/1x/2x/3x/4x", Sev::Red);
    }

    // 2) Time-scale detector: game speed may only change if the level really
    //    contains a Time Warp trigger. Checked from BOTH m_timeWarp and the
    //    scheduler, and never skipped just because m_timeWarp != 1.
    void detectTimeScale(float tw, float ts) {
        if (!g_cfg.checkTimescale) { g_scaleFrames = 0; g_mismatchFrames = 0; return; }
        bool changed = std::fabs(tw - 1.f) > 0.001f || std::fabs(ts - 1.f) > 0.001f;
        if (changed && !g_levelHasTimeWarp) {
            g_scaleFrames++;
            if (g_scaleFrames == 10 || g_scaleFrames % 120 == 0)
                raise("TimeScale",
                    fmt::format("game speed changed (timeWarp {:.2f}, scheduler {:.2f}) but level has no Time Warp trigger", tw, ts),
                    Sev::Red);
        } else {
            g_scaleFrames = 0;
        }
        if (g_levelHasTimeWarp && std::fabs(tw - ts) > 0.02f) {
            g_mismatchFrames++;
            if (g_mismatchFrames == 30)
                raise("TimeScale", "timeWarp and scheduler time scale disagree", Sev::Yellow);
        } else {
            g_mismatchFrames = 0;
        }
    }

    // 3) Timing window: TPS detector + clock detector (independent counters).
    void detectTiming(Clock::time_point now, float tw, float ts, float expect, bool active) {
        auto& w = g_timeWin;
        bool changed = std::fabs(tw - w.tw) > 1e-4f || std::fabs(ts - w.ts) > 1e-4f;
        if (!active || !w.valid || changed) {
            if (active) {
                w.t = now; w.sys = SysClock::now();
                w.steps = g_steps; w.frames = g_frames; w.half = g_halfSteps;
                w.stepDt = g_stepDtSum; w.dt = g_sumDt;
                w.tw = tw; w.ts = ts; w.valid = true;
            } else {
                w.valid = false;
            }
            return;
        }
        float real = std::chrono::duration<float>(now - w.t).count();
        if (real < 1.f) return;

        float sys = std::chrono::duration<float>(SysClock::now() - w.sys).count();
        float dts = g_sumDt - w.dt;
        int calls = g_steps - w.steps;
        int frames = std::max(1, g_frames - w.frames);
        float stepsPerFrame = static_cast<float>(calls) / frames;
        float fps = frames / real;
        float tpsCalls = calls / real;
        float avgStepDt = calls > 0 ? (g_stepDtSum - w.stepDt) / calls : 0.f;
        float tpsDelta = avgStepDt > 0.f ? 1.f / avgStepDt : 0.f;
        // FPS is NOT the physics rate. If the step function runs several times
        // per frame we count steps; otherwise we derive TPS from the step delta.
        float tps = stepsPerFrame > 1.5f ? tpsCalls : tpsDelta;
        float tpsNorm = tps / std::max(expect, 0.05f);

        if (g_cfg.debug) {
            log::info("[CI] fps={:.1f} steps/s={:.1f} steps/frame={:.2f} halfTicks/s={:.1f} stepDt={:.5f} tps={:.1f} tpsNorm={:.1f} expect={:.2f} ts={:.2f} sys/real={:.3f} dt/real={:.3f}",
                fps, tpsCalls, stepsPerFrame, (g_halfSteps - w.half) / real, avgStepDt, tps, tpsNorm, expect, ts, sys / real, dts / real);
        }

        // TPS detector
        float lim = g_cfg.tpsLimit;
        if (tpsNorm > lim * 1.03f) {
            g_tpsStrikes++;
            raise("TPS", fmt::format("physics rate above {:.0f} TPS", lim),
                g_tpsStrikes >= 2 ? Sev::Red : Sev::Yellow);
        } else if (tpsNorm > lim * 1.01f) {
            g_tpsStrikes = 0;
            raise("TPS", fmt::format("physics rate slightly above {:.0f} TPS", lim), Sev::Yellow);
        } else {
            g_tpsStrikes = 0;
        }

        // Clock detector: independent time sources must agree
        bool sysBad = std::fabs(sys / real - 1.f) > 0.1f;
        bool dtBad = std::fabs(dts / real - ts) > 0.1f;
        if (sysBad || dtBad) {
            g_clockStrikes++;
            raise("Clock",
                sysBad ? "system clock disagrees with monotonic clock (time hack?)"
                       : "game delta time disagrees with real time (speedhack?)",
                g_clockStrikes >= 2 ? Sev::Red : Sev::Yellow);
        } else {
            g_clockStrikes = 0;
        }

        w.t = now; w.sys = SysClock::now();
        w.steps = g_steps; w.frames = g_frames; w.half = g_halfSteps;
        w.stepDt = g_stepDtSum; w.dt = g_sumDt;
        w.tw = tw; w.ts = ts;
    }

    // 4) Physics detector: real distance covered per real second must match
    //    the speed the player claims, scaled by the *legitimate* time warp
    //    (1.0 unless the level has a Time Warp trigger). A hack that sets
    //    m_timeWarp in a level without such a trigger is therefore caught.
    void detectMotion(Clock::time_point now, float x, float speed, float expect,
                      SpeedInfo const* match) {
        auto& w = g_motionWin;
        if (!match) { w.valid = false; g_physStrikes = 0; return; }
        if (!w.valid || std::fabs(speed - w.speed) > 1e-4f || x < w.x
            || std::fabs(expect - w.expect) > 1e-4f) {
            w.t = now; w.x = x; w.speed = speed; w.expect = expect; w.valid = true;
            return;
        }
        float real = std::chrono::duration<float>(now - w.t).count();
        if (real < 1.f) return;
        float dx = x - w.x;
        if (dx > 50.f) {
            float ratio = ((dx / real) / match->ups) / std::max(expect, 0.05f);
            if (g_cfg.debug) log::info("[CI] physics ratio={:.3f} speed={}", ratio, match->name);
            if (ratio < 0.9f || ratio > 1.1f) {
                g_physStrikes++;
                raise("Physics",
                    fmt::format("distance per real second is {:.0f}% of expected", ratio * 100.f),
                    g_physStrikes >= 2 ? Sev::Red : Sev::Yellow);
            } else {
                g_physStrikes = 0;
            }
        }
        w.t = now; w.x = x;
    }

    // ============================================================== per frame
    void check(PlayLayer* pl) {
        loadCfg();
        auto dot = getDot(pl);
        if (!g_cfg.enabled) { dot->setVisible(false); return; }
        dot->setVisible(true);

        auto now = Clock::now();
        // Pause/resume or a big lag spike: drop timing windows, keep the rest.
        bool gap = g_hasUpdate && (now - g_lastUpdate) > std::chrono::milliseconds(250);
        g_lastUpdate = now;
        g_hasUpdate = true;
        if (gap) resetWindows();

        g_percent = pl->getCurrentPercent();
        bool grace = (now - g_attemptStart) < std::chrono::milliseconds(750);

        auto p = pl->m_player1;
        bool alive = p && !p->m_isDead;
        bool platformer = pl->m_level->isPlatformer();
        float tw = pl->m_gameState.m_timeWarp;
        float ts = CCDirector::get()->getScheduler()->getTimeScale();
        float expect = g_levelHasTimeWarp ? tw : 1.f;

        if (alive && !platformer && !grace) {
            float speed = p->m_playerSpeed;
            SpeedInfo const* match = nullptr;
            for (auto& s : SPEEDS)
                if (std::fabs(s.val - speed) < 0.02f) match = &s;

            detectSpeed(true, match);
            detectTimeScale(tw, ts);
            if (!gap) {
                detectTiming(now, tw, ts, expect, true);
                detectMotion(now, p->getPositionX(), speed, expect, match);
            }
        } else {
            detectSpeed(false, nullptr);
            detectTimeScale(1.f, 1.f);
            detectTiming(now, tw, ts, expect, false);
            g_motionWin.valid = false;
        }

        drawDot(dot, currentLight());
    }
}

// ===================================================================== hooks
class $modify(CheatBaseLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        // Count only full ticks as physics steps; half ticks are tracked
        // separately so they can never inflate the TPS reading.
        if (isHalfTick) {
            g_halfSteps++;
        } else {
            g_steps++;
            g_stepDtSum += dt;
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (PlayLayer::get() && g_inputs.size() < MAX_INPUTS)
            g_inputs.push_back({g_steps, down, button, isPlayer1});
    }

    void update(float dt) {
        GJBaseGameLayer::update(dt);
        g_frames++;
        g_sumDt += dt;
        if (auto pl = PlayLayer::get()) check(pl);
    }
};

class $modify(CheatPlay, PlayLayer) {
    static void onModify(auto& self) {
        // Outermost hook: sees the final result after other mods ran.
        (void)self.setHookPriority("PlayLayer::destroyPlayer", Priority::First);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        onNewAttempt(this);
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        bool wasDead = player->m_isDead;
        PlayLayer::destroyPlayer(player, obj);
        // Noclip detector: destroyPlayer ran but the player is still alive.
        if (g_cfg.enabled && g_cfg.noclip && !wasDead && !player->m_isDead
            && !m_levelEndAnimationStarted) {
            g_noclipHits++;
            if (g_cfg.debug) log::info("[CI] destroyPlayer did not kill ({})", g_noclipHits);
            if (g_noclipHits >= 5) raise("Noclip", "destroyPlayer is being blocked", Sev::Red);
            else if (g_noclipHits >= 2) raise("Noclip", "destroyPlayer did not kill the player", Sev::Yellow);
        }
    }

    void levelComplete() {
        PlayLayer::levelComplete();
        finalizeRun(this);
    }
};

class $modify(CheatPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        // Add the button one frame later so it lands after other mods' buttons.
        Ref<CheatPause> self = this;
        Loader::get()->queueInMainThread([self] {
            if (self->getParent()) self->addCheatButton();
        });
    }

    void addCheatButton() {
        auto menu = this->getChildByID("left-button-menu");
        if (!menu) {
            auto win = CCDirector::get()->getWinSize();
            auto m = CCMenu::create();
            m->setID("left-button-menu");
            m->setPosition({30.f, win.height / 2.f});
            m->setContentSize({50.f, win.height - 60.f});
            m->setLayout(
                ColumnLayout::create()
                    ->setAxisReverse(true)
                    ->setAxisAlignment(AxisAlignment::End)
                    ->setGap(6.f)
            );
            this->addChild(m);
            menu = m;
        }
        auto spr = CCSprite::createWithSpriteFrameName("GJ_optionsBtn02_001.png");
        spr->setScale(0.75f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(CheatPause::onCheatSettings));
        btn->setID("cheat-indicator-settings"_spr);
        menu->addChild(btn);
        menu->updateLayout();
    }

    void onCheatSettings(CCObject*) {
        openSettingsPopup(Mod::get(), false);
    }
};
