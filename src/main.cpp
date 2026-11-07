#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <string>

using namespace geode::prelude;

// A "tick" = one processCommands() call (240 per second).
namespace tsm {
    struct Slot {
        bool pressPending = false;      // click held back, waiting to become usable
        bool releaseQueued = false;     // user released while the press was still pending
        bool releaseScheduled = false;  // release delayed on purpose
        bool held = false;              // the game currently thinks the button is down
        int pressStart = 0;
        int pressExpire = 0;
        int releaseDue = 0;
    };

    struct State {
        int tick = 0;
        int saves = 0;
        int lastSaved = 0;
        bool dirty = true;
        bool passthrough = false;
        std::string lastText;
        Slot slot[2];
    };

    struct Cfg {
        bool enabled = true;
        bool orbs = true;
        bool overlay = true;
        int press = 8;
        int hold = 0;
        int minHold = 2;
    };

    inline State g;

    inline Cfg readCfg() {
        auto m = Mod::get();
        Cfg c;
        c.enabled = m->getSettingValue<bool>("enabled");
        c.orbs = m->getSettingValue<bool>("use-orbs");
        c.overlay = m->getSettingValue<bool>("overlay");
        c.press = std::clamp(static_cast<int>(m->getSettingValue<int64_t>("press-window")), 0, 32);
        c.hold = std::clamp(static_cast<int>(m->getSettingValue<int64_t>("hold-extend")), 0, 16);
        c.minHold = std::clamp(static_cast<int>(m->getSettingValue<int64_t>("min-hold")), 1, 8);
        return c;
    }

    // Can a click do something right now?
    inline bool canUse(PlayerObject* p, bool orbs) {
        if (!p) return true;
        // hold/tap modes do not need buffering
        if (p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing) return true;
        if (p->m_isOnGround) return true;
        if (orbs && p->m_touchingRings && p->m_touchingRings->count() > 0) return true;
        return false;
    }
}

class $modify(TSMBaseLayer, GJBaseGameLayer) {
    bool tsmActive() {
        auto pl = PlayLayer::get();
        return pl && static_cast<GJBaseGameLayer*>(pl) == static_cast<GJBaseGameLayer*>(this);
    }

    void tsmInject(bool down, bool p1) {
        tsm::g.passthrough = true;
        this->handleButton(down, 1, p1);
        tsm::g.passthrough = false;
    }

    void tsmFlush(tsm::Cfg const& cfg) {
        auto& st = tsm::g;
        for (int i = 0; i < 2; i++) {
            auto& s = st.slot[i];
            bool p1 = (i == 0);
            auto* p = p1 ? m_player1 : m_player2;

            if (s.pressPending) {
                if (tsm::canUse(p, cfg.orbs)) {
                    s.pressPending = false;
                    st.saves++;
                    st.lastSaved = st.tick - s.pressStart;
                    st.dirty = true;
                    tsmInject(true, p1);
                    s.held = true;
                    if (s.releaseQueued) {
                        s.releaseQueued = false;
                        s.releaseScheduled = true;
                        s.releaseDue = st.tick + cfg.minHold;
                    }
                } else if (st.tick >= s.pressExpire) {
                    // window ran out: behave like vanilla
                    s.pressPending = false;
                    if (s.releaseQueued) {
                        s.releaseQueued = false;   // click + release in the air = nothing
                    } else {
                        tsmInject(true, p1);       // still holding: late press
                        s.held = true;
                    }
                }
            }

            if (s.releaseScheduled && st.tick >= s.releaseDue) {
                s.releaseScheduled = false;
                s.held = false;
                tsmInject(false, p1);
            }
        }
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto& st = tsm::g;
        if (st.passthrough || button != 1 || !tsmActive()) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        auto cfg = tsm::readCfg();
        if (!cfg.enabled) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        auto& s = st.slot[isPlayer1 ? 0 : 1];
        auto* p = isPlayer1 ? m_player1 : m_player2;

        if (down) {
            if (s.releaseScheduled) {
                // new click inside the extension window: keep holding
                s.releaseScheduled = false;
                st.saves++;
                st.lastSaved = 0;
                st.dirty = true;
                return;
            }
            if (cfg.press > 0 && !tsm::canUse(p, cfg.orbs)) {
                s.pressPending = true;
                s.releaseQueued = false;
                s.pressStart = st.tick;
                s.pressExpire = st.tick + cfg.press;
                return;
            }
            s.held = true;
            return GJBaseGameLayer::handleButton(true, button, isPlayer1);
        }

        // release
        if (s.pressPending) {
            s.releaseQueued = true;
            return;
        }
        if (cfg.hold > 0 && s.held && !s.releaseScheduled) {
            s.releaseScheduled = true;
            s.releaseDue = st.tick + cfg.hold;
            return;
        }
        s.held = false;
        return GJBaseGameLayer::handleButton(false, button, isPlayer1);
    }

    void processCommands(float dt) {
        auto& st = tsm::g;
        if (!tsmActive()) {
            return GJBaseGameLayer::processCommands(dt);
        }
        auto cfg = tsm::readCfg();
        if (cfg.enabled) {
            st.tick++;
            tsmFlush(cfg);
        }

        GJBaseGameLayer::processCommands(dt);

        if (m_uiLayer && (st.dirty || st.tick % 60 == 0)) {
            std::string text;
            if (cfg.enabled && cfg.overlay) {
                text = fmt::format(
                    "TickMix  press {}t  hold {}t  saves {} (last {}t)",
                    cfg.press, cfg.hold, st.saves, st.lastSaved
                );
            }
            if (text != st.lastText) {
                if (auto l = typeinfo_cast<CCLabelBMFont*>(m_uiLayer->getChildByID("tickmix-label"_spr))) {
                    l->setString(text.c_str());
                }
                st.lastText = text;
            }
            st.dirty = false;
        }
    }
};

class $modify(TSMPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        tsm::g = tsm::State{};
        if (!m_uiLayer) return;
        auto label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setID("tickmix-label"_spr);
        label->setScale(0.35f);
        label->setOpacity(160);
        label->setAnchorPoint({0.f, 1.f});
        auto win = CCDirector::get()->getWinSize();
        label->setPosition({6.f, win.height - 6.f});
        m_uiLayer->addChild(label, 100);
    }

    void resetLevel() {
        auto text = tsm::g.lastText;
        tsm::g = tsm::State{};
        tsm::g.lastText = text;
        PlayLayer::resetLevel();
    }
};
