#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Windows.h>
#include <cstdio>
#include <algorithm>

using namespace geode::prelude;

// ============================================================================
//  USER SETTINGS
// ============================================================================

// True Wave Spam Guard: only bypasses the buffer if you do at least
// SPAM_STREAK_REQUIRED consecutive inputs faster than SPAM_INTERVAL_TICKS apart.
// This keeps isolated micro-clicks and spike/sawblade dodges 100% protected!
constexpr int SPAM_INTERVAL_TICKS = 8;
constexpr int SPAM_STREAK_REQUIRED = 3;

// ============================================================================

static void refreshBufferHud();

// SONWARE V2 defaults are defined in mod.json.
static bool essentialLogsEnabled() {
    return Mod::get()->getSettingValue<bool>("essential-logs");
}

static bool detailedInputLogsEnabled() {
    return Mod::get()->getSettingValue<bool>("detailed-input-logs");
}

static int getDelayScanStride(int maxDelayFrames) {
    // Full scans every possible delay. Balanced checks up to about 32
    // candidate delays; Low-end checks about 12 to reduce CPU cost.
    // This trades some fine-grained delay selection for lower CPU cost.
    auto preset = Mod::get()->getSettingValue<std::string>("performance-preset");
    if (preset == "Full" || maxDelayFrames <= 1) return 1;

    int budget = preset == "Low-end" ? 6 : 12;
    return std::max(1, (maxDelayFrames + budget - 1) / budget);
}

class TrajectoryBufferSim {
private:
    PlayerObject* m_simPlayer1 = nullptr;
    PlayerObject* m_simPlayer2 = nullptr;

    bool m_simulating = false;
    bool m_firingBuffered = false;
    bool m_simulationDead = false;

    // Tracks actual physical mouse state vs what PlayLayer currently has active
    bool m_physicalHolding = false;
    bool m_gameHolding = false;

    // F6 Cycle State (Starts on Stage 0: 2 ticks Normal / 2 ticks Wave)
    int  m_presetIndex = 0;
    bool m_enabled = true;
    int  m_maxTicksNormal = 2;
    int  m_maxTicksWave = 2;

    // Buffer state
    int  m_waitTicks = -1;          // -1 = inactive, >= 0 = countdown ticks, -2 = queued opposite tick
    bool m_bufferedPush = true;     // true = buffered PRESS, false = buffered RELEASE
    bool m_queuedOpposite = false;  // true if user quickly released/re-pressed during wait
    float m_frameDt = 0.25f;        // GD 240 TPS normalized dt is 0.25f

    // Streak-based Spam Guard state
    int  m_ticksSinceLastInput = 999;
    int  m_rapidInputStreak = 0;

    PlayerObject* createTrajectoryPlayer() {
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return nullptr;

        PlayerObject* player = PlayerObject::create(1, 1, pl, pl, true);
        player->retain();
        player->setPosition({0, 105});
        player->setVisible(false);
        pl->m_objectLayer->addChild(player);
        return player;
    }

    void resetCollisionLog(PlayerObject* self) {
        self->m_collisionLogTop->removeAllObjects();
        self->m_collisionLogBottom->removeAllObjects();
        self->m_collisionLogLeft->removeAllObjects();
        self->m_collisionLogRight->removeAllObjects();
    }

    // Matches Show Trajectory's exact attribute + hitbox size synchronization
    void syncSimPlayer(PlayerObject* sim, PlayerObject* real) {
        sim->setVisible(false);
        sim->copyAttributes(real);
        sim->setScale(real->getScale());
        sim->setContentSize(real->getContentSize());
        sim->m_width = real->m_width;
        sim->m_height = real->m_height;
        sim->m_gravityMod = real->m_gravityMod;
        sim->m_isOnGround = real->m_isOnGround;
        sim->m_isUpsideDown = real->m_isUpsideDown;
        sim->m_vehicleSize = real->m_vehicleSize;
        sim->m_playerSpeed = real->m_playerSpeed;
    }

    float getValidDt() const {
        if (m_frameDt > 0.001f && m_frameDt < 2.0f) {
            return m_frameDt;
        }
        return 0.25f;
    }

public:
    void init() {
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return;

        m_simPlayer1 = createTrajectoryPlayer();
        m_simPlayer2 = createTrajectoryPlayer();
        m_waitTicks = -1;
        m_queuedOpposite = false;
        m_physicalHolding = false;
        m_gameHolding = false;
        m_simulating = false;
        m_firingBuffered = false;
        m_ticksSinceLastInput = 999;
        m_rapidInputStreak = 0;
    }

    void quit() {
        if (m_simPlayer1) {
            m_simPlayer1->release();
            m_simPlayer1 = nullptr;
        }
        if (m_simPlayer2) {
            m_simPlayer2->release();
            m_simPlayer2 = nullptr;
        }
        m_simulating = false;
        m_firingBuffered = false;
        m_waitTicks = -1;
        m_queuedOpposite = false;
        m_rapidInputStreak = 0;
    }

    // F6 cycles through 9 buffer presets (maximum 60 ticks) and OFF.
    // Single-value presets use the same tick limit for normal and wave.
    void cyclePreset() {
        m_presetIndex = (m_presetIndex + 1) % 10;
        m_waitTicks = -1;
        m_queuedOpposite = false;

        constexpr int normalTicks[] = {2, 4, 8, 16, 24, 32, 48, 54, 60, 0};
        constexpr int waveTicks[]   = {2, 4, 8,  8, 24, 32, 48, 54, 60, 0};
        m_enabled = m_presetIndex != 9;
        m_maxTicksNormal = normalTicks[m_presetIndex];
        m_maxTicksWave = waveTicks[m_presetIndex];

        if (essentialLogsEnabled()) {
            if (m_enabled) {
                log::info("[SONWARE] Stage {} selected: {} ticks normal / {} ticks wave.",
                    m_presetIndex + 1, m_maxTicksNormal, m_maxTicksWave);
            } else {
                log::info("[SONWARE] Buffer OFF; raw input enabled.");
            }
        }
    }

    int normalTicks() const { return m_maxTicksNormal; }
    int waveTicks() const { return m_maxTicksWave; }
    bool enabled() const { return m_enabled; }
    bool isSimulating() const { return m_simulating; }
    bool isFiringBuffered() const { return m_firingBuffered; }

    bool checkSimulationDeath(PlayerObject* player) {
        if (m_simulating && (player == m_simPlayer1 || player == m_simPlayer2)) {
            m_simulationDead = true;
            return true;
        }
        // Real player died -> clear any pending buffer so respawn is 100% clean
        m_waitTicks = -1;
        m_queuedOpposite = false;
        m_gameHolding = false;
        m_rapidInputStreak = 0;
        return false;
    }

    void setFrameDelta(float dt) {
        PlayLayer* pl = PlayLayer::get();
        if (pl && pl->m_gameState.m_timeWarp > 0.f) {
            m_frameDt = dt / pl->m_gameState.m_timeWarp;
        } else {
            m_frameDt = dt;
        }
    }

    int getSurvivalTicks(PlayLayer* pl, PlayerObject* realP1, PlayerObject* realP2, bool isDual, int delayFrames, bool actionPush, int maxTicks) {
        if (!pl || !m_simPlayer1 || !realP1) return maxTicks;

        m_simulating = true;
        m_simulationDead = false;

        syncSimPlayer(m_simPlayer1, realP1);
        if (isDual && m_simPlayer2 && realP2) {
            syncSimPlayer(m_simPlayer2, realP2);
        }

        // Before delayFrames, maintain the pre-input button state
        if (delayFrames > 0) {
            if (!actionPush) {
                m_simPlayer1->pushButton(PlayerButton::Jump);
                if (isDual && m_simPlayer2) {
                    m_simPlayer2->pushButton(PlayerButton::Jump);
                }
            } else {
                m_simPlayer1->releaseButton(PlayerButton::Jump);
                if (isDual && m_simPlayer2) {
                    m_simPlayer2->releaseButton(PlayerButton::Jump);
                }
            }
        }

        float dt = getValidDt();
        int survived = 0;
        bool isContinuousMode = realP1->m_isDart || realP1->m_isBird || realP1->m_isSwing ||
                                (isDual && realP2 && (realP2->m_isDart || realP2->m_isBird || realP2->m_isSwing));
        int airborneTicks = 0;
        int safeLandedTicks = 0;

        for (int i = 0; i < delayFrames + maxTicks; i++) {
            if (i == delayFrames) {
                if (actionPush) {
                    m_simPlayer1->pushButton(PlayerButton::Jump);
                    if (isDual && m_simPlayer2) {
                        m_simPlayer2->pushButton(PlayerButton::Jump);
                    }
                } else {
                    m_simPlayer1->releaseButton(PlayerButton::Jump);
                    if (isDual && m_simPlayer2) {
                        m_simPlayer2->releaseButton(PlayerButton::Jump);
                    }
                }
            }

            m_simPlayer1->update(dt);
            m_simPlayer1->updateRotation(dt);
            if (isDual && m_simPlayer2) {
                m_simPlayer2->update(dt);
                m_simPlayer2->updateRotation(dt);
            }
            if (m_simulationDead) break;

            resetCollisionLog(m_simPlayer1);
            pl->checkCollisions(m_simPlayer1, dt, false);
            if (m_simulationDead) break;

            if (isDual && m_simPlayer2 && realP2) {
                resetCollisionLog(m_simPlayer2);
                pl->checkCollisions(m_simPlayer2, dt, false);
                if (m_simulationDead) break;
            }

            if (i >= delayFrames) {
                survived++;
                if (!isContinuousMode) {
                    bool p1Grounded = m_simPlayer1->m_isOnGround;
                    bool p2Grounded = (!isDual || !m_simPlayer2) ? true : m_simPlayer2->m_isOnGround;
                    if (!p1Grounded || !p2Grounded) {
                        airborneTicks++;
                        safeLandedTicks = 0;
                    } else if (airborneTicks >= 15) {
                        safeLandedTicks++;
                        if (safeLandedTicks >= 10) {
                            survived = maxTicks;
                            break;
                        }
                    }
                }
            }
        }

        m_simulating = false;
        return survived;
    }

    bool interceptInput(GJBaseGameLayer* layer, bool push, bool isPlayer1) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return true;

        bool isDual = pl->m_gameState.m_isDualMode && pl->m_player2 != nullptr;
        bool isTwoPlayerControl = pl->m_level && pl->m_level->m_twoPlayerMode;

        // Keep Player 2 synced with Player 1 in 1-player Dual mode
        if (!isPlayer1) {
            if (isDual && !isTwoPlayerControl && (m_waitTicks >= 0 || m_waitTicks == -2)) {
                return false;
            }
            return true;
        }

        // Always record the user's true physical mouse/key state first!
        m_physicalHolding = push;

        if (!m_enabled) {
            m_gameHolding = push;
            return true;
        }

        PlayerObject* p1 = layer->m_player1;
        PlayerObject* p2 = isDual ? pl->m_player2 : nullptr;
        if (!p1 || p1->m_isDead) {
            m_waitTicks = -1;
            m_queuedOpposite = false;
            m_gameHolding = push;
            return true;
        }

        // 1. NEVER touch Ship mode (or Dual with Ship)
        if (p1->m_isShip || (isDual && p2 && p2->m_isShip)) {
            m_waitTicks = -1;
            m_queuedOpposite = false;
            m_gameHolding = push;
            return true;
        }

        // 2. If an input is currently waiting in the buffer and the user quickly releases
        // (or re-presses) 1-2 ticks later, queue it for the next tick so fast taps are never eaten
        if (m_waitTicks >= 0) {
            if (push != m_bufferedPush) {
                m_queuedOpposite = true;
            } else {
                m_queuedOpposite = false;
            }
            m_ticksSinceLastInput = 0;
            return false;
        }

        // If a queued opposite action is due to fire on the next tick (m_waitTicks == -2)
        // and the user clicks again, fire the pending opposite action first
        if (m_waitTicks == -2) {
            m_waitTicks = -1;
            m_queuedOpposite = false;
            if (m_gameHolding != !m_bufferedPush) {
                m_SimFireButton(pl, !m_bufferedPush);
            }
        }

        bool isWave = p1->m_isDart || (isDual && p2 && p2->m_isDart);
        bool isMiniWave = (p1->m_isDart && p1->m_vehicleSize < 1.0f) ||
                          (isDual && p2 && p2->m_isDart && p2->m_vehicleSize < 1.0f);

        // 3. STREAK-BASED WAVE SPAM GUARD:
        // Only bypasses if you perform 3+ rapid inputs in a row (< 8 ticks apart).
        // Single micro-clicks and normal Wave turns around spikes/sawblades stay 100% active!
        if (m_ticksSinceLastInput < SPAM_INTERVAL_TICKS) {
            m_rapidInputStreak++;
        } else {
            m_rapidInputStreak = 0;
        }
        m_ticksSinceLastInput = 0;

        if (isWave && m_rapidInputStreak >= SPAM_STREAK_REQUIRED) {
            m_gameHolding = push;
            return true;
        }

        // Only Wave (m_isDart) and Robot (m_isRobot) change trajectory on RELEASE
        bool releaseAffectsPhysics = p1->m_isDart || p1->m_isRobot ||
                                     (isDual && p2 && (p2->m_isDart || p2->m_isRobot));
        if (!push && !releaseAffectsPhysics) {
            m_gameHolding = false;
            return true;
        }

        m_queuedOpposite = false;
        const char* act = push ? "PRESS" : "RELEASE";

        // Full Show-Trajectory lookahead for Wave (65 ticks / 50 ticks Mini Wave)
        // so it sees spikes and sawblades 1-3 blocks ahead along the diagonal path!
        int targetHorizon = isMiniWave ? 50 : (isWave ? 65 : 160);
        int maxDelayFrames = isWave ? m_maxTicksWave : m_maxTicksNormal;

        // 4. Check Frame 0 (immediate input) across the full trajectory horizon
        int ticksFrame0 = getSurvivalTicks(pl, p1, p2, isDual, 0, push, targetHorizon);
        if (ticksFrame0 >= targetHorizon) {
            m_gameHolding = push;
            return true;
        }

        // 5. Frame 0 hits a spike, sawblade, or slope within the horizon!
        // Scan +1..maxDelayFrames for the earliest delay that genuinely clears the obstacle.
        int bestDelay = 0;
        int bestTicks = ticksFrame0;

        const int scanStride = getDelayScanStride(maxDelayFrames);
        for (int delay = 1; delay <= maxDelayFrames; delay += scanStride) {
            int ticks = getSurvivalTicks(pl, p1, p2, isDual, delay, push, targetHorizon);

            if (isWave) {
                // In Wave mode, delaying a turn inside a flat corridor naturally adds ~(2 * delay)
                // ticks against the opposite wall. Require at least (2 * delay + 10) extra ticks
                // so we ONLY buffer when the delayed trajectory actually clears past a spike,
                // sawblade, or corner!
                int minClearedGain = (delay * 2) + 10;
                if (ticks >= targetHorizon && (ticks - ticksFrame0) >= (delay * 2) + 6) {
                    bestDelay = delay;
                    bestTicks = ticks;
                    break;
                }
                if (ticks >= ticksFrame0 + minClearedGain && ticks > bestTicks + 2) {
                    bestDelay = delay;
                    bestTicks = ticks;
                    if (ticks >= targetHorizon) break;
                }
            } else {
                if (ticks >= targetHorizon) {
                    bestDelay = delay;
                    bestTicks = ticks;
                    break;
                }
                if (ticks > bestTicks + 12) {
                    bestDelay = delay;
                    bestTicks = ticks;
                }
            }
        }

        if (bestDelay > 0) {
            // Detailed per-input logging is OFF by default to avoid log spam.
            if (detailedInputLogsEnabled()) {
                log::info("[SONWARE] Buffered {} by {} ticks (immediate survival: {}, delayed survival: {}, limit: {}).",
                    act, bestDelay, ticksFrame0, bestTicks, maxDelayFrames);
            }
            m_bufferedPush = push;
            m_waitTicks = bestDelay;
            return false;
        }

        m_gameHolding = push;
        return true;
    }

    void onPlayerUpdate(PlayerObject* player) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl || player != pl->m_player1) return;

        // Check F6 before m_enabled check so you can always cycle back ON when it is OFF!
        if (GetAsyncKeyState(VK_F6) & 1) {
            cyclePreset();
            refreshBufferHud();
        }

        if (!m_enabled) return;

        if (m_ticksSinceLastInput < 999) {
            m_ticksSinceLastInput++;
        }

        if (player->m_isShip || (pl->m_gameState.m_isDualMode && pl->m_player2 && pl->m_player2->m_isShip)) {
            m_waitTicks = -1;
            m_queuedOpposite = false;
            if (m_gameHolding != m_physicalHolding) {
                m_SimFireButton(pl, m_physicalHolding);
            }
            return;
        }

        if (m_waitTicks == 0) {
            m_SimFireButton(pl, m_bufferedPush);
            if (m_queuedOpposite) {
                m_waitTicks = -2;
                m_queuedOpposite = false;
            } else {
                m_waitTicks = -1;
            }
        } else if (m_waitTicks > 0) {
            m_waitTicks--;
        } else if (m_waitTicks == -2) {
            m_SimFireButton(pl, !m_bufferedPush);
            m_queuedOpposite = false;
            m_waitTicks = -1;
        } else if (m_waitTicks == -1) {
            // FAILSAFE: When no buffer is active, guarantee the game's hold state
            // matches your actual physical mouse button so Wave/Mini Wave can NEVER stay stuck holding!
            if (m_gameHolding != m_physicalHolding) {
                m_SimFireButton(pl, m_physicalHolding);
            }
        }
    }

    void m_SimFireButton(PlayLayer* pl, bool push) {
        m_firingBuffered = true;
        m_gameHolding = push;
        pl->handleButton(push, 1, true);
        if (pl->m_gameState.m_isDualMode && (!pl->m_level || !pl->m_level->m_twoPlayerMode)) {
            pl->handleButton(push, 1, false);
        }
        m_firingBuffered = false;
    }
};

static TrajectoryBufferSim s_sim;
static cocos2d::CCLabelBMFont* s_bufferHud = nullptr;
static cocos2d::CCLabelBMFont* s_bufferButtonLabel = nullptr;
static cocos2d::CCLayerColor* s_sonwarePanel = nullptr;

static void refreshBufferButton() {
    if (!s_bufferButtonLabel) return;
    s_bufferButtonLabel->setString("CYCLE PRESET  >");
    s_bufferButtonLabel->setColor({145, 235, 255});
}

static void refreshBufferHud() {
    if (s_bufferHud) {
        if (!s_sim.enabled()) {
            s_bufferHud->setString("STATUS  |  OFF");
            s_bufferHud->setColor({255, 135, 145});
        } else {
            auto normal = s_sim.normalTicks();
            auto wave = s_sim.waveTicks();
            auto status = normal == wave
                ? fmt::format("STATUS  |  {} TICKS", normal)
                : fmt::format("STATUS  |  {} NORMAL / {} WAVE", normal, wave);
            s_bufferHud->setString(status.c_str());
            s_bufferHud->setColor({130, 245, 190});
        }
    }
    refreshBufferButton();
}

// Shared in-game/menu overlay. Right Alt toggles visibility; drag the title strip to move it.
class SonwareGuiLayer : public cocos2d::CCLayer {
protected:
    cocos2d::CCLayerColor* m_panel = nullptr;
    bool m_rightAltWasDown = false;
    bool m_dragging = false;
    bool m_leftWasDown = false;
    cocos2d::CCPoint m_lastMouse = {0.f, 0.f};

    void keepPanelOnScreen() {
        if (!m_panel) return;
        auto size = cocos2d::CCDirector::sharedDirector()->getWinSize();
        auto pos = m_panel->getPosition();
        auto panelSize = m_panel->getContentSize();
        float halfW = panelSize.width * m_panel->getScaleX() * 0.5f;
        float halfH = panelSize.height * m_panel->getScaleY() * 0.5f;
        pos.x = std::max(halfW + 6.f, std::min(size.width - halfW - 6.f, pos.x));
        pos.y = std::max(halfH + 6.f, std::min(size.height - halfH - 6.f, pos.y));
        m_panel->setPosition(pos);
    }

public:
    static SonwareGuiLayer* create() {
        auto ret = new SonwareGuiLayer();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    bool init() override {
        if (!cocos2d::CCLayer::init()) return false;
        auto size = cocos2d::CCDirector::sharedDirector()->getWinSize();
        this->setContentSize(size);
        this->scheduleUpdate();

        // Compact dashboard. Scale it down on smaller game windows so the full
        // panel and its labels remain inside the visible screen area.
        constexpr float panelW = 228.f;
        constexpr float panelH = 98.f;
        m_panel = cocos2d::CCLayerColor::create({12, 18, 31, 238}, panelW, panelH);
        if (!m_panel) return true;
        m_panel->setAnchorPoint({0.5f, 0.5f});
        float availableW = std::max(1.f, size.width - 24.f);
        float availableH = std::max(1.f, size.height - 24.f);
        float uiScale = std::min(1.f, std::min(availableW / panelW, availableH / panelH));
        m_panel->setScale(uiScale);
        m_panel->setPosition({size.width * 0.5f, size.height * 0.5f});
        m_panel->setZOrder(9999);
        this->addChild(m_panel);
        s_sonwarePanel = m_panel;

        auto* accent = cocos2d::CCLayerColor::create({55, 205, 255, 255}, 3.f, panelH);
        if (accent) {
            accent->setAnchorPoint({0.f, 0.f});
            accent->setPosition({0.f, 0.f});
            m_panel->addChild(accent);
        }
        auto* divider = cocos2d::CCLayerColor::create({65, 100, 135, 190}, panelW - 20.f, 1.f);
        if (divider) {
            divider->setPosition({10.f, 33.f});
            m_panel->addChild(divider);
        }
        auto* title = cocos2d::CCLabelBMFont::create("SONWARE", "bigFont.fnt");
        if (title) {
            title->setAnchorPoint({0.f, 0.5f});
            title->setPosition({11.f, 81.f});
            title->setScale(0.42f);
            title->setColor({125, 220, 255});
            m_panel->addChild(title);
        }
        auto* cap = cocos2d::CCLabelBMFont::create("MAX 60", "bigFont.fnt");
        if (cap) {
            cap->setAnchorPoint({1.f, 0.5f});
            cap->setPosition({panelW - 10.f, 81.f});
            cap->setScale(0.27f);
            cap->setColor({255, 205, 115});
            m_panel->addChild(cap);
        }
        auto* subtitle = cocos2d::CCLabelBMFont::create("INPUT BUFFER  |  RIGHT ALT: HIDE", "bigFont.fnt");
        if (subtitle) {
            subtitle->setAnchorPoint({0.f, 0.5f});
            subtitle->setPosition({11.f, 63.f});
            subtitle->setScale(0.23f);
            subtitle->setColor({155, 175, 200});
            m_panel->addChild(subtitle);
        }
        s_bufferHud = cocos2d::CCLabelBMFont::create("STATUS  |  2 TICKS", "bigFont.fnt");
        if (s_bufferHud) {
            s_bufferHud->setAnchorPoint({0.f, 0.5f});
            s_bufferHud->setPosition({11.f, 46.f});
            s_bufferHud->setScale(0.29f);
            m_panel->addChild(s_bufferHud);
        }
        auto* buttonBg = cocos2d::CCLayerColor::create({24, 43, 63, 255}, panelW - 20.f, 23.f);
        if (buttonBg) {
            buttonBg->setPosition({10.f, 5.f});
            m_panel->addChild(buttonBg);
        }
        auto* buttonText = cocos2d::CCLabelBMFont::create("CYCLE PRESET  >", "bigFont.fnt");
        if (buttonText) {
            buttonText->setScale(0.31f);
            s_bufferButtonLabel = buttonText;
            auto* item = cocos2d::CCMenuItemLabel::create(buttonText, this, menu_selector(SonwareGuiLayer::onSonwareCycle));
            if (item) {
                auto* menu = cocos2d::CCMenu::createWithItem(item);
                if (menu) {
                    menu->setPosition({panelW * 0.5f, 17.f});
                    menu->setZOrder(1);
                    m_panel->addChild(menu);
                }
            }
        }

        // Dragging is handled by lightweight Windows mouse polling in update().
        // This avoids Cocos2d touch-listener APIs that are not available in this Geode build.
        refreshBufferHud();
        return true;
    }

    void update(float) override {
        // Right Alt toggles visibility on the key-down edge.
        bool rightAltDown = (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
        if (rightAltDown && !m_rightAltWasDown && m_panel) {
            m_panel->setVisible(!m_panel->isVisible());
        }
        m_rightAltWasDown = rightAltDown;

        // Poll the Windows mouse once per frame rather than using unavailable
        // cocos2d touch classes. This adds no scanning work to the buffer logic.
        bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        HWND hwnd = GetForegroundWindow();
        POINT cursor{};
        RECT client{};
        auto winSize = cocos2d::CCDirector::sharedDirector()->getWinSize();
        bool haveMouse = hwnd && GetCursorPos(&cursor) && ScreenToClient(hwnd, &cursor) &&
                         GetClientRect(hwnd, &client) &&
                         (client.right - client.left) > 0 && (client.bottom - client.top) > 0;

        if (haveMouse) {
            float clientW = static_cast<float>(client.right - client.left);
            float clientH = static_cast<float>(client.bottom - client.top);
            cocos2d::CCPoint mousePos{
                static_cast<float>(cursor.x) * winSize.width / clientW,
                (clientH - static_cast<float>(cursor.y)) * winSize.height / clientH
            };

            if (leftDown && !m_leftWasDown && m_panel && m_panel->isVisible()) {
                auto local = m_panel->convertToNodeSpace(mousePos);
                auto panelSize = m_panel->getContentSize();
                // Drag only from the title strip, keeping the preset button clickable.
                if (local.x >= 0.f && local.x <= panelSize.width &&
                    local.y >= panelSize.height - 30.f && local.y <= panelSize.height) {
                    m_dragging = true;
                    m_lastMouse = mousePos;
                }
            } else if (leftDown && m_dragging && m_panel) {
                auto pos = m_panel->getPosition();
                pos.x += mousePos.x - m_lastMouse.x;
                pos.y += mousePos.y - m_lastMouse.y;
                m_panel->setPosition(pos);
                m_lastMouse = mousePos;
                keepPanelOnScreen();
            }
        }

        if (!leftDown) m_dragging = false;
        m_leftWasDown = leftDown;
    }

    void onSonwareCycle(cocos2d::CCObject*) {
        s_sim.cyclePreset();
        refreshBufferHud();
    }

    void onExit() override {
        if (s_sonwarePanel == m_panel) {
            s_sonwarePanel = nullptr;
            s_bufferHud = nullptr;
            s_bufferButtonLabel = nullptr;
        }
        cocos2d::CCLayer::onExit();
    }
};

$on_mod(Loaded) {
    log::info("[SONWARE V2] Predictive Buffer loaded. Default performance preset: Balanced.");
    log::info("[SONWARE V2] Essential event logs are enabled; detailed input logs are disabled by default.");
}

class $modify(SonwareMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        if (auto* gui = SonwareGuiLayer::create()) this->addChild(gui, 9999);
        return true;
    }
};

class $modify(BufPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", -1000000);
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", -1000000);
        (void)self.setHookPriority("PlayLayer::flipGravity", -1000000);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        s_sim.init();
        // Centered, draggable GUI; also available in the main menu. Right Alt toggles it.
        if (auto* gui = SonwareGuiLayer::create()) this->addChild(gui, 9999);
        return true;
    }

    void destroyPlayer(PlayerObject* player, GameObject* gameObject) {
        if (s_sim.checkSimulationDeath(player)) return;

        auto* pl = PlayLayer::get();
        if (essentialLogsEnabled() && pl &&
            (player == pl->m_player1 || player == pl->m_player2)) {
            log::info("[SONWARE] Player death recorded.");
        }

        PlayLayer::destroyPlayer(player, gameObject);
    }

    void playEndAnimationToPos(cocos2d::CCPoint p0) {
        if (s_sim.isSimulating()) return;
        PlayLayer::playEndAnimationToPos(p0);
    }

    void flipGravity(PlayerObject* p0, bool p1, bool p2) {
        if (s_sim.isSimulating()) return;
        PlayLayer::flipGravity(p0, p1, p2);
    }

    void onQuit() {
        s_bufferHud = nullptr;
        s_bufferButtonLabel = nullptr;
        s_sonwarePanel = nullptr;
        PlayLayer::onQuit();
        s_sim.quit();
    }
};

class $modify(BufEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool unk) {
        bool res = LevelEditorLayer::init(level, unk);
        s_sim.quit();
        return res;
    }
};

class $modify(BufHardStreak, HardStreak) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("HardStreak::addPoint", -1000000);
    }

    void addPoint(cocos2d::CCPoint p0) {
        if (s_sim.isSimulating()) return;
        HardStreak::addPoint(p0);
    }
};

class $modify(BufEffectGameObject, EffectGameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("EffectGameObject::triggerObject", -1000000);
    }

    void triggerObject(GJBaseGameLayer* p0, int p1, const gd::vector<int>* p2) {
        if (s_sim.isSimulating()) return;
        EffectGameObject::triggerObject(p0, p1, p2);
    }
};

class $modify(BufGameObject, GameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GameObject::playShineEffect", -1000000);
    }

    void playShineEffect() {
        if (s_sim.isSimulating()) return;
        GameObject::playShineEffect();
    }
};

class $modify(BufPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", -1000000);
        (void)self.setHookPriority("PlayerObject::incrementJumps", -1000000);
        (void)self.setHookPriority("PlayerObject::ringJump", -1000000);
    }

    void playSpiderDashEffect(cocos2d::CCPoint from, cocos2d::CCPoint to) {
        if (s_sim.isSimulating()) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void incrementJumps() {
        if (s_sim.isSimulating()) return;
        PlayerObject::incrementJumps();
    }

    void update(float dt) {
        if (!s_sim.isSimulating()) {
            s_sim.onPlayerUpdate(this);
        }
        PlayerObject::update(dt);
        if (PlayLayer::get() && !s_sim.isSimulating()) {
            s_sim.setFrameDelta(dt);
        }
    }

    void ringJump(RingObject* p0, bool p1) {
        if (s_sim.isSimulating()) return;
        PlayerObject::ringJump(p0, p1);
    }
};

class $modify(BufBaseGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::canBeActivatedByPlayer", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::collisionCheckObjects", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::playerTouchedRing", -1000000);
    }

    bool canBeActivatedByPlayer(PlayerObject* p0, EffectGameObject* p1) {
        if (s_sim.isSimulating()) return false;
        return GJBaseGameLayer::canBeActivatedByPlayer(p0, p1);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!s_sim.isSimulating() && !s_sim.isFiringBuffered() && button == 1 && PlayLayer::get()) {
            if (!s_sim.interceptInput(this, down, isPlayer1)) {
                return;
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* vec, int objectsCount, float dt) {
        if (s_sim.isSimulating()) {
            gd::vector<GameObject*> extra;
            extra.reserve(objectsCount);
            for (int i = 0; i < objectsCount; i++) {
                GameObject* obj = vec->at(i);
                if (!obj) continue;
                if (obj->m_objectType == GameObjectType::Solid ||
                    obj->m_objectType == GameObjectType::Hazard ||
                    obj->m_objectType == GameObjectType::AnimatedHazard ||
                    obj->m_objectType == GameObjectType::CollisionObject ||
                    obj->m_objectType == GameObjectType::Slope) {
                    extra.push_back(obj);
                }
            }
            GJBaseGameLayer::collisionCheckObjects(player, &extra, extra.size(), dt);
            return;
        }
        GJBaseGameLayer::collisionCheckObjects(player, vec, objectsCount, dt);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (s_sim.isSimulating()) return;
        GJBaseGameLayer::playerTouchedRing(player, ring);
    }
};

