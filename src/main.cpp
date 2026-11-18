#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/Notification.hpp>
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
    // ---------------------------------------------------------------- data
    struct SpeedInfo { float val; float ups; const char* name; };
    constexpr SpeedInfo SPEEDS[] = {
        {0.7f, 251.16f, "0.5x"},
        {0.9f, 311.58f, "1x"},
        {1.1f, 387.42f, "2x"},
        {1.3f, 468.00f, "3x"},
        {1.6f, 576.00f, "4x"},
    };

    // ID mod hack da biet (tu tri nho, hay kiem tra lai tren Geode index
    // hoac them ID khac trong settings "extra-hack-ids")
    constexpr const char* HACK_IDS[] = {
        "absolllute.megahack",
        "tobyadd.gdh",
        "zilko.xdbot",
        "eclipse.eclipse-menu",
    };

    struct Cfg {
        bool enabled = true;
        float size = 5.f;
        float opacity = 1.f;
        std::string corner = "top-left";
        float tpsLimit = 240.f;
        float redSec = 5.f;
        bool checkTimescale = true;
        bool noclip = true;
        bool scanMods = true;
        bool saveReport = true;
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
        g_cfg.checkTimescale = m->getSettingValue<bool>("check-timescale");
        g_cfg.noclip = m->getSettingValue<bool>("check-noclip");
        g_cfg.scanMods = m->getSettingValue<bool>("scan-mods");
        g_cfg.saveReport = m->getSettingValue<bool>("save-report");
        g_cfg.extraIds = m->getSettingValue<std::string>("extra-hack-ids");
    }

    struct FlagInfo { float percent = 0.f; int count = 0; };
    std::map<std::string, FlagInfo> g_attemptFlags;  // luot choi hien tai
    std::map<std::string, FlagInfo> g_sessionFlags;  // ca level (tu luc vao)
    std::set<std::string> g_hackMods;
    float g_percent = 0.f;
    int g_levelId = -1;

    struct Input { int step; bool down; int button; bool p1; };
    std::vector<Input> g_inputs;
    constexpr size_t MAX_INPUTS = 30000;

    int g_steps = 0;
    int g_noclipHits = 0;
    float g_sumDt = 0.f;
    Clock::time_point g_redUntil;
    Clock::time_point g_lastUpdate;
    bool g_hasUpdate = false;

    struct Win {
        Clock::time_point t;
        SysClock::time_point sys;
        float x = 0.f, speed = 0.f, dt = 0.f;
        int steps = 0;
        bool valid = false;
    } g_win;
    int g_strikes = 0;
    int g_dtStrikes = 0;

    // ---------------------------------------------------------------- flag
    void flag(std::string const& reason) {
        log::warn("CHEAT INDICATOR: {} ({}%)", reason, g_percent);
        g_redUntil = Clock::now() +
            std::chrono::milliseconds(static_cast<int>(g_cfg.redSec * 1000.f));
        for (auto* m : {&g_attemptFlags, &g_sessionFlags}) {
            auto& f = (*m)[reason];
            if (f.count == 0) f.percent = g_percent;
            f.count++;
        }
    }

    void resetWindow(float x, float speed) {
        g_win.t = Clock::now();
        g_win.sys = SysClock::now();
        g_win.x = x;
        g_win.speed = speed;
        g_win.dt = g_sumDt;
        g_win.steps = g_steps;
        g_win.valid = true;
    }

    // ------------------------------------------------- quet mod hack dang bat
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
                flag("mod hack dang bat: " + mod->getID());
            }
        }
    }

    // ------------------------------------------------------- thong ke input
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

    // ------------------------------------------------------------- bao cao
    std::string esc(std::string s) {
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

    void writeFlags(std::ostringstream& o, std::map<std::string, FlagInfo> const& m) {
        o << "[";
        bool first = true;
        for (auto& [r, f] : m) {
            if (!first) o << ",";
            first = false;
            o << "{\"reason\":\"" << esc(r) << "\",\"percent\":" << f.percent
              << ",\"count\":" << f.count << "}";
        }
        o << "]";
    }

    void finalizeRun(PlayLayer* pl) {
        loadCfg();
        if (!g_cfg.enabled) return;

        g_percent = 100.f;
        auto st = calcStats();
        if (st.tap1 >= 15) flag("nhieu tap 1 buoc (macro/bot?)");
        if (st.presses >= 20 && st.cv < 0.02f) flag("nhip bam deu bat thuong (bot?)");

        bool invalid = pl->m_isPracticeMode || pl->m_isTestMode;
        std::string verdict = "clean";
        if (invalid) verdict = "invalid-practice-or-test";
        else if (!g_attemptFlags.empty()) verdict = "suspicious";
        else if (!g_sessionFlags.empty()) verdict = "review";

        std::string name = pl->m_level->m_levelName;
        int id = pl->m_level->m_levelID.value();
        auto now = SysClock::to_time_t(SysClock::now());

        std::ostringstream body;
        body << "\"level_id\":" << id
             << ",\"level_name\":\"" << esc(name) << "\""
             << ",\"time\":" << static_cast<long long>(now)
             << ",\"verdict\":\"" << verdict << "\""
             << ",\"attempt_flags\":";
        writeFlags(body, g_attemptFlags);
        body << ",\"session_flags\":";
        writeFlags(body, g_sessionFlags);
        body << ",\"hack_mods\":[";
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
            if (r.isErr()) log::error("Khong ghi duoc bao cao: {}", r.unwrapErr());
            else log::info("Da luu bao cao: {}", path.string());
        }

        if (verdict == "clean") {
            Notification::create("Run sach, khong phat hien gian lan", NotificationIcon::Success)->show();
        } else if (verdict == "invalid-practice-or-test") {
            Notification::create("Practice/test mode: khong tinh la run hop le", NotificationIcon::Warning)->show();
        } else {
            Notification::create(
                fmt::format("Nghi van ({}): {} canh bao trong luot choi nay",
                    verdict, g_attemptFlags.size()),
                NotificationIcon::Warning)->show();
        }
    }

    void onNewAttempt(PlayLayer* pl) {
        loadCfg();
        int id = pl->m_level->m_levelID.value();
        if (id != g_levelId) {
            g_levelId = id;
            g_sessionFlags.clear();
            g_hackMods.clear();
        }
        g_attemptFlags.clear();
        g_inputs.clear();
        g_steps = 0;
        g_noclipHits = 0;
        g_strikes = 0;
        g_dtStrikes = 0;
        g_win.valid = false;
        if (g_cfg.enabled) scanMods();
    }

    // ----------------------------------------------------------------- UI
    void drawDot(CCDrawNode* dot, bool red) {
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
        dot->drawDot({0, 0}, g_cfg.size,
            red ? ccColor4F{1.f, 0.f, 0.f, a} : ccColor4F{0.f, 1.f, 0.f, a});
    }

    CCDrawNode* getDot(PlayLayer* pl) {
        auto id = "cheat-dot"_spr;
        if (auto n = pl->m_uiLayer->getChildByID(id)) return static_cast<CCDrawNode*>(n);
        auto dot = CCDrawNode::create();
        dot->setID(id);
        pl->m_uiLayer->addChild(dot, 9999);
        drawDot(dot, false);
        return dot;
    }

    // ------------------------------------------------------------- kiem tra
    void check(PlayLayer* pl) {
        loadCfg();
        auto dot = getDot(pl);
        if (!g_cfg.enabled) { dot->setVisible(false); return; }
        dot->setVisible(true);

        auto now = Clock::now();
        // pause / lag lon: bo cua so do de khong bao nham
        bool gap = g_hasUpdate &&
            (now - g_lastUpdate) > std::chrono::milliseconds(250);
        g_lastUpdate = now;
        g_hasUpdate = true;

        g_percent = pl->getCurrentPercent();
        auto p = pl->m_player1;
        bool warped = std::fabs(pl->m_gameState.m_timeWarp - 1.f) > 0.001f;

        if (p && !p->m_isDead && !pl->m_level->isPlatformer()) {
            float x = p->getPositionX();
            float speed = p->m_playerSpeed;

            // Check 1: toc do player khong thuoc 0.5x/1x/2x/3x/4x
            const SpeedInfo* match = nullptr;
            for (auto& s : SPEEDS)
                if (std::fabs(s.val - speed) < 0.02f) match = &s;
            if (!match) flag("toc do player khong hop le");

            if (warped || gap) {
                // time warp trigger hop le lam doi toc do: khong do trong luc nay
                g_win.valid = false;
                g_strikes = 0;
                g_dtStrikes = 0;
            } else {
                // Check 2: time scale cua scheduler
                if (g_cfg.checkTimescale) {
                    float ts = CCDirector::get()->getScheduler()->getTimeScale();
                    if (std::fabs(ts - 1.f) > 0.001f) flag("time scale cua game bi doi");
                }

                if (!g_win.valid || std::fabs(speed - g_win.speed) > 1e-4f || x < g_win.x) {
                    resetWindow(x, speed);
                } else {
                    float real = std::chrono::duration<float>(now - g_win.t).count();
                    if (real >= 1.f) {
                        float dx = x - g_win.x;
                        float tps = (g_steps - g_win.steps) / real;
                        float sys = std::chrono::duration<float>(SysClock::now() - g_win.sys).count();
                        float dts = g_sumDt - g_win.dt;

                        // Check 3: TPS theo thoi gian thuc
                        if (tps > g_cfg.tpsLimit + 5.f) flag("TPS vuot gioi han");

                        // Check 4: nhieu nguon thoi gian phai khop nhau
                        if (std::fabs(sys / real - 1.f) > 0.1f)
                            flag("dong ho he thong lech dong ho steady (hack thoi gian?)");
                        if (std::fabs(dts / real - 1.f) > 0.1f) g_dtStrikes++;
                        else g_dtStrikes = 0;
                        if (g_dtStrikes >= 2)
                            flag("dt cua game lech thoi gian thuc (speedhack?)");

                        // Check 5: vat ly (quang duong / thoi gian thuc)
                        if (match && dx > 50.f) {
                            float ratio = (dx / real) / match->ups;
                            if (ratio < 0.9f || ratio > 1.1f) g_strikes++;
                            else g_strikes = 0;
                            if (g_strikes >= 2)
                                flag("vat ly khong khop toc do that (speedhack?)");
                        }
                        resetWindow(x, speed);
                    }
                }
            }
        } else {
            g_win.valid = false;
            g_strikes = 0;
            g_dtStrikes = 0;
        }

        drawDot(dot, Clock::now() < g_redUntil);
    }
}

// ------------------------------------------------------------------ hooks
class $modify(CheatBaseLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        g_steps++;
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (PlayLayer::get() && g_inputs.size() < MAX_INPUTS)
            g_inputs.push_back({g_steps, down, button, isPlayer1});
    }

    void update(float dt) {
        GJBaseGameLayer::update(dt);
        g_sumDt += dt;
        if (auto pl = PlayLayer::get()) check(pl);
    }
};

class $modify(CheatPlay, PlayLayer) {
    static void onModify(auto& self) {
        // outermost: chay phan sau-original cuoi cung, thay duoc ket qua cuoi
        (void)self.setHookPriority("PlayLayer::destroyPlayer", Priority::First);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        onNewAttempt(this);
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        bool wasDead = player->m_isDead;
        PlayLayer::destroyPlayer(player, obj);
        // Noclip thuong chan destroyPlayer: goi xong nguoi choi van song
        if (g_cfg.enabled && g_cfg.noclip && !wasDead && !player->m_isDead
            && !m_levelEndAnimationStarted) {
            g_noclipHits++;
            log::debug("destroyPlayer khong lam chet (lan {})", g_noclipHits);
            if (g_noclipHits >= 2) flag("destroyPlayer bi chan (noclip?)");
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
        // them nut o frame sau de chac chan nam sau nut cua cac mod khac
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
