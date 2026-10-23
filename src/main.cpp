#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>

using namespace geode::prelude;

namespace {

class SpamCorrecter {
private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // Three alternating Jump transitions, each no more than 120 ms apart,
    // switch on correction. A 300 ms pause between transitions switches it off.
    static constexpr auto kRapidInputWindow = std::chrono::milliseconds{120};
    static constexpr auto kStopTimeout = std::chrono::milliseconds{300};
    static constexpr int kRapidTransitionsToActivate = 3;

    bool m_active = false;
    bool m_sendingInput = false;
    bool m_userHolding = false;
    bool m_gameHolding = false;
    bool m_autoHolding = false;

    bool m_hasPreviousTransition = false;
    int m_rapidTransitions = 0;
    TimePoint m_lastTransition{};
    TimePoint m_lastUserTransition{};
    TimePoint m_nextToggle{};

    std::chrono::milliseconds getInterval(bool holding) const {
        auto const key = holding ? "hold-interval" : "release-interval";
        auto const value = Mod::get()->getSettingValue<int64_t>(key);
        auto const safeValue = std::clamp<int64_t>(value, 1, 60000);
        return std::chrono::milliseconds{safeValue};
    }

    void sendButton(GJBaseGameLayer* layer, bool down) {
        if (!layer || m_gameHolding == down) {
            return;
        }

        // Route generated presses through the normal gameplay input handler,
        // while the hook below lets this one generated event pass through.
        m_sendingInput = true;
        layer->handleButton(down, 1, true);

        // In single-player Dual mode, mirror the generated input to player 2,
        // matching the game's usual single-player control behavior.
        PlayLayer* playLayer = PlayLayer::get();
        if (playLayer && static_cast<GJBaseGameLayer*>(playLayer) == layer &&
            playLayer->m_gameState.m_isDualMode &&
            (!playLayer->m_level || !playLayer->m_level->m_twoPlayerMode)) {
            layer->handleButton(down, 1, false);
        }

        m_sendingInput = false;
        m_gameHolding = down;
    }

    void finishSpam(GJBaseGameLayer* layer) {
        if (m_gameHolding) {
            sendButton(layer, false);
        }

        m_active = false;
        m_autoHolding = false;
        m_rapidTransitions = 0;
        m_hasPreviousTransition = false;
    }

public:
    bool isActive() const {
        return m_active;
    }

    bool isSendingInput() const {
        return m_sendingInput;
    }

    void reset() {
        m_active = false;
        m_sendingInput = false;
        m_userHolding = false;
        m_gameHolding = false;
        m_autoHolding = false;
        m_hasPreviousTransition = false;
        m_rapidTransitions = 0;
        m_lastTransition = TimePoint{};
        m_lastUserTransition = TimePoint{};
        m_nextToggle = TimePoint{};
    }

    // Returns true when the physical Jump event should be swallowed.
    bool handleUserInput(GJBaseGameLayer* layer, bool down) {
        auto const now = Clock::now();

        // If the player resumes after the timeout, release automation first and
        // process this new input as an ordinary manual input.
        if (m_active && now - m_lastUserTransition >= kStopTimeout) {
            finishSpam(layer);
        }

        // Repeated events without a press/release transition are not spam and
        // do not keep correction mode alive.
        if (down == m_userHolding) {
            return m_active;
        }

        m_userHolding = down;
        m_lastUserTransition = now;

        if (m_active) {
            return true;
        }

        if (m_hasPreviousTransition && now - m_lastTransition <= kRapidInputWindow) {
            ++m_rapidTransitions;
        } else {
            m_rapidTransitions = 1;
        }

        m_lastTransition = now;
        m_hasPreviousTransition = true;

        if (m_rapidTransitions < kRapidTransitionsToActivate) {
            return false;
        }

        // Take over at the third fast alternating edge. Start in the pressed
        // phase; later phases use the user's custom hold/release intervals.
        m_active = true;
        m_autoHolding = true;
        m_rapidTransitions = 0;
        m_hasPreviousTransition = false;
        sendButton(layer, true);
        m_nextToggle = now + getInterval(true);
        return true;
    }

    void noteGameHolding(bool down) {
        m_gameHolding = down;
    }

    void update(PlayerObject* player) {
        PlayLayer* playLayer = PlayLayer::get();
        if (!playLayer || playLayer->m_player1 != player || !m_active) {
            return;
        }

        auto const now = Clock::now();
        if (now - m_lastUserTransition >= kStopTimeout) {
            finishSpam(playLayer);
            return;
        }

        if (now < m_nextToggle) {
            return;
        }

        m_autoHolding = !m_autoHolding;
        sendButton(playLayer, m_autoHolding);
        m_nextToggle = now + getInterval(m_autoHolding);
    }

    void cancel(GJBaseGameLayer* layer) {
        if (m_gameHolding) {
            sendButton(layer, false);
        }
        reset();
    }
};

SpamCorrecter g_spamCorrecter;

} // namespace

class $modify(SpamCorrecterBaseLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (g_spamCorrecter.isSendingInput()) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        PlayLayer* playLayer = PlayLayer::get();
        bool const isCurrentPlayLayer =
            playLayer && static_cast<GJBaseGameLayer*>(playLayer) == this;

        if (button == 1 && isCurrentPlayLayer) {
            bool const isSinglePlayerDual = playLayer->m_gameState.m_isDualMode &&
                (!playLayer->m_level || !playLayer->m_level->m_twoPlayerMode);

            // Ignore the game's mirrored player-2 event while automated input
            // is controlling both avatars in single-player Dual mode.
            if (!isPlayer1 && isSinglePlayerDual && g_spamCorrecter.isActive()) {
                return;
            }

            if (isPlayer1 && g_spamCorrecter.handleUserInput(this, down)) {
                return;
            }
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        if (button == 1 && isCurrentPlayLayer && isPlayer1) {
            g_spamCorrecter.noteGameHolding(down);
        }
    }
};

class $modify(SpamCorrecterPlayerObject, PlayerObject) {
    void update(float dt) {
        g_spamCorrecter.update(this);
        PlayerObject::update(dt);
    }
};

class $modify(SpamCorrecterPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }
        g_spamCorrecter.reset();
        return true;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (player == m_player1) {
            g_spamCorrecter.cancel(this);
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void onQuit() {
        g_spamCorrecter.cancel(this);
        PlayLayer::onQuit();
    }
};
