#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/ui/Notification.hpp>

#include <algorithm>
#include <array>
#include <unordered_map>

using namespace geode::prelude;

namespace {
    constexpr size_t kButtonBuckets = 16;
    enum class EdgeKind { Press, Release };

    EdgeKind opposite(EdgeKind kind) {
        return kind == EdgeKind::Press ? EdgeKind::Release : EdgeKind::Press;
    }

    struct Channel {
        EdgeKind lastEmitted = EdgeKind::Release;
        int queuedEdges = 0;
        PlayerButton button = PlayerButton::Jump;
        double lockedUntil = 0.0;
    };

    struct PlayerState {
        std::array<Channel, kButtonBuckets> channels {};
        double sharedLockedUntil = 0.0;
    };

    struct Config {
        bool enabled = false;
        bool perButton = true;
        double holdLockout = 0.0;
        double gapLockout = 0.0;
    };

    std::unordered_map<PlayerObject*, PlayerState> g_states;
    double g_gameTime = 0.0;
    bool g_wasCappedThisAttempt = false;
    bool g_emittingDeferred = false;

    size_t buttonBucket(PlayerButton button) {
        auto raw = static_cast<int>(button);
        if (raw < 0) {
            raw = -raw;
        }
        return static_cast<size_t>(raw) % kButtonBuckets;
    }

    Config currentConfig() {
        auto mod = Mod::get();
        Config cfg;
        cfg.enabled = mod->getSettingValue<bool>("enabled");
        cfg.perButton = mod->getSettingValue<bool>("per-button");

        auto maxCps = std::max(0.1, mod->getSettingValue<double>("max-cps"));
        auto holdFraction = std::clamp(
            mod->getSettingValue<double>("hold-fraction"), 0.1, 0.9
        );
        auto cycle = 1.0 / maxCps;
        cfg.holdLockout = cycle * holdFraction;
        cfg.gapLockout = cycle * (1.0 - holdFraction);
        return cfg;
    }

    void resetIfConfigChanged(Config const& cfg) {
        static double lastHold = 0.0;
        static double lastGap = 0.0;
        static bool lastPerButton = false;
        if (cfg.holdLockout != lastHold || cfg.gapLockout != lastGap
            || cfg.perButton != lastPerButton) {
            lastHold = cfg.holdLockout;
            lastGap = cfg.gapLockout;
            lastPerButton = cfg.perButton;
            g_states.clear();
        }
    }

    Channel& channelFor(PlayerState& state, PlayerButton button, bool perButton) {
        return state.channels[perButton ? buttonBucket(button) : 0];
    }

    double& lockRef(PlayerState& state, Channel& ch, bool perButton) {
        return perButton ? ch.lockedUntil : state.sharedLockedUntil;
    }
    double lockoutFor(EdgeKind kind, Config const& cfg) {
        return kind == EdgeKind::Press ? cfg.holdLockout : cfg.gapLockout;
    }
    bool handleEdge(PlayerObject* player, PlayerButton button, EdgeKind kind) {
        if (g_emittingDeferred) {
            return true;
        }
        auto cfg = currentConfig();
        if (!cfg.enabled) {
            return true;
        }
        resetIfConfigChanged(cfg);

        auto now = g_gameTime;
        auto& state = g_states[player];
        auto& ch = channelFor(state, button, cfg.perButton);
        auto& lock = lockRef(state, ch, cfg.perButton);

        if (ch.queuedEdges == 0 && now >= lock) {
            ch.lastEmitted = kind;
            ch.button = button;
            lock = now + lockoutFor(kind, cfg);
            return true;
        }

        auto effectiveLast = ch.queuedEdges == 1
            ? opposite(ch.lastEmitted)
            : ch.lastEmitted;

        g_wasCappedThisAttempt = true;
        if (kind == effectiveLast) {

            return false;
        }   
        if (ch.queuedEdges == 0) {
            ch.queuedEdges = 1;
            ch.button = button;
        }
        else {
            ch.queuedEdges = 0;
        }
        return false;
    }

    void flushPlayer(PlayerObject* player, Config const& cfg, double now) {
        auto it = g_states.find(player);
        if (it == g_states.end()) {
            return;
        }
        auto& state = it->second;

        size_t buckets = cfg.perButton ? kButtonBuckets : 1;
        for (size_t i = 0; i < buckets; ++i) {
            auto& ch = state.channels[i];
            auto& lock = lockRef(state, ch, cfg.perButton);
            while (ch.queuedEdges > 0 && now >= lock) {
                auto fireTime = lock;
                auto kind = opposite(ch.lastEmitted);
                ch.lastEmitted = kind;
                ch.queuedEdges -= 1;
                lock = fireTime + lockoutFor(kind, cfg);

                g_emittingDeferred = true;
                if (kind == EdgeKind::Press) {
                    player->pushButton(ch.button);
                }
                else {
                    player->releaseButton(ch.button);
                }
                g_emittingDeferred = false;
            }
        }
    }
}

class $modify(CappedBaseLayer, GJBaseGameLayer) {
    void update(float dt) {
        g_gameTime += dt;
        auto cfg = currentConfig();
        if (cfg.enabled && !g_states.empty()) {
            resetIfConfigChanged(cfg);
            if (m_player1) flushPlayer(m_player1, cfg, g_gameTime);
            if (m_player2) flushPlayer(m_player2, cfg, g_gameTime);
        }
        GJBaseGameLayer::update(dt);
    }
};

class $modify(CappedPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        g_wasCappedThisAttempt = false;
        g_states.clear();
        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    void levelComplete() {
        PlayLayer::levelComplete();

        if (g_wasCappedThisAttempt) {
            Notification::create("CPS capped", NotificationIcon::Info)->show();
            g_wasCappedThisAttempt = false;
        }
    }

    void resetLevel() {
        g_wasCappedThisAttempt = false;
        g_states.clear();
        PlayLayer::resetLevel();
    }

    void resetLevelFromStart() {
        g_wasCappedThisAttempt = false;
        g_states.clear();
        PlayLayer::resetLevelFromStart();
    }

    void onQuit() {
        g_wasCappedThisAttempt = false;
        g_states.clear();
        PlayLayer::onQuit();
    }
};

class $modify(CappedPlayerObject, PlayerObject) {
    bool pushButton(PlayerButton button) {
        if (!handleEdge(this, button, EdgeKind::Press)) {
            return false;
        }
        return PlayerObject::pushButton(button);
    }

    bool releaseButton(PlayerButton button) {
        if (!handleEdge(this, button, EdgeKind::Release)) {
            return false;
        }
        return PlayerObject::releaseButton(button);
    }
};
