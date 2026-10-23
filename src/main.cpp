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

    void selectPreset(int index) {
        constexpr int normalTicks[] = {2, 4, 8, 16, 24, 32, 48, 54, 60, 0};
        constexpr int waveTicks[]   = {2, 4, 8,  8, 24, 32, 48, 54, 60, 0};
        index = std::max(0, std::min(9, index));
        m_presetIndex = index;
        m_waitTicks = -1;
        m_queuedOpposite = false;
        m_enabled = index != 9;
        m_maxTicksNormal = normalTicks[index];
        m_maxTicksWave = waveTicks[index];
        if (essentialLogsEnabled()) {
            if (m_enabled) log::info("[SONWARE] Stage {} selected: {} ticks normal / {} ticks wave.",
                index + 1, m_maxTicksNormal, m_maxTicksWave);
            else log::info("[SONWARE] Buffer OFF; raw input enabled.");
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
static cocos2d::CCLabelTTF* s_bufferHud = nullptr;
static cocos2d::CCLabelBMFont* s_bufferButtonLabel = nullptr;
static cocos2d::CCLayerColor* s_sonwarePanel = nullptr;

// Windows console is allocated once and hidden/shown on demand.
static HWND s_consoleWindow = nullptr;
static bool s_consoleVisible = false;
static FILE* s_consoleOut = nullptr;
static FILE* s_consoleErr = nullptr;

static void sonwareConsoleLog(const char* message) {
    if (s_consoleWindow) {
        std::printf("[SONWARE] %s\n", message);
        std::fflush(stdout);
    }
}

static bool toggleSonwareConsole() {
    if (!s_consoleWindow) {
        if (!AllocConsole()) return false;
        s_consoleWindow = GetConsoleWindow();
        freopen_s(&s_consoleOut, "CONOUT$", "w", stdout);
        freopen_s(&s_consoleErr, "CONOUT$", "w", stderr);
        SetConsoleTitleW(L"SONWARE - Debug Console");
        sonwareConsoleLog("Console ready. Stage changes appear here.");
    }
    s_consoleVisible = !s_consoleVisible;
    ShowWindow(s_consoleWindow, s_consoleVisible ? SW_SHOW : SW_HIDE);
    return true;
}

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

// SONWARE desktop-inspired dashboard. System Segoe UI font, not Geometry Dash bitmap fonts.
class SonwareGuiLayer : public cocos2d::CCLayer {
protected:
    cocos2d::CCLayerColor* m_panel = nullptr;
    bool m_rightAltWasDown = false;
    bool m_dragging = false;
    bool m_leftWasDown = false;
    cocos2d::CCPoint m_lastMouse = {0.f, 0.f};
    static constexpr float W = 490.f;
    static constexpr float H = 286.f;

    void rect(float x, float y, float w, float h, cocos2d::ccColor4B color, int z = 0) {
        auto* node = cocos2d::CCLayerColor::create(color, w, h);
        if (node) { node->setPosition({x, y}); m_panel->addChild(node, z); }
    }

    cocos2d::CCLabelTTF* label(const char* str, float x, float y, float fontSize,
                               cocos2d::ccColor3B color, float maxWidth = 0.f,
                               cocos2d::CCPoint anchor = {0.f, 0.5f}) {
        auto* node = cocos2d::CCLabelTTF::create(str, "Segoe UI", fontSize);
        if (!node) return nullptr;
        node->setAnchorPoint(anchor);
        node->setPosition({x, y});
        node->setColor(color);
        if (maxWidth > 0.f && node->getContentSize().width > maxWidth)
            node->setScale(maxWidth / node->getContentSize().width);
        m_panel->addChild(node, 5);
        return node;
    }

    void button(const char* caption, float x, float y, float w, float h,
                cocos2d::SEL_MenuHandler cb, bool accent = false) {
        rect(x, y, w, h, accent ? cocos2d::ccc4(90, 49, 155, 255) : cocos2d::ccc4(38, 32, 58, 255), 1);
        rect(x, y + h - 1.f, w, 1.f, accent ? cocos2d::ccc4(171, 116, 252, 255) : cocos2d::ccc4(72, 61, 99, 255), 2);
        auto* title = cocos2d::CCLabelTTF::create(caption, "Segoe UI", 12.f);
        if (!title) return;
        title->setColor({244, 239, 255});
        if (title->getContentSize().width > w - 14.f)
            title->setScale((w - 14.f) / title->getContentSize().width);
        auto* item = cocos2d::CCMenuItemLabel::create(title, this, cb);
        if (!item) return;
        auto* menu = cocos2d::CCMenu::createWithItem(item);
        if (!menu) return;
        menu->setPosition({x + w * 0.5f, y + h * 0.5f});
        m_panel->addChild(menu, 6);
    }

    void keepPanelOnScreen() {
        if (!m_panel) return;
        auto size = cocos2d::CCDirector::sharedDirector()->getWinSize();
        auto pos = m_panel->getPosition();
        float halfW = W * m_panel->getScaleX() * .5f;
        float halfH = H * m_panel->getScaleY() * .5f;
        pos.x = std::max(halfW + 4.f, std::min(size.width - halfW - 4.f, pos.x));
        pos.y = std::max(halfH + 4.f, std::min(size.height - halfH - 4.f, pos.y));
        m_panel->setPosition(pos);
    }

public:
    static SonwareGuiLayer* create() {
        auto* ret = new SonwareGuiLayer();
        if (ret && ret->init()) { ret->autorelease(); return ret; }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    bool init() override {
        if (!cocos2d::CCLayer::init()) return false;
        auto win = cocos2d::CCDirector::sharedDirector()->getWinSize();
        this->setContentSize(win);
        this->scheduleUpdate();
        m_panel = cocos2d::CCLayerColor::create({14, 13, 23, 255}, W, H);
        if (!m_panel) return false;
        m_panel->ignoreAnchorPointForPosition(false);
        m_panel->setAnchorPoint({.5f, .5f});
        float scale = std::min(1.f, std::min((win.width - 12.f) / W, (win.height - 12.f) / H));
        m_panel->setScale(std::max(.1f, scale));
        m_panel->setPosition({win.width * .5f, win.height * .5f});
        this->addChild(m_panel, 9999);
        s_sonwarePanel = m_panel;

        // Desktop-style opaque frame, purple border and slim title bar.
        rect(0, 0, W, 1, {129, 80, 210, 255});
        rect(0, H-1, W, 1, {129, 80, 210, 255});
        rect(0, 0, 1, H, {129, 80, 210, 255});
        rect(W-1, 0, 1, H, {129, 80, 210, 255});
        rect(1, H-37, W-2, 36, {23, 19, 37, 255});
        rect(1, 1, 116, H-39, {21, 18, 33, 255});
        rect(117, 1, 1, H-39, {53, 42, 79, 255});
        rect(0, H-38, W, 1, {65, 47, 100, 255});
        rect(12, H-28, 3, 16, {166, 105, 255, 255});
        label("SONWARE", 22, H-20, 17, {241, 229, 255}, 150);
        label("PREDICTIVE BUFFER", 144, H-20, 10, {149, 134, 174}, 180);
        label("Right Alt  •  Hide / Show", W-12, H-20, 9, {161, 146, 188}, 126, {1.f,.5f});

        label("NAVIGATION", 14, 221, 9, {128, 113, 157}, 95);
        rect(8, 183, 100, 28, {63, 39, 104, 255});
        rect(8, 183, 3, 28, {172, 111, 255, 255});
        label("Dashboard", 19, 197, 12, {244, 236, 255}, 84);
        label("Stages", 19, 163, 11, {179, 168, 198}, 84);
        label("Console", 19, 133, 11, {179, 168, 198}, 84);
        rect(13, 55, 90, 1, {60, 48, 82, 255});
        label("SONWARE", 14, 37, 10, {164, 114, 239}, 90);
        label("Windows  •  Geode", 14, 21, 9, {136, 125, 159}, 90);

        label("Stage control", 133, 226, 18, {239, 233, 250}, 195);
        label("Choose a simulation preset", 133, 206, 10, {152, 140, 174}, 230);
        // Six equal tiles, all captions centered and auto-fit to their tiles.
        constexpr float bw = 106.f;
        constexpr float bh = 40.f;
        button("STAGE 1", 133, 150, bw, bh, menu_selector(SonwareGuiLayer::onStage1));
        button("STAGE 2", 247, 150, bw, bh, menu_selector(SonwareGuiLayer::onStage2));
        button("STAGE 3", 361, 150, bw, bh, menu_selector(SonwareGuiLayer::onStage3));
        button("STAGE 4", 133, 101, bw, bh, menu_selector(SonwareGuiLayer::onStage4));
        button("EXTREME", 247, 101, bw, bh, menu_selector(SonwareGuiLayer::onExtreme), true);
        button("CYCLE PRESET", 361, 101, bw, bh, menu_selector(SonwareGuiLayer::onSonwareCycle));

        rect(133, 84, 334, 1, {62, 49, 87, 255});
        s_bufferHud = label("STATUS  |  2 TICKS", 133, 65, 11, {139, 238, 189}, 318);
        label("Orb helper / Auto-click: unavailable", 133, 46, 9, {143, 130, 161}, 204);
        button("OPEN CONSOLE", 343, 18, 124, 38, menu_selector(SonwareGuiLayer::onConsole), true);
        label("Drag the top bar to move", 133, 20, 9, {134, 122, 155}, 190);
        refreshBufferHud();
        return true;
    }

    void update(float) override {
        bool rightAltDown = (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
        if (rightAltDown && !m_rightAltWasDown && m_panel) {
            m_panel->setVisible(!m_panel->isVisible());
            m_dragging = false;
        }
        m_rightAltWasDown = rightAltDown;

        bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        HWND hwnd = GetForegroundWindow();
        POINT cursor{};
        RECT client{};
        auto winSize = cocos2d::CCDirector::sharedDirector()->getWinSize();
        DWORD pid = 0;
        if (hwnd) GetWindowThreadProcessId(hwnd, &pid);
        bool haveMouse = hwnd && pid == GetCurrentProcessId() && GetCursorPos(&cursor) &&
            ScreenToClient(hwnd, &cursor) && GetClientRect(hwnd, &client) &&
            client.right > client.left && client.bottom > client.top;
        if (haveMouse && m_panel && m_panel->isVisible()) {
            float cw = static_cast<float>(client.right - client.left);
            float ch = static_cast<float>(client.bottom - client.top);
            cocos2d::CCPoint mousePos{cursor.x * winSize.width / cw,
                                      (ch - cursor.y) * winSize.height / ch};
            if (leftDown && !m_leftWasDown) {
                auto local = m_panel->convertToNodeSpace(mousePos);
                if (local.x >= 0.f && local.x <= W && local.y >= H - 37.f && local.y <= H) {
                    m_dragging = true;
                    m_lastMouse = mousePos;
                }
            } else if (leftDown && m_dragging) {
                auto pos = m_panel->getPosition();
                pos.x += mousePos.x - m_lastMouse.x;
                pos.y += mousePos.y - m_lastMouse.y;
                m_panel->setPosition(pos);
                m_lastMouse = mousePos;
                keepPanelOnScreen();
            }
        }
        if (!leftDown || !haveMouse) m_dragging = false;
        m_leftWasDown = leftDown;
    }

    void onConsole(cocos2d::CCObject*) {
        if (!toggleSonwareConsole()) log::error("[SONWARE] Could not open Windows console");
    }
    void onStage1(cocos2d::CCObject*) { s_sim.selectPreset(0); refreshBufferHud(); sonwareConsoleLog("Selected Stage1"); }
    void onStage2(cocos2d::CCObject*) { s_sim.selectPreset(1); refreshBufferHud(); sonwareConsoleLog("Selected Stage2"); }
    void onStage3(cocos2d::CCObject*) { s_sim.selectPreset(2); refreshBufferHud(); sonwareConsoleLog("Selected Stage3"); }
    void onStage4(cocos2d::CCObject*) { s_sim.selectPreset(3); refreshBufferHud(); sonwareConsoleLog("Selected Stage4"); }
    void onExtreme(cocos2d::CCObject*) { s_sim.selectPreset(8); refreshBufferHud(); sonwareConsoleLog("Selected Extreme"); }
    void onSonwareCycle(cocos2d::CCObject*) { s_sim.cyclePreset(); refreshBufferHud(); sonwareConsoleLog("Cycled buffer stage"); }
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
    log::info("[SONWARE V2] Predictive Buffer loaded. Dashboard loaded; Balanced performance preset active.");
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

