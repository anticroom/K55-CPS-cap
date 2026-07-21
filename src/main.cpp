#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/Popup.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_map>

using namespace geode::prelude;

namespace {

struct Options {
    bool enabled;
    bool perButton;
    bool debounce;
    double pressLock;
    double releaseLock;
};

constexpr double DEFAULT_MAX_CPS = 16.6667;
constexpr double DEFAULT_HOLD_FRACTION = 0.5;

Options loadOptions() {
    auto mod = Mod::get();
    Options o;
    o.enabled = mod->getSavedValue<bool>("enabled", true);
    // o.perButton = mod->getSavedValue<bool>("per-button", false);
    o.perButton = false;
    o.debounce = mod->getSavedValue<bool>("debounce", false);

    auto maxCps = std::max(0.1, mod->getSavedValue<double>("max-cps", DEFAULT_MAX_CPS));
    auto holdFraction = std::clamp(
        mod->getSavedValue<double>("hold-fraction", DEFAULT_HOLD_FRACTION), 0.1, 0.9
    );
    auto period = 1.0 / maxCps;
    o.pressLock = period * holdFraction;
    o.releaseLock = period * (1.0 - holdFraction);
    return o;
}

// int buttonSlot(PlayerButton button) {
//     switch (button) {
//         case PlayerButton::Left: return 1;
//         case PlayerButton::Right: return 2;
//         default: return 0;
//     }
// }

struct Gate {
    bool held = false;
    bool queued = false;
    PlayerButton pendingButton = PlayerButton::Jump;
    double unlockFrame = 0.0;
};

struct PlayerGates {
    Gate perButton[3];
    Gate shared;
};

std::unordered_map<PlayerObject*, PlayerGates> gates;
uint64_t frameCount = 0;
double medianFrameTime = 1.0 / 240.0;
std::array<float, 9> frameTimes{};
size_t frameTimeIndex = 0;
bool replaying = false;
bool cappedThisAttempt = false;

void trackFrameTime(float dt) {
    frameTimes[frameTimeIndex % frameTimes.size()] = dt;
    frameTimeIndex++;

    auto count = std::min(frameTimeIndex, frameTimes.size());
    auto sorted = frameTimes;
    std::sort(sorted.begin(), sorted.begin() + count);
    medianFrameTime = sorted[count / 2];
}

double toFrames(double seconds) {
    return seconds / medianFrameTime;
}

bool unlocked(double unlockFrame) {
    return static_cast<double>(frameCount) >= unlockFrame - 0.1;
}

double carryUnlock(double unlockFrame) {
    return static_cast<double>(frameCount) <= unlockFrame + 1.0
        ? unlockFrame
        : static_cast<double>(frameCount);
}

void resetIfChanged(Options const& o) {
    static double lastPress = 0.0;
    static double lastRelease = 0.0;
    static bool lastPerButton = false;
    static bool lastDebounce = false;

    if (o.pressLock != lastPress || o.releaseLock != lastRelease
        || o.perButton != lastPerButton || o.debounce != lastDebounce) {
        lastPress = o.pressLock;
        lastRelease = o.releaseLock;
        lastPerButton = o.perButton;
        lastDebounce = o.debounce;
        gates.clear();
    }
}

// Gate& pickGate(PlayerGates& pg, PlayerButton button, bool perButton) {
//     return perButton ? pg.perButton[buttonSlot(button)] : pg.shared;
// }

bool filterEdge(PlayerObject* player, PlayerButton button, bool press) {
    if (replaying) {
        return true;
    }

    auto options = loadOptions();
    if (!options.enabled) {
        return true;
    }
    resetIfChanged(options);

    auto& gate = gates[player].shared;

    if (options.debounce) {
        if (!press) {
            return true;
        }
        if (unlocked(gate.unlockFrame)) {
            gate.unlockFrame = carryUnlock(gate.unlockFrame)
                + toFrames(options.pressLock + options.releaseLock);
            return true;
        }
        cappedThisAttempt = true;
        return false;
    }

    if (!gate.queued && unlocked(gate.unlockFrame)) {
        gate.held = press;
        gate.pendingButton = button;
        gate.unlockFrame = carryUnlock(gate.unlockFrame)
            + toFrames(press ? options.pressLock : options.releaseLock);
        return true;
    }

    bool effectiveHeld = gate.queued ? !gate.held : gate.held;
    cappedThisAttempt = true;
    if (press == effectiveHeld) {
        return false;
    }

    if (!gate.queued) {
        gate.queued = true;
        gate.pendingButton = button;
    } else {
        gate.queued = false;
    }
    return false;
}

void flushGate(Gate& gate, PlayerObject* player, Options const& options) {
    while (gate.queued && unlocked(gate.unlockFrame)) {
        gate.held = !gate.held;
        gate.queued = false;
        gate.unlockFrame += toFrames(gate.held ? options.pressLock : options.releaseLock);

        replaying = true;
        if (gate.held) {
            player->pushButton(gate.pendingButton);
        } else {
            player->releaseButton(gate.pendingButton);
        }
        replaying = false;
    }
}

void flushPlayer(PlayerObject* player, Options const& options) {
    auto it = gates.find(player);
    if (it == gates.end()) {
        return;
    }
    auto& pg = it->second;

    // if (options.perButton) {
    //     for (auto& gate : pg.perButton) {
    //         flushGate(gate, player, options);
    //     }
    // } else {
        flushGate(pg.shared, player, options);
    // }
}

}

class $modify(CappedBaseLayer, GJBaseGameLayer) {
    void update(float dt) {
        if (dt > 0.f) {
            frameCount++;
            trackFrameTime(dt);
        }

        auto options = loadOptions();
        if (options.enabled && !gates.empty()) {
            resetIfChanged(options);
            if (m_player1) flushPlayer(m_player1, options);
            if (m_player2) flushPlayer(m_player2, options);
        }

        GJBaseGameLayer::update(dt);
    }
};

class $modify(CappedPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        gates.clear();
        cappedThisAttempt = false;
        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    void levelComplete() {
        PlayLayer::levelComplete();

        if (cappedThisAttempt) {
            Notification::create("CPS capped", NotificationIcon::Info)->show();
            cappedThisAttempt = false;
        }
    }

    void resetLevel() {
        gates.clear();
        cappedThisAttempt = false;
        PlayLayer::resetLevel();
    }

    void resetLevelFromStart() {
        gates.clear();
        cappedThisAttempt = false;
        PlayLayer::resetLevelFromStart();
    }

    void onQuit() {
        gates.clear();
        cappedThisAttempt = false;
        PlayLayer::onQuit();
    }
};

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
        auto cps = Mod::get()->getSavedValue<double>("max-cps", DEFAULT_MAX_CPS);
        m_cpsInput->setString(fmt::format("{:.2f}", cps));
        if (updateSlider) {
            m_cpsSlider->setValue(sliderFromCps(cps));
        }
    }

    void refreshFrac(bool updateSlider) {
        auto frac = Mod::get()->getSavedValue<double>("hold-fraction", DEFAULT_HOLD_FRACTION);
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

        auto cpsLabel = CCLabelBMFont::create("Max CPS", "bigFont.fnt");
        cpsLabel->setScale(0.45f);
        cpsLabel->setAnchorPoint({0.f, 0.5f});
        cpsLabel->setPosition({65.f, 216.f});
        m_mainLayer->addChild(cpsLabel);

        m_cpsInput = TextInput::create(70.f, "16.67");
        m_cpsInput->setCommonFilter(CommonFilter::Float);
        m_cpsInput->setMaxCharCount(7);
        m_cpsInput->setScale(0.8f);
        m_cpsInput->setPosition({250.f, 216.f});
        m_cpsInput->setCallback([this](std::string const& str) {
            this->onCpsInput(str);
        });
        m_mainLayer->addChild(m_cpsInput);

        m_cpsSlider = Slider::create(this, menu_selector(CPSCapPopup::onCpsSlider), 0.8f);
        m_cpsSlider->setPosition({150.f, 188.f});
        m_mainLayer->addChild(m_cpsSlider);

        auto k55Spr = CCSprite::createWithSpriteFrameName("geode.loader/reset-gold.png");
        k55Spr->setScale(0.7f);
        auto k55Btn = CCMenuItemSpriteExtra::create(
            k55Spr, this, menu_selector(CPSCapPopup::onK55Reset)
        );
        k55Btn->setPosition({258.f, 188.f});
        menu->addChild(k55Btn);

        auto fracLabel = CCLabelBMFont::create("Press/Release Split", "bigFont.fnt");
        fracLabel->setScale(0.45f);
        fracLabel->setAnchorPoint({0.f, 0.5f});
        fracLabel->setPosition({65.f, 154.f});
        m_mainLayer->addChild(fracLabel);

        m_fracInput = TextInput::create(70.f, "50");
        m_fracInput->setCommonFilter(CommonFilter::Uint);
        m_fracInput->setMaxCharCount(2);
        m_fracInput->setScale(0.8f);
        m_fracInput->setPosition({250.f, 154.f});
        m_fracInput->setCallback([this](std::string const& str) {
            this->onFracInput(str);
        });
        m_mainLayer->addChild(m_fracInput);

        m_fracSlider = Slider::create(this, menu_selector(CPSCapPopup::onFracSlider), 0.8f);
        m_fracSlider->setPosition({150.f, 126.f});
        m_mainLayer->addChild(m_fracSlider);

        addToggle(menu, "Enable CPS Cap", "enabled", true, 78.f);
        addToggle(menu, "Debounce", "debounce", false, 49.f);

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
        Mod::get()->setSavedValue<double>("max-cps", DEFAULT_MAX_CPS);
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

        auto sprite = CCSprite::create("MenuCap.png"_spr);
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
        if (!filterEdge(this, button, true)) {
            return false;
        }
        return PlayerObject::pushButton(button);
    }

    bool releaseButton(PlayerButton button) {
        if (!filterEdge(this, button, false)) {
            return false;
        }
        return PlayerObject::releaseButton(button);
    }
};
