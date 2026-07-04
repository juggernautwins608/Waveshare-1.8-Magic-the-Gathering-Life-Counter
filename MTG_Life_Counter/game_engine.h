/*
  MTG Game Companion - Game Engine (UI build)
  ------------------------------------------------------------
  The board-independent "brain", refactored from the original
  soul_dial_engine.ino into a header so the LVGL UI can call the
  exact same functions. Pure logic: no setup()/loop()/Serial.

  A personal MTG tracker: YOUR life total, plus (in Commander)
  the commander damage dealt TO YOU by up to 3 opponents.
  ------------------------------------------------------------
*/
#pragma once

// ---------------- Formats / presets ----------------
struct Format { const char* name; int life; bool cmdrDamage; };
static Format FORMATS[] = {
  { "Standard",  20, false },
  { "Commander", 40, true  },
};
static const int NUM_FORMATS = sizeof(FORMATS) / sizeof(FORMATS[0]);
static int formatIndex = 1;   // default to Commander

// ---------------- Config ----------------
static const int LETHAL_CMDR_DAMAGE = 21;
static const int NUM_OPPONENTS       = 3;
static const int LIFE_MIN            = -99;
static const int LIFE_MAX            = 999;   // life can climb far past the start

// ---------------- Game state ----------------
static int  startingLife         = 40;    // from chosen format (or custom)
static bool trackCommanderDamage = true;  // off for Standard, on for Commander
static int  myLife = 40;
static int  cmdrDamage[NUM_OPPONENTS] = {0, 0, 0};   // damage dealt TO YOU
static const char* oppName[NUM_OPPONENTS] = {"Opp 1", "Opp 2", "Opp 3"};
// Optional counters on YOU (0 = Poison, 1 = Energy, 2 = Storm). counterLethal[k]
// of 0 means the counter has no death threshold. Poison is enabled by default.
static const int NUM_COUNTERS = 3;
static int  counterVal[NUM_COUNTERS]     = {0, 0, 0};
static bool counterEnabled[NUM_COUNTERS] = {true, false, false};
static const int counterLethal[NUM_COUNTERS] = {10, 0, 0};
// selection: 0 = You (life), 1..NUM_OPPONENTS = opponent cmdr damage,
//            COUNTER_SEL_BASE+k = counter k
static const int COUNTER_SEL_BASE = NUM_OPPONENTS + 1;
// Commander tax (extra mana cost; permanent, Commander only; steps of 2)
static int  cmdrTax = 0;
static const int TAX_SEL = COUNTER_SEL_BASE + NUM_COUNTERS;
static int  selected = 0;
static int  combatAmount = 0;

// ---------------- Small helper ----------------
static inline int clampInt(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// ---------------- Setup / format actions ----------------
static inline void newGame() {
  myLife = startingLife;
  for (int i = 0; i < NUM_OPPONENTS; i++) cmdrDamage[i] = 0;
  for (int k = 0; k < NUM_COUNTERS; k++) counterVal[k] = 0;  // keep enabled set
  cmdrTax = 0;
  selected = 0;
  combatAmount = 0;
}

static inline void applyFormat(int idx) {
  formatIndex = ((idx % NUM_FORMATS) + NUM_FORMATS) % NUM_FORMATS;
  startingLife = FORMATS[formatIndex].life;
  trackCommanderDamage = FORMATS[formatIndex].cmdrDamage;
  newGame();
}

static inline void setCustomLife(int n) {
  startingLife = clampInt(n, 1, LIFE_MAX);
  newGame();   // keeps the current commander-damage setting
}

// shows the preset name, or "Custom" if starting life was changed by hand
static inline const char* formatLabel() {
  return (startingLife == FORMATS[formatIndex].life)
           ? FORMATS[formatIndex].name : "Custom";
}

// ---------------- Core actions (the UI calls these) ----------------
static inline void adjustSelected(int delta) {
  if (selected == 0) {
    myLife = clampInt(myLife + delta, LIFE_MIN, LIFE_MAX);  // no cap at start
  } else if (selected <= NUM_OPPONENTS) {
    // Adjusting an opponent's commander damage = that opponent hitting you:
    // their damage rises AND your life drops by the same amount. Turning the
    // damage back down restores the life (the two stay coupled & reversible).
    int i = selected - 1;
    int before  = cmdrDamage[i];
    cmdrDamage[i] = clampInt(before + delta, 0, 99);
    int applied = cmdrDamage[i] - before;                  // real change after clamp
    myLife = clampInt(myLife - applied, LIFE_MIN, LIFE_MAX);
  } else if (selected == TAX_SEL) {
    cmdrTax = clampInt(cmdrTax + delta * 2, 0, 99);        // commander tax, steps of 2
  } else {
    int k = selected - COUNTER_SEL_BASE;                  // optional counter k
    if (k >= 0 && k < NUM_COUNTERS)
      counterVal[k] = clampInt(counterVal[k] + delta, 0, 99);
  }
}

// Advance selection to the next valid target: life is always valid; opponents
// only in Commander; counters only when enabled. (loop-until-valid)
static inline void cycleSelection() {
  const int total = COUNTER_SEL_BASE + NUM_COUNTERS + 1;   // life + opps + counters + tax
  for (int step = 0; step < total; step++) {
    selected = (selected + 1) % total;
    if (selected == 0) return;                             // life
    if (selected <= NUM_OPPONENTS) {                       // opponent
      if (trackCommanderDamage) return;
      continue;
    }
    if (selected == TAX_SEL) {                             // commander tax (Commander only)
      if (trackCommanderDamage) return;
      continue;
    }
    if (counterEnabled[selected - COUNTER_SEL_BASE]) return;  // enabled counter
  }
  selected = 0;
}

// An opponent's commander connects for `amount`:
// you lose that much life AND take that much commander damage from them.
static inline void combatHit(int oppIndex, int amount) {
  myLife = clampInt(myLife - amount, LIFE_MIN, LIFE_MAX);
  if (!trackCommanderDamage) return;
  if (oppIndex < 0 || oppIndex >= NUM_OPPONENTS) return;
  cmdrDamage[oppIndex] = clampInt(cmdrDamage[oppIndex] + amount, 0, 99);
}

static inline bool isDead() {
  if (myLife <= 0) return true;
  for (int k = 0; k < NUM_COUNTERS; k++)
    if (counterLethal[k] > 0 && counterVal[k] >= counterLethal[k]) return true;
  if (trackCommanderDamage)
    for (int i = 0; i < NUM_OPPONENTS; i++)
      if (cmdrDamage[i] >= LETHAL_CMDR_DAMAGE) return true;
  return false;
}
