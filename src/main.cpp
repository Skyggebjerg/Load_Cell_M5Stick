/*
 * ============================================================
 *  M5StickC Plus — HX711 Weight Scale
 *  PlatformIO / Arduino framework
 *
 *  Hardware connections — Grove port (HY2.0-4P, PORT.CUSTOM):
 *
 *   Weight Unit  │ Grove cable │ M5StickC Plus Grove pin
 *  ─────────────────────────────────────────────────────────
 *   GND          │ Black       │ GND
 *   5V           │ Red         │ 5V  (sourced via AXP192)
 *   CLK          │ Yellow      │ G32
 *   DAT          │ White       │ G33
 *
 *  Button mapping:
 *    BtnA (front, large) – hold 2 s → calibrate to 1 kg reference
 *    BtnB (side, small)  – short press → tare / zero
 * ============================================================
 */

#include <M5Unified.h>    // Must come before M5GFX when used together
#include <M5GFX.h>
#include <HX711_ADC.h>
#include <Preferences.h>

// ─── Pin definitions (Grove port / PORT.CUSTOM) ─────────────
#define HX711_SCK_PIN    32   // G32 – Yellow wire – CLK to HX711
#define HX711_DOUT_PIN   33   // G33 – White wire  – DAT from HX711

// ─── Calibration ────────────────────────────────────────────
#define CAL_REFERENCE_G     1000.0f // Reference weight in grams for calibration (e.g. 1000 g = 1 kg)
#define DEFAULT_CAL_FACTOR  263.0f
#define NVS_NAMESPACE       "scale"
#define NVS_KEY_CAL         "cal_factor"

// ─── Filter / stability settings ────────────────────────────
#define HX711_SAMPLES       32
#define EMA_ALPHA           0.15f
#define STABLE_THRESHOLD_G  3.0f
#define STABLE_COUNT_REQ    12
#define AUTO_TARE_THRESHOLD 8.0f
#define AUTO_TARE_SECONDS   8
#define DISPLAY_REFRESH_MS  80

// ─── Canvas — attached to M5.Display, NOT a standalone M5GFX ─
// M5.Display is initialised by M5.begin(); a separate M5GFX
// object would never be wired to the hardware and stays blank.
M5Canvas canvas(&M5.Display);

HX711_ADC   scale(HX711_DOUT_PIN, HX711_SCK_PIN);
Preferences prefs;

float    calFactor       = DEFAULT_CAL_FACTOR;
float    emaWeight       = 0.0f;
float    displayedWeight = 0.0f;
int      stableCount     = 0;
bool     isStable        = false;

uint32_t autoTareTimer   = 0;
uint32_t lastDisplayMs   = 0;
uint32_t uptimeSeconds   = 0;
uint32_t lastUptimeTick  = 0;

bool     btnAWasDown     = false;
uint32_t btnAHoldStart   = 0;
#define  CAL_HOLD_MS     2000

bool inCalMode = false;

// ─── Display dimensions (set after M5.begin()) ───────────────
int DISP_W = 240;
int DISP_H = 135;

// ─── Colour palette (16-bit RGB565) ─────────────────────────
static const uint32_t COL_BG      = 0x0000; // Dark blue background. to have black background change to 0x0000
static const uint32_t COL_PANEL   = 0x0000; 
static const uint32_t COL_ACCENT  = 0xFFE0; //
static const uint32_t COL_GREEN   = 0xFFE0;
static const uint32_t COL_YELLOW  = 0xFFE0; //
static const uint32_t COL_RED     = 0xF800;
static const uint32_t COL_WHITE   = 0xFFFF;
static const uint32_t COL_GREY    = 0xFFE0;
static const uint32_t COL_DKGREY  = 0xFFE0;

// ─── NVS helpers ─────────────────────────────────────────────
void loadCalibration() {
    prefs.begin(NVS_NAMESPACE, true);
    calFactor = prefs.getFloat(NVS_KEY_CAL, DEFAULT_CAL_FACTOR);
    prefs.end();
}
void saveCalibration(float factor) {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putFloat(NVS_KEY_CAL, factor);
    prefs.end();
    calFactor = factor;
}

// ─── Battery ─────────────────────────────────────────────────
float getBattVolts() { return M5.Power.getBatteryVoltage() / 1000.0f; }
int   getBattPct()   {
    float v = getBattVolts();
    int p = (int)((v - 3.5f) / (4.2f - 3.5f) * 100.0f);
    return (p < 0) ? 0 : (p > 100) ? 100 : p;
}

// ─── Temporary full-screen message ──────────────────────────
void showMessage(const char* line1, const char* line2, uint32_t col, uint32_t ms) {
    canvas.fillSprite(COL_BG);
    canvas.fillRoundRect(10, 20, DISP_W - 20, DISP_H - 40, 8, COL_PANEL);
    canvas.setTextSize(2);
    canvas.setTextColor(col, COL_PANEL);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString(line1, DISP_W / 2, 55);
    canvas.setTextSize(1);
    canvas.setTextColor(COL_GREY, COL_PANEL);
    canvas.drawString(line2, DISP_W / 2, 85);
    canvas.pushSprite(0, 0);
    delay(ms);
}

// ─── Tare ────────────────────────────────────────────────────
void doTare() {
    showMessage("TARING...", "Keep scale empty", COL_YELLOW, 800);
    scale.tare();
    emaWeight     = 0.0f;
    stableCount   = 0;
    isStable      = false;
    autoTareTimer = millis();
    showMessage("TARED", "Zero set", COL_GREEN, 600);
}

// ─── Calibration ─────────────────────────────────────────────
void startCalibration() {
    showMessage("CAL MODE", "Remove all weight", COL_YELLOW, 1500);
    scale.tare();
    inCalMode = true;
    showMessage("Place 1 kg", "Then hold Btn A", COL_ACCENT, 1000);
}
void confirmCalibration() {
    float newFactor = scale.getNewCalibration(CAL_REFERENCE_G);
    saveCalibration(newFactor);
    scale.setCalFactor(newFactor);
    inCalMode = false;
    char msg[32];
    snprintf(msg, sizeof(msg), "Factor: %.1f", newFactor);
    showMessage("CAL DONE", msg, COL_GREEN, 1500);
    doTare();
}

// ─── Draw main screen ─────────────────────────────────────────
void drawScreen(float weightG, bool stable, bool calMode, const char* statusMsg) {
    canvas.fillSprite(COL_BG);

    // ── Top status bar ────────────────────────────────────────
    canvas.fillRect(0, 0, DISP_W, 18, COL_PANEL);

    char ubuf[16];
    snprintf(ubuf, sizeof(ubuf), "%02lu:%02lu:%02lu",
             uptimeSeconds / 3600,
             (uptimeSeconds % 3600) / 60,
             uptimeSeconds % 60);
    canvas.setTextSize(1);
    canvas.setTextDatum(ML_DATUM);
    canvas.setTextColor(COL_DKGREY, COL_PANEL);
    canvas.drawString(ubuf, 4, 9);

    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(COL_ACCENT, COL_PANEL);
    canvas.drawString("WEIGHT SCALE", DISP_W / 2, 9);

    int   pct = getBattPct();
    float v   = getBattVolts();
    char bbuf[20];
    snprintf(bbuf, sizeof(bbuf), "%.2fV %d%%", v, pct);
    uint32_t batCol = (pct > 30) ? COL_GREEN : (pct > 15) ? COL_YELLOW : COL_RED;
    canvas.setTextDatum(MR_DATUM);
    canvas.setTextColor(batCol, COL_PANEL);
    canvas.drawString(bbuf, DISP_W - 4, 9);

    canvas.drawFastHLine(0, 18, DISP_W, COL_ACCENT);

    // ── Main weight panel ─────────────────────────────────────
    canvas.fillRoundRect(4, 22, DISP_W - 8, 68, 6, COL_PANEL);

    if (calMode) {
        canvas.setTextSize(1);
        canvas.setTextDatum(MC_DATUM);
        canvas.setTextColor(COL_YELLOW, COL_PANEL);
        canvas.drawString("CALIBRATING", DISP_W / 2, 38);
        canvas.setTextSize(2);
        canvas.setTextColor(COL_WHITE, COL_PANEL);
        canvas.drawString("Place 1 kg", DISP_W / 2, 60);
        canvas.setTextSize(1);
        canvas.setTextColor(COL_GREY, COL_PANEL);
        canvas.drawString("Hold Btn A to confirm", DISP_W / 2, 80);
    } else {
        bool useKg = (weightG >= 1000.0f);
        char wbuf[16];
        if (useKg)
            snprintf(wbuf, sizeof(wbuf), "%.3f", weightG / 1000.0f);
        else
            snprintf(wbuf, sizeof(wbuf), "%.1f", weightG);

        uint32_t wCol = (weightG < 1.0f) ? COL_RED :
                        useKg            ? COL_YELLOW  : COL_WHITE;

        canvas.setTextSize(4);
        canvas.setTextColor(wCol, COL_PANEL);
        canvas.setTextDatum(MR_DATUM);
        canvas.drawString(wbuf, DISP_W - 30, 55);

        canvas.setTextSize(2);
        canvas.setTextColor(COL_ACCENT, COL_PANEL);
        canvas.setTextDatum(ML_DATUM);
        canvas.drawString(useKg ? "kg" : "g", DISP_W - 28, 55);

        uint32_t dotCol = stable ? COL_GREEN : COL_DKGREY;
        canvas.fillCircle(14, 57, 5, dotCol);
        canvas.setTextSize(1);
        canvas.setTextColor(dotCol, COL_PANEL);
        canvas.setTextDatum(ML_DATUM);
        canvas.drawString(stable ? "STABLE" : "...", 22, 57);
    }

    // ── Bottom info bar ───────────────────────────────────────
    canvas.drawFastHLine(0, 92, DISP_W, COL_DKGREY);
    canvas.fillRoundRect(4, 95, DISP_W - 8, 37, 6, COL_PANEL);

    char cbuf[24];
    snprintf(cbuf, sizeof(cbuf), "Cal:%.1f", calFactor);
    canvas.setTextSize(1);
    canvas.setTextColor(COL_DKGREY, COL_PANEL);
    canvas.setTextDatum(ML_DATUM);
    canvas.drawString(cbuf, 10, 106);

    canvas.setTextColor(COL_GREY, COL_PANEL);
    canvas.setTextDatum(MR_DATUM);
    canvas.drawString(statusMsg, DISP_W - 8, 106);

    canvas.setTextColor(COL_ACCENT, COL_PANEL);
    canvas.setTextDatum(ML_DATUM);
    canvas.drawString("[A]2s=Cal", 10, 122);
    canvas.setTextDatum(MR_DATUM);
    canvas.drawString("[B]=Tare", DISP_W - 8, 122);

    canvas.pushSprite(0, 0);
}

// ─── Setup ───────────────────────────────────────────────────
void setup() {
    // M5.begin() initialises the display, buttons, power chip etc.
    // Do NOT call display.begin() separately — M5.Display is ready after this.
    auto cfg = M5.config();
    M5.begin(cfg);

    // Read actual display dimensions (accounts for rotation)
    M5.Display.setRotation(1);          // landscape
    M5.Display.setBrightness(160);
    DISP_W = M5.Display.width();
    DISP_H = M5.Display.height();

    // Create the full-screen canvas attached to M5.Display
    canvas.setColorDepth(16);
    canvas.createSprite(DISP_W, DISP_H);
    canvas.setTextFont(0);              // built-in font

    // Boot splash — push directly so it's visible immediately
    canvas.fillSprite(COL_BG);
    canvas.setTextSize(2);
    canvas.setTextColor(COL_ACCENT, COL_BG);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString("Weight Scale", DISP_W / 2, 40);
    canvas.setTextSize(1);
    canvas.setTextColor(COL_GREY, COL_BG);
    canvas.drawString("Initialising HX711...", DISP_W / 2, 68);
    canvas.drawString("Grove: G32=CLK  G33=DAT", DISP_W / 2, 84);
    canvas.pushSprite(0, 0);

    loadCalibration();

    scale.begin();
    scale.start(2000, true);    // 2 s warm-up + built-in tare

    if (scale.getTareTimeoutFlag()) {
        showMessage("HX711 ERROR", "Check Grove wiring!", COL_RED, 3000);
    }

    scale.setCalFactor(calFactor);
    scale.setSamplesInUse(HX711_SAMPLES);

    emaWeight      = 0.0f;
    autoTareTimer  = millis();
    lastDisplayMs  = millis();
    lastUptimeTick = millis();

    showMessage("READY", "Place weight on scale", COL_GREEN, 800);
}

// ─── Loop ────────────────────────────────────────────────────
void loop() {
    M5.update();
    uint32_t now = millis();

    // Uptime
    if (now - lastUptimeTick >= 1000) {
        uptimeSeconds++;
        lastUptimeTick = now;
    }

    // Non-blocking HX711 read
    if (scale.dataWaitingAsync()) {
        scale.updateAsync();
        float reading = scale.getData();
        emaWeight = EMA_ALPHA * reading + (1.0f - EMA_ALPHA) * emaWeight;
    }

    // Clamp noise around zero
    float w = emaWeight;
    if (w > -0.5f && w < 0.5f) w = 0.0f;
    if (w < 0.0f)               w = 0.0f;

    // Stability detection
    float delta = fabsf(w - displayedWeight);
    if (delta < STABLE_THRESHOLD_G) {
        if (stableCount < STABLE_COUNT_REQ + 10) stableCount++;
    } else {
        stableCount = 0;
    }
    isStable        = (stableCount >= STABLE_COUNT_REQ);
    displayedWeight = w;

    // Auto-tare when empty
    if (!inCalMode) {
        if (w < AUTO_TARE_THRESHOLD) {
            if ((now - autoTareTimer) >= (uint32_t)(AUTO_TARE_SECONDS * 1000)) {
                scale.tare();
                emaWeight     = 0.0f;
                autoTareTimer = now;
            }
        } else {
            autoTareTimer = now;
        }
    }

    // Button A – hold to calibrate
    if (M5.BtnA.isPressed()) {
        if (!btnAWasDown) {
            btnAWasDown   = true;
            btnAHoldStart = now;
        }
        if ((now - btnAHoldStart) >= CAL_HOLD_MS) {
            if (!inCalMode) startCalibration();
            else            confirmCalibration();
            btnAWasDown = false;
        }
    } else {
        btnAWasDown = false;
    }

    // Button B – manual tare
    if (M5.BtnB.wasPressed() && !inCalMode) {
        doTare();
        autoTareTimer = now;
    }

    // Status string
    const char* statusMsg =
        inCalMode                 ? "Place 1kg, hold A" :
        (isStable && w > 1.0f)   ? "Stable"            :
        (w < AUTO_TARE_THRESHOLD) ? "Empty"             :
                                    "Measuring...";

    // Refresh display
    if ((now - lastDisplayMs) >= DISPLAY_REFRESH_MS) {
        drawScreen(displayedWeight, isStable, inCalMode, statusMsg);
        lastDisplayMs = now;
    }

    delay(5);
}