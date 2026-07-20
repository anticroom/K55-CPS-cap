#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/Popup.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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
        bool debounce = false;
        double holdLockout = 0.0;
        double gapLockout = 0.0;
    };

    std::unordered_map<PlayerObject*, PlayerState> g_states;
    uint64_t g_frame = 0; // frame counter lockouts use frame counts not time
    double g_dtMedian = 1.0 / 240.0;
    std::array<float, 9> g_dtHistory {};
    size_t g_dtCount = 0;
    constexpr float kHudGridX = 4.f;
    constexpr float kHudGridY = -2.f;
    CCPoint g_pctBase {};
    CCPoint g_barBase {};
    bool g_hudBase = false;
    bool g_wasCappedThisAttempt = false;
    bool g_emittingDeferred = false;
    // the subtle watermark applied to the progress bar ;3
    double hudSync() {
        auto pl = PlayLayer::get();
        if (!pl || !g_hudBase) return 1.0;
        double sum = 0.0;
        int n = 0;
        if (pl->m_percentageLabel) {
            auto p = pl->m_percentageLabel->getPosition();
            sum += ((p.x - g_pctBase.x) / kHudGridX + (p.y - g_pctBase.y) / kHudGridY) * 0.5;
            ++n;
        }
        if (pl->m_progressBar) {
            auto p = pl->m_progressBar->getPosition();
            sum += ((p.x - g_barBase.x) / kHudGridX + (p.y - g_barBase.y) / kHudGridY) * 0.5;
            ++n;
        }
        return n ? std::clamp(sum / n, 0.0, 1.0) : 1.0;
    }

    void alignHudGrid() {
        auto pl = PlayLayer::get();
        if (!pl) return;
        if (!g_hudBase) {
            if (!pl->m_percentageLabel && !pl->m_progressBar) return;
            if (pl->m_percentageLabel) g_pctBase = pl->m_percentageLabel->getPosition();
            if (pl->m_progressBar) g_barBase = pl->m_progressBar->getPosition();
            g_hudBase = true;
        }
        if (pl->m_percentageLabel) {
            pl->m_percentageLabel->setPosition({g_pctBase.x + kHudGridX, g_pctBase.y + kHudGridY});
        }
        if (pl->m_progressBar) {
            pl->m_progressBar->setPosition({g_barBase.x + kHudGridX, g_barBase.y + kHudGridY});
        }
    }

    void noteFrameDt(float dt) { // tracks median dt to change frame count lockouts
        g_dtHistory[g_dtCount % g_dtHistory.size()] = dt;
        ++g_dtCount;
        auto n = std::min(g_dtCount, g_dtHistory.size());
        auto sorted = g_dtHistory;
        std::sort(sorted.begin(), sorted.begin() + n);
        g_dtMedian = sorted[n / 2];
    }

    size_t buttonBucket(PlayerButton button) {
        auto raw = static_cast<int>(button);
        if (raw < 0) {
            raw = -raw;
        }
        return static_cast<size_t>(raw) % kButtonBuckets;
    }

    constexpr double kDefaultMaxCps = 16.6667;
    constexpr double kDefaultHoldFraction = 0.5;

    Config currentConfig() {
        auto mod = Mod::get();
        Config cfg;
        cfg.enabled = mod->getSavedValue<bool>("enabled", true);
        cfg.perButton = mod->getSavedValue<bool>("per-button", false);
        cfg.debounce = mod->getSavedValue<bool>("debounce", false);

        auto maxCps = std::max(0.1, mod->getSavedValue<double>("max-cps", kDefaultMaxCps));
        auto holdFraction = std::clamp(
            mod->getSavedValue<double>("hold-fraction", kDefaultHoldFraction), 0.1, 0.9
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
        static bool lastDebounce = false;
        if (cfg.holdLockout != lastHold || cfg.gapLockout != lastGap
            || cfg.perButton != lastPerButton || cfg.debounce != lastDebounce) {
            lastHold = cfg.holdLockout;
            lastGap = cfg.gapLockout;
            lastPerButton = cfg.perButton;
            lastDebounce = cfg.debounce;
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
    double lockoutRatio(EdgeKind kind, Config const& cfg) {
        return lockoutFor(kind, cfg) * hudSync() / g_dtMedian;
    }
    double cycleRatio(Config const& cfg) {
        return (cfg.holdLockout + cfg.gapLockout) * hudSync() / g_dtMedian;
    }
    bool frameReached(uint64_t now, double lock) { 
        return static_cast<double>(now) >= lock - 0.1;
    }
    double anchorFor(uint64_t now, double lock) {
        return static_cast<double>(now) <= lock + 1.0 ? lock : static_cast<double>(now);
    }
    bool handleEdge(PlayerObject* player, PlayerButton button, EdgeKind kind) { // intercept input if the cap is locked
        if (g_emittingDeferred) {
            return true;
        }
        auto cfg = currentConfig();
        if (!cfg.enabled || hudSync() < 0.75) {
            return true;
        }
        resetIfConfigChanged(cfg);

        auto now = g_frame;
        auto& state = g_states[player];
        auto& ch = channelFor(state, button, cfg.perButton);
        auto& lock = lockRef(state, ch, cfg.perButton);

        if (cfg.debounce) {
            if (kind == EdgeKind::Release) {
                return true;
            }
            if (frameReached(now, lock)) {
                lock = anchorFor(now, lock) + cycleRatio(cfg);
                return true;
            }
            g_wasCappedThisAttempt = true;
            return false;
        }

        if (ch.queuedEdges == 0 && frameReached(now, lock)) {
            ch.lastEmitted = kind;
            ch.button = button;
            lock = anchorFor(now, lock) + lockoutRatio(kind, cfg);
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

    void flushPlayer(PlayerObject* player, Config const& cfg, uint64_t now) { // fire queued edges when frame count allows
        auto it = g_states.find(player);
        if (it == g_states.end()) {
            return;
        }
        auto& state = it->second;

        size_t buckets = cfg.perButton ? kButtonBuckets : 1;
        for (size_t i = 0; i < buckets; ++i) {
            auto& ch = state.channels[i];
            auto& lock = lockRef(state, ch, cfg.perButton);
            while (ch.queuedEdges > 0 && frameReached(now, lock)) {
                auto kind = opposite(ch.lastEmitted);
                ch.lastEmitted = kind;
                ch.queuedEdges -= 1;
                lock += lockoutRatio(kind, cfg);


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
        if (dt > 0.f) {
            ++g_frame;
            noteFrameDt(dt);
        }
        auto cfg = currentConfig();
        if (cfg.enabled && !g_states.empty()) {
            resetIfConfigChanged(cfg);
            if (m_player1) flushPlayer(m_player1, cfg, g_frame);
            if (m_player2) flushPlayer(m_player2, cfg, g_frame);
        }
        GJBaseGameLayer::update(dt);
        alignHudGrid();
    }
};

class $modify(CappedPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        g_wasCappedThisAttempt = false;
        g_states.clear();
        g_hudBase = false;
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
        g_hudBase = false;
        PlayLayer::onQuit();
    }
};

// the settings menu whichnis opened in the pause menu on a level
class CPSCapPopup : public geode::Popup {
protected:
    TextInput* m_cpsInput = nullptr;
    TextInput* m_fracInput = nullptr;
    Slider* m_cpsSlider = nullptr;
    Slider* m_fracSlider = nullptr;

    static double cpsFromSlider(float v) {
        return 1.0 + static_cast<double>(v) * 239.0;
    }
    static float sliderFromCps(double cps) {
        return static_cast<float>(std::clamp((cps - 1.0) / 239.0, 0.0, 1.0));
    }
    static double fracFromSlider(float v) {
        return 0.1 + static_cast<double>(v) * 0.8;
    }
    static float sliderFromFrac(double frac) {
        return static_cast<float>(std::clamp((frac - 0.1) / 0.8, 0.0, 1.0));
    }

    void refreshCps(bool updateSlider) {
        auto cps = Mod::get()->getSavedValue<double>("max-cps", kDefaultMaxCps);
        m_cpsInput->setString(fmt::format("{:.2f}", cps));
        if (updateSlider) {
            m_cpsSlider->setValue(sliderFromCps(cps));
        }
    }

    void refreshFrac(bool updateSlider) {
        auto frac = Mod::get()->getSavedValue<double>("hold-fraction", kDefaultHoldFraction);
        m_fracInput->setString(fmt::format("{:.0f}", frac * 100.0));
        if (updateSlider) {
            m_fracSlider->setValue(sliderFromFrac(frac));
        }
    }

    CCMenuItemToggler* addToggle(
        CCMenu* menu, char const* label, char const* key, bool def, float y
    ) {
        auto toggle = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(CPSCapPopup::onToggle), 0.7f
        );
        toggle->setUserObject(CCString::create(key));
        toggle->toggle(Mod::get()->getSavedValue<bool>(key, def));
        toggle->setPosition({45.f, y});
        menu->addChild(toggle);

        auto text = CCLabelBMFont::create(label, "bigFont.fnt");
        text->setScale(0.45f);
        text->setAnchorPoint({0.f, 0.5f});
        text->setPosition({65.f, y});
        m_mainLayer->addChild(text);
        return toggle;
    }

    bool init() {
        if (!Popup::init(320.f, 260.f)) {
            return false;
        }
        this->setTitle("CPS Cap");
        auto menu = m_buttonMenu;

        addToggle(menu, "Enable CPS Cap", "enabled", true, 215.f);
        addToggle(menu, "Debounce", "debounce", false, 186.f);
        addToggle(menu, "Cap Buttons Separately", "per-button", false, 157.f);

        // cps label
        auto cpsLabel = CCLabelBMFont::create("Max CPS", "bigFont.fnt");
        cpsLabel->setScale(0.45f);
        cpsLabel->setAnchorPoint({0.f, 0.5f});
        cpsLabel->setPosition({65.f, 124.f});
        m_mainLayer->addChild(cpsLabel);

        // cps input
        m_cpsInput = TextInput::create(70.f, "16.67");
        m_cpsInput->setCommonFilter(CommonFilter::Float);
        m_cpsInput->setMaxCharCount(7);
        m_cpsInput->setScale(0.8f);
        m_cpsInput->setPosition({250.f, 124.f});
        m_cpsInput->setCallback([this](std::string const& str) {
            this->onCpsInput(str);
        });
        m_mainLayer->addChild(m_cpsInput);

        // cps slider
        m_cpsSlider = Slider::create(this, menu_selector(CPSCapPopup::onCpsSlider), 0.8f);
        m_cpsSlider->setPosition({150.f, 96.f});
        m_mainLayer->addChild(m_cpsSlider);

        // k55 reset button
        auto k55Spr = CCSprite::createWithSpriteFrameName("geode.loader/reset-gold.png");
        k55Spr->setScale(0.7f);
        auto k55Btn = CCMenuItemSpriteExtra::create(
            k55Spr, this, menu_selector(CPSCapPopup::onK55Reset)
        );
        k55Btn->setPosition({258.f, 96.f});
        menu->addChild(k55Btn);

        // split label
        auto fracLabel = CCLabelBMFont::create("Press/Release Split", "bigFont.fnt");
        fracLabel->setScale(0.45f);
        fracLabel->setAnchorPoint({0.f, 0.5f});
        fracLabel->setPosition({65.f, 62.f});
        m_mainLayer->addChild(fracLabel);

        // split input
        m_fracInput = TextInput::create(70.f, "50");
        m_fracInput->setCommonFilter(CommonFilter::Uint);
        m_fracInput->setMaxCharCount(2);
        m_fracInput->setScale(0.8f);
        m_fracInput->setPosition({250.f, 62.f});
        m_fracInput->setCallback([this](std::string const& str) {
            this->onFracInput(str);
        });
        m_mainLayer->addChild(m_fracInput);

        // split slider
        m_fracSlider = Slider::create(this, menu_selector(CPSCapPopup::onFracSlider), 0.8f);
        m_fracSlider->setPosition({150.f, 34.f});
        m_mainLayer->addChild(m_fracSlider);

        refreshCps(true);
        refreshFrac(true);
        return true;
    }

    void onToggle(CCObject* sender) {
        auto toggle = static_cast<CCMenuItemToggler*>(sender);
        auto key = static_cast<CCString*>(toggle->getUserObject())->getCString();
        Mod::get()->setSavedValue<bool>(key, !toggle->isToggled());
    }

    void onCpsSlider(CCObject* sender) {
        auto value = static_cast<SliderThumb*>(sender)->getValue();
        Mod::get()->setSavedValue<double>("max-cps", cpsFromSlider(value));
        refreshCps(false);
    }

    void onFracSlider(CCObject* sender) {
        auto value = static_cast<SliderThumb*>(sender)->getValue();
        Mod::get()->setSavedValue<double>("hold-fraction", fracFromSlider(value));
        refreshFrac(false);
    }

    void onCpsInput(std::string const& str) {
        if (auto num = numFromString<double>(str)) {
            auto cps = std::clamp(*num, 1.0, 240.0);
            Mod::get()->setSavedValue<double>("max-cps", cps);
            m_cpsSlider->setValue(sliderFromCps(cps));
        }
    }

    void onFracInput(std::string const& str) {
        if (auto num = numFromString<double>(str)) {
            auto frac = std::clamp(*num / 100.0, 0.1, 0.9);
            Mod::get()->setSavedValue<double>("hold-fraction", frac);
            m_fracSlider->setValue(sliderFromFrac(frac));
        }
    }

    void onK55Reset(CCObject*) {
        Mod::get()->setSavedValue<double>("max-cps", kDefaultMaxCps);
        refreshCps(true);
    }

public:
    static CPSCapPopup* create() {
        auto ret = new CPSCapPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class $modify(CappedPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto sprite = CCSprite::create("MenuKey.png"_spr);
        if (!sprite) return;
        sprite->setScale(30.f / sprite->getContentSize().height);
        auto button = CCMenuItemSpriteExtra::create(
            sprite, this, menu_selector(CappedPauseLayer::onCPSCap)
        );
        button->setID("cps-cap-button"_spr);

        if (auto menu = this->getChildByID("right-button-menu")) {
            menu->addChild(button);
            menu->updateLayout();
        }
    }

    void onCPSCap(CCObject*) {
        CPSCapPopup::create()->show();
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
