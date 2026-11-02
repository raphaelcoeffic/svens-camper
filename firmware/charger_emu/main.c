// Westfalia charger emulator for the "smart LIN / TTL" adapter board (STM32F103, TJA1021).
// Behaviour follows ../../FINDINGS.md, "Minimal emulator". Whether this unlocks heating is untested.
//
// Pins: PA2/PA3 USART2 to the TJA1021 TXD/RXD (LIN, 19200 8N1). PA9/PA10 USART1 console (57600 8N1).
// PA11 SLP_N, held high by the board: left alone. PB9 1k master pull-up switch: kept low, as the
// camper's controller is the master. PB8 drives the RESET pad (inverted): kept low. PA15 STA LED.
//
// Everything is polled from one loop. Interrupts stay masked and only wake the loop from WFI, so
// there are no handlers. The independent watchdog resets a hung loop.
//
// Battery protection: ID 22, read passively, gives the voltage. Below cut_mv for cut_s, or with
// ID 22 lost, it emulates a mains loss as seen in the traces: 00 01 for silent_ms, then silence.
// Above rec_mv for rec_s, it comes back as the real charger does, starting at status 01.
//
// The console's "help" lists the commands. Settings live in the last flash page; "mode" and the
// master pull-up are never saved, so a power cycle always returns to normal operation.
#include <stdint.h>

#define VERSION "1.1"

#define R(a) (*(volatile uint32_t *)(a))

// Peripheral base addresses
#define TIM2 0x40000000u
#define IWDG 0x40003000u
#define USART2 0x40004400u
#define AFIO_MAPR 0x40010004u
#define GPIOA 0x40010800u
#define GPIOB 0x40010C00u
#define USART1 0x40013800u
#define DMA1_CH5 0x40020058u
#define RCC 0x40021000u
#define FLASH 0x40022000u

// Core registers
#define SYST_CSR 0xE000E010u
#define NVIC_ISER0 0xE000E100u
#define NVIC_ISER1 0xE000E104u
#define NVIC_ICPR0 0xE000E280u
#define NVIC_ICPR1 0xE000E284u
#define SCB_VTOR 0xE000ED08u
#define SCB_AIRCR 0xE000ED0Cu

// Register offsets
#define RCC_CR 0x00u
#define RCC_CFGR 0x04u
#define RCC_AHBENR 0x14u
#define RCC_APB2ENR 0x18u
#define RCC_APB1ENR 0x1Cu

#define GPIO_CRL 0x00u
#define GPIO_CRH 0x04u
#define GPIO_BSRR 0x10u

#define USART_SR 0x00u
#define USART_DR 0x04u
#define USART_BRR 0x08u
#define USART_CR1 0x0Cu
#define USART_CR2 0x10u
#define USART_CR3 0x14u

#define TIM_CR1 0x00u
#define TIM_DIER 0x0Cu
#define TIM_SR 0x10u
#define TIM_EGR 0x14u
#define TIM_CNT 0x24u
#define TIM_PSC 0x28u
#define TIM_ARR 0x2Cu
#define TIM_CCR1 0x34u

#define DMA_CCR 0x00u
#define DMA_CNDTR 0x04u
#define DMA_CPAR 0x08u
#define DMA_CMAR 0x0Cu

#define IWDG_KR 0x00u
#define IWDG_PR 0x04u
#define IWDG_RLR 0x08u

#define FLASH_ACR 0x00u
#define FLASH_KEYR 0x04u
#define FLASH_SR 0x0Cu
#define FLASH_CR 0x10u
#define FLASH_AR 0x14u

#define FLASH_START 0x08000000u

#define SR_FE (1u << 1)
#define SR_NE (1u << 2)
#define SR_ORE (1u << 3)
#define SR_RXNE (1u << 5)
#define SR_TC (1u << 6)
#define SR_TXE (1u << 7)
#define SR_LBD (1u << 8)
#define CR1_SBK 1u

// Survives a reset, as it's above the stack and outside .bss: asks the next boot to enter ROM.
#define ROM_FLAG (*(volatile uint32_t *)0x20004FFCu)
#define ROM_MAGIC 0x524F4D21u
#define ROM_BASE 0x1FFFF000u

#define ID_CHARGER 0x19
#define ID_BATTERY 0x22
#define ID_DIAG_REQ 0x3C
#define ID_DIAG_RESP 0x3D
#define STATUS_AWAKE 0x01
#define STATUS_MAINS 0x09
#define SILENCE_MS 3000u       // no ID 19 header for this long restarts the sequence
#define BATTERY_FRESH_MS 2000u
#define MV_MIN 6000            // ID 22 voltages outside this range are ignored, so they go stale
#define MV_MAX 20000
#define SYSCLK_HPRE (9u << 4)  // AHB = HSI / 4 = 2 MHz
#define ECHO_TIMEOUT_US 3000u

enum { MODE_AUTO, MODE_MAINS, MODE_NO_MAINS, MODE_SILENT };

// ---- settings
static int32_t s_mode, s_cur, s_ma_count, s_mains_ms, s_cutoff, s_cut_mv, s_cut_s, s_rec_mv, s_rec_s,
    s_silent_ms, s_resp_us, s_stats_s, s_log, s_sta_on;

typedef struct {
  const char *name;
  int32_t *p;
  int32_t def, min, max;
  uint8_t saved;
  const char *help;
} setting_t;

static const setting_t settings[] = {
    {"mode", &s_mode, MODE_AUTO, 0, 3, 0, "0 auto, 1 force mains, 2 force no mains, 3 silent (never saved)"},
    {"cur", &s_cur, -1, -1, 255, 1, "ID 19 current in counts, -1 = from ID 22"},
    {"ma_count", &s_ma_count, 472, 1, 5000, 1, "mA per ID 19 count"},
    {"mains_ms", &s_mains_ms, 7000, 0, 600000, 1, "status 01 time before 09"},
    {"cutoff", &s_cutoff, 1, 0, 1, 1, "battery protection, 0 off, 1 on"},
    {"cut_mv", &s_cut_mv, 11000, MV_MIN, MV_MAX, 1, "cut below this voltage"},
    {"cut_s", &s_cut_s, 10, 0, 3600, 1, "for this long"},
    {"rec_mv", &s_rec_mv, 12800, MV_MIN, MV_MAX, 1, "recover above this voltage"},
    {"rec_s", &s_rec_s, 60, 0, 86400, 1, "for this long"},
    {"silent_ms", &s_silent_ms, 1700, 0, 60000, 1, "status 01 time between a cut and silence"},
    {"resp_us", &s_resp_us, 130, 0, 5000, 1, "reply delay; 130 gives about 380 us on the bus"},
    {"stats_s", &s_stats_s, 0, 0, 3600, 1, "print the counters this often, 0 off"},
    {"log", &s_log, 1, 0, 1, 1, "print events, 0 off, 1 on"},
    {"sta_on", &s_sta_on, 1, 0, 1, 1, "level that lights the STA LED"},
};
#define N_SETTINGS (int)(sizeof settings / sizeof settings[0])

#define SETTINGS_ADDR 0x0800FC00u  // the last 1 KB page; the linker keeps code out of it
#define SETTINGS_MAGIC 0x53455431u
typedef struct {
  uint32_t magic, count;
  int32_t v[N_SETTINGS];
  uint32_t sum;
} stored_t;

static uint32_t stored_sum(const stored_t *x) {
  uint32_t s = x->magic ^ x->count;
  for (int i = 0; i < N_SETTINGS; i++) s = (s << 1 | s >> 31) ^ (uint32_t)x->v[i];
  return s;
}
static void settings_defaults(void) {
  for (int i = 0; i < N_SETTINGS; i++) *settings[i].p = settings[i].def;
}
static int settings_load(void) {
  const stored_t *x = (const stored_t *)SETTINGS_ADDR;
  settings_defaults();
  if (x->magic != SETTINGS_MAGIC || x->count != (uint32_t)N_SETTINGS || x->sum != stored_sum(x)) return 0;
  for (int i = 0; i < N_SETTINGS; i++)
    if (settings[i].saved && x->v[i] >= settings[i].min && x->v[i] <= settings[i].max) *settings[i].p = x->v[i];
  return 1;
}

static void flash_wait(void) {
  while (R(FLASH + FLASH_SR) & 1) R(IWDG + IWDG_KR) = 0xAAAA;
  R(FLASH + FLASH_SR) = (1u << 5) | (1u << 4) | (1u << 2);
}
// The CPU stalls during erase and write, so this costs a few LIN frames. Only "save" calls it.
static int settings_save(void) {
  static stored_t x;
  x.magic = SETTINGS_MAGIC; x.count = N_SETTINGS;
  for (int i = 0; i < N_SETTINGS; i++) x.v[i] = settings[i].saved ? *settings[i].p : settings[i].def;
  x.sum = stored_sum(&x);
  R(FLASH + FLASH_KEYR) = 0x45670123u; R(FLASH + FLASH_KEYR) = 0xCDEF89ABu;
  flash_wait();
  R(FLASH + FLASH_CR) = 1u << 1;
  R(FLASH + FLASH_AR) = SETTINGS_ADDR;
  R(FLASH + FLASH_CR) = (1u << 1) | (1u << 6);
  flash_wait();
  R(FLASH + FLASH_CR) = 1u;
  const uint16_t *src = (const uint16_t *)&x;
  for (unsigned i = 0; i < sizeof x / 2; i++) {
    *(volatile uint16_t *)(SETTINGS_ADDR + 2 * i) = src[i];
    flash_wait();
  }
  R(FLASH + FLASH_CR) = 1u << 7; // lock
  const stored_t *y = (const stored_t *)SETTINGS_ADDR;
  return y->magic == x.magic && y->sum == x.sum;
}

// ---- time
static uint32_t now_ms, now_us_acc;
static uint16_t tim_last;

static void tick(void) {
  uint16_t c = (uint16_t)R(TIM2 + TIM_CNT);
  now_us_acc += (uint16_t)(c - tim_last);
  tim_last = c;
  while (now_us_acc >= 1000) { now_us_acc -= 1000; now_ms++; }
}
static uint16_t us(void) { return (uint16_t)R(TIM2 + TIM_CNT); }

// ---- console output: a ring buffer that drops when full. At 2 MHz a long print would take longer
// than a LIN byte, so writing each character also services LIN.
static void lin_poll(void);
static char txq[1024];
static uint16_t txh, txt;
static int in_lin;
static char evq[96]; // what LIN handling prints waits here, so it can't land inside another line
static uint8_t evh, evt;
static void qputc(char c) {
  if (in_lin) {
    uint8_t e = (uint8_t)((evh + 1) % sizeof evq);
    if (e != evt) { evq[evh] = c; evh = e; }
    return;
  }
  uint16_t n = (txh + 1) % sizeof txq;
  if (n != txt) { txq[txh] = c; txh = n; }
  if (!in_lin) lin_poll();
}
static void dputc(char c) { if (c == '\n') qputc('\r'); qputc(c); }
static void dputs(const char *s) { while (*s) dputc(*s++); }
static void ddec(uint32_t v) { char b[11]; int i = 0; do { b[i++] = '0' + v % 10; v /= 10; } while (v); while (i) dputc(b[--i]); }
static void dint(int32_t v) { if (v < 0) { dputc('-'); ddec((uint32_t)-v); } else ddec((uint32_t)v); }
static void dhex(uint8_t v) { static const char h[] = "0123456789ABCDEF"; dputc(h[v >> 4]); dputc(h[v & 15]); }
static void dbg_tx(void) {
  if (txt != txh && (R(USART1 + USART_SR) & SR_TXE)) { R(USART1 + USART_DR) = (uint8_t)txq[txt]; txt = (txt + 1) % sizeof txq; }
}
static void dbg_flush(void) {
  while (txt != txh) { dbg_tx(); R(IWDG + IWDG_KR) = 0xAAAA; }
  while (!(R(USART1 + USART_SR) & SR_TC));
}
static void event(const char *s) { if (s_log) dputs(s); }
static void evq_poll(void) {
  while (evt != evh) { char c = evq[evt]; evt = (uint8_t)((evt + 1) % sizeof evq); qputc(c); }
}

// ---- LIN helpers
static uint8_t pid_of(uint8_t id) {
  uint8_t b0 = id & 1, b1 = (id >> 1) & 1, b2 = (id >> 2) & 1, b3 = (id >> 3) & 1, b4 = (id >> 4) & 1, b5 = (id >> 5) & 1;
  return (uint8_t)(id | ((b0 ^ b1 ^ b2 ^ b4) << 6) | (((b1 ^ b3 ^ b4 ^ b5) ^ 1) << 7));
}
static uint8_t checksum(const uint8_t *d, int n, uint16_t seed) {
  uint16_t s = seed;
  for (int i = 0; i < n; i++) { s += d[i]; if (s > 255) s -= 255; }
  return (uint8_t)~s;
}
static uint8_t frame_ck(uint8_t id, const uint8_t *d, int n) { // classic on 3C/3D, enhanced otherwise
  return checksum(d, n, (id == ID_DIAG_REQ || id == ID_DIAG_RESP) ? 0 : pid_of(id));
}

// ---- LIN slave state machine
enum { ST_IDLE, ST_SYNC, ST_PID, ST_DATA, ST_RESP_WAIT, ST_TX };
static uint8_t st = ST_IDLE, cur_id;
static uint8_t rx[9]; static int rx_n, rx_len;
static uint8_t tx[9]; static int tx_i, tx_len;
static uint16_t t_mark;
static int diag_armed;
static uint32_t t_first19, t_last19, t_batt;
static int have19, have_batt;
static int32_t batt_ma;
static uint8_t last_status, last_current;
static int cut, low_latch, below, above;
static uint32_t t_cut, t_below, t_above, n_silent;
static int32_t batt_mv;
static uint32_t n_hdr, n_19, n_3d, n_ck, n_echo, n_parity, n_batt, n_lbd, n_rx, n_fe;
static uint8_t last_rx;

// Bus monitor: each frame's bytes after the sync, handed to the main loop at the next break
static int mon_on;
static uint8_t mon[12], mon_out[12]; static int mon_n, mon_out_n, mon_seen_sync;

static void respond(const uint8_t *d, int n) {
  for (int i = 0; i < n; i++) tx[i] = d[i];
  tx[n] = frame_ck(cur_id, d, n);
  tx_len = n + 1; tx_i = 0;
  t_mark = us();
  st = ST_RESP_WAIT;
}

static int battery_fresh(void) { return have_batt && now_ms - t_batt < BATTERY_FRESH_MS; }

static void battery_voltage(int32_t mv) {
  batt_mv = mv;
  if (mv < s_cut_mv) {
    if (!below) { below = 1; t_below = now_ms; }
    if (now_ms - t_below >= (uint32_t)s_cut_s * 1000) low_latch = 1;
  } else {
    below = 0;
  }
  if (mv >= s_rec_mv) {
    if (!above) { above = 1; t_above = now_ms; }
    if (now_ms - t_above >= (uint32_t)s_rec_s * 1000) low_latch = 0;
  } else {
    above = 0;
  }
}

static void battery_poll(void) {
  int c = s_cutoff && (low_latch || (have_batt && !battery_fresh()));
  if (c == cut) return;
  cut = c;
  if (c) { t_cut = now_ms; event(low_latch ? "cut: low voltage\n" : "cut: no ID 22\n"); }
  else { have19 = 0; event("cut over\n"); } // the next poll restarts at status 01
}

static uint8_t current_counts(void) {
  if (s_cur >= 0) return (uint8_t)s_cur;
  if (!battery_fresh() || batt_ma <= 0) return 0;
  int32_t c = batt_ma / s_ma_count;
  return c > 255 ? 255 : (uint8_t)c;
}

static void answer_charger(uint8_t current, uint8_t status) {
  if (status != last_status) { event("status "); if (s_log) dhex(status); event("\n"); last_status = status; }
  last_current = current;
  uint8_t d[2] = {current, status};
  respond(d, 2);
}

static void on_charger_header(void) {
  n_19++;
  if (s_mode == MODE_SILENT) { n_silent++; return; }
  if (s_mode == MODE_NO_MAINS) { answer_charger(0, STATUS_AWAKE); return; }
  if (s_mode == MODE_MAINS) {
    have19 = 1; t_last19 = now_ms;
    answer_charger(current_counts(), STATUS_MAINS);
    return;
  }
  if (cut) {
    if (now_ms - t_cut >= (uint32_t)s_silent_ms) { n_silent++; return; }
    answer_charger(0, STATUS_AWAKE);
    return;
  }
  if (!have19 || now_ms - t_last19 > SILENCE_MS) { t_first19 = now_ms; have19 = 1; event("poll start\n"); }
  t_last19 = now_ms;
  uint8_t status = now_ms - t_first19 >= (uint32_t)s_mains_ms && battery_fresh() ? STATUS_MAINS : STATUS_AWAKE;
  answer_charger(status == STATUS_MAINS ? current_counts() : 0, status);
}

static void on_frame(void) {
  if (frame_ck(cur_id, rx, rx_len - 1) != rx[rx_len - 1]) { n_ck++; return; }
  if (cur_id == ID_BATTERY) {
    int32_t mv = rx[3] | (rx[4] << 8);
    n_batt++;
    if (mv >= MV_MIN && mv <= MV_MAX) {
      batt_ma = (int32_t)(rx[0] | (rx[1] << 8)) - 0x8000;
      t_batt = now_ms; have_batt = 1;
      battery_voltage(mv);
    }
  } else if (cur_id == ID_DIAG_REQ) {
    if (rx[0] == 0x18 && rx[1] == 0x06 && rx[2] == 0xB2 && rx[3] == 0x00 && rx[4] == 0x34 && rx[5] == 0x12 && rx[7] == 0x00) {
      diag_armed = rx[6] == 0x0E;
      event("3C fn ");
      if (s_log) dhex(rx[6]);
      event("\n");
    }
  }
}

static void on_pid(uint8_t pid) {
  uint8_t id = pid & 0x3F;
  if (pid_of(id) != pid) { n_parity++; st = ST_IDLE; return; }
  n_hdr++; cur_id = id; st = ST_IDLE;
  if (id == ID_CHARGER) {
    on_charger_header();
  } else if (id == ID_DIAG_RESP) {
    if (diag_armed && s_mode != MODE_SILENT && !(s_mode == MODE_AUTO && cut)) {
      static const uint8_t d[8] = {0x18, 0x06, 0xF2, 0x34, 0x12, 0x0E, 0x00, 0x01};
      diag_armed = 0; n_3d++;
      respond(d, 8);
    }
  } else if (id == ID_BATTERY) {
    rx_n = 0; rx_len = 8; st = ST_DATA;
  } else if (id == ID_DIAG_REQ) {
    rx_n = 0; rx_len = 9; st = ST_DATA;
  }
}

static void lin_poll(void) {
  if (in_lin) return;
  in_lin = 1;
  uint32_t sr = R(USART2 + USART_SR);
  if (sr & SR_LBD) {
    R(USART2 + USART_SR) = ~SR_LBD; st = ST_SYNC; n_lbd++;
    if (mon_on && mon_n) { for (int i = 0; i < mon_n; i++) mon_out[i] = mon[i]; mon_out_n = mon_n; }
    mon_n = 0; mon_seen_sync = 0;
  }
  if (sr & (SR_RXNE | SR_ORE | SR_FE | SR_NE)) {
    uint8_t c = (uint8_t)R(USART2 + USART_DR);
    n_rx++; last_rx = c; if (sr & SR_FE) n_fe++;
    if (mon_on) {
      if (mon_seen_sync) { if (!(sr & SR_FE) && mon_n < (int)sizeof mon) mon[mon_n++] = c; }
      else if (c == 0x55) mon_seen_sync = 1;
    }
    switch (st) {
    case ST_SYNC:
      if (c == 0x55) st = ST_PID;
      else if (c != 0) st = ST_IDLE;
      break;
    case ST_PID:
      on_pid(c);
      break;
    case ST_DATA:
      rx[rx_n++] = c;
      if (rx_n == rx_len) { st = ST_IDLE; on_frame(); }
      break;
    case ST_TX: // the transceiver echoes each byte: stop on a mismatch, as someone else is talking
      if (c != tx[tx_i]) { n_echo++; st = ST_IDLE; break; }
      if (++tx_i < tx_len) { R(USART2 + USART_DR) = tx[tx_i]; t_mark = us(); }
      else st = ST_IDLE;
      break;
    default:
      break;
    }
  }
  if (st == ST_RESP_WAIT && (uint16_t)(us() - t_mark) >= (uint32_t)s_resp_us) {
    R(USART2 + USART_DR) = tx[0]; t_mark = us(); st = ST_TX;
  } else if (st == ST_TX && (uint16_t)(us() - t_mark) >= ECHO_TIMEOUT_US) {
    n_echo++; st = ST_IDLE;
  }
  in_lin = 0;
}

static void mon_poll(void) {
  if (!mon_out_n) return;
  int n = mon_out_n;
  mon_out_n = 0;
  dhex(mon_out[0] & 0x3F); dputc(':');
  for (int i = 1; i < n; i++) { dputc(' '); dhex(mon_out[i]); }
  dputc('\n');
}

// ---- bench self-test: a queued master frame, sent a byte at a time
static uint8_t mq[12]; static int mq_i, mq_n, mq_break;
static void master_poll(void) {
  if (mq_break) { R(USART2 + USART_CR1) |= CR1_SBK; mq_break = 0; return; }
  if (mq_i < mq_n && !(R(USART2 + USART_CR1) & CR1_SBK) && (R(USART2 + USART_SR) & SR_TXE) && st != ST_RESP_WAIT && st != ST_TX) {
    R(USART2 + USART_DR) = mq[mq_i++];
  }
}

// ---- console
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int parse_int(const char *s, int32_t *out) {
  int neg = *s == '-';
  if (neg) s++;
  if (!*s) return 0;
  int32_t v = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9' || v > 100000000) return 0;
    v = v * 10 + (*s - '0');
  }
  *out = neg ? -v : v;
  return 1;
}
static void print_stats(void) {
  dputs("t="); ddec(now_ms); dputs(" hdr="); ddec(n_hdr); dputs(" id19="); ddec(n_19); dputs(" 3d="); ddec(n_3d);
  dputs(" batt="); ddec(n_batt); dputs(" st="); dhex(last_status); dputs(" cur="); ddec(last_current);
  dputs(" ck_err="); ddec(n_ck); dputs(" echo_err="); ddec(n_echo); dputs(" par_err="); ddec(n_parity);
  dputs(" mv="); dint(batt_mv); dputs(" ma="); dint(batt_ma); dputs(" cut="); dint(cut); dputs(" silent="); ddec(n_silent);
  dputs(" lbd="); ddec(n_lbd); dputs(" rx="); ddec(n_rx); dputs(" fe="); ddec(n_fe); dputc('\n');
}
static void print_settings(void) {
  for (int i = 0; i < N_SETTINGS; i++) {
    dputs(settings[i].name); dputs(" = "); dint(*settings[i].p);
    dputs("  ("); dputs(settings[i].help); dputs(")\n");
  }
}
static void print_help(void) {
  dputs("st                  counters\n"
        "show                settings\n"
        "set <name> <value>  change a setting now\n"
        "save                keep settings over a power cycle\n"
        "defaults            reset settings, until saved\n"
        "mon 0|1             print every LIN frame: id: bytes\n"
        "pu 0|1              1k master pull-up: bench only\n"
        "f <id> [hex bytes]  send a LIN frame as if master\n"
        "boot                reset\n"
        "rom                 enter the ROM bootloader, to update\n"
        "hang                test the watchdog\n");
}
static void command(char *s) {
  char *a[12]; int n = 0;
  while (*s && n < 12) {
    while (*s == ' ') *s++ = 0;
    if (!*s) break;
    a[n++] = s;
    while (*s && *s != ' ') s++;
  }
  while (*s == ' ') *s++ = 0;
  if (!n) return;
  int32_t v;
  if (streq(a[0], "help")) print_help();
  else if (streq(a[0], "st")) print_stats();
  else if (streq(a[0], "show")) print_settings();
  else if (streq(a[0], "set") && n == 3) {
    for (int i = 0; i < N_SETTINGS; i++) if (streq(a[1], settings[i].name)) {
      if (parse_int(a[2], &v) && v >= settings[i].min && v <= settings[i].max) {
        *settings[i].p = v; dputs(settings[i].name); dputs(" = "); dint(v); dputc('\n');
      } else {
        dputs("range "); dint(settings[i].min); dputs(" to "); dint(settings[i].max); dputc('\n');
      }
      return;
    }
    dputs("no such setting, try show\n");
  }
  else if (streq(a[0], "save")) {
    dbg_flush();
    dputs(settings_save() ? "saved\n" : "save FAILED\n");
  }
  else if (streq(a[0], "defaults")) { settings_defaults(); dputs("defaults, until saved\n"); }
  else if (streq(a[0], "mon") && n == 2 && parse_int(a[1], &v)) { mon_on = v != 0; dputs("mon\n"); }
  else if (streq(a[0], "pu") && n == 2 && parse_int(a[1], &v)) {
    R(GPIOB + GPIO_BSRR) = v ? (1u << 9) : (1u << (9 + 16));
    dputs(v ? "pu 1\n" : "pu 0\n");
  }
  else if (streq(a[0], "boot")) { dputs("reset\n"); dbg_flush(); R(SCB_AIRCR) = 0x05FA0004u; for (;;); }
  else if (streq(a[0], "rom")) { dputs("rom\n"); dbg_flush(); ROM_FLAG = ROM_MAGIC; R(SCB_AIRCR) = 0x05FA0004u; for (;;); }
  else if (streq(a[0], "hang")) { dputs("hanging\n"); dbg_flush(); for (;;); }
  else if (streq(a[0], "f") && n >= 2) {
    uint8_t b[10]; int nb = 0;
    for (int i = 1; i < n && nb < 10; i++) {
      int h = hexval(a[i][0]), l = hexval(a[i][1]);
      if (h < 0 || l < 0 || a[i][2]) { dputs("bad hex\n"); return; }
      b[nb++] = (uint8_t)(h * 16 + l);
    }
    if (mq_i < mq_n || mq_break) { dputs("busy\n"); return; }
    mq_n = 0; mq_i = 0;
    mq[mq_n++] = 0x55; mq[mq_n++] = pid_of(b[0] & 0x3F);
    for (int k = 1; k < nb; k++) mq[mq_n++] = b[k];
    if (nb > 1) mq[mq_n++] = frame_ck(b[0] & 0x3F, b + 1, nb - 1);
    mq_break = 1;
  }
  else dputs("unknown, try help\n");
}
// At 2 MHz, a console byte arrives about every 350 cycles, too fast to poll reliably, so DMA
// stores received bytes in this ring.
static volatile uint8_t rxq[128];
static uint16_t rxq_tail;
static void console_poll(void) {
  static char line[48]; static int n;
  uint16_t head = (uint16_t)(sizeof rxq - R(DMA1_CH5 + DMA_CNDTR)) % sizeof rxq;
  if (head == rxq_tail) return;
  char c = (char)rxq[rxq_tail];
  rxq_tail = (rxq_tail + 1) % sizeof rxq;
  if (c == '\r' || c == '\n') {
    if (n) {
      dputs("\n");
      line[n] = 0;
      n = 0;
      command(line);
      dputs("> ");
    }
  } else if (c == 8 || c == 127) {
    if (n) { n--; dputs("\b \b"); }
  } else if (c >= 32 && c < 127 && n < (int)sizeof line - 1) {
    line[n++] = c;
    dputc(c);
  }
}

// ---- STA, short flashes to save current: every 2 s with no ID 19 polling, every 500 ms while
// answering it
#define FLASH_MS 20u
static void led_poll(void) {
  uint32_t period = (have19 && !cut && now_ms - t_last19 < 1000) ? 500 : 2000;
  int on = now_ms % period < FLASH_MS;
  R(GPIOA + GPIO_BSRR) = on == s_sta_on ? (1u << 15) : (1u << 31);
}

// ---- sleep until a USART event or the next millisecond, unless something needs polling
#define IRQ0_TIM2 (1u << 28)
#define IRQ1_LIN (1u << 6)
static void idle(void) {
  if ((uint16_t)(sizeof rxq - R(DMA1_CH5 + DMA_CNDTR)) % sizeof rxq != rxq_tail) return;
  if (evt != evh || st == ST_RESP_WAIT || st == ST_TX || txt != txh || mq_break || mq_i < mq_n || mon_out_n) return;
  R(TIM2 + TIM_CCR1) = (uint16_t)(us() + 1000);
  R(TIM2 + TIM_SR) = 0;
  R(NVIC_ICPR0) = IRQ0_TIM2;
  R(NVIC_ICPR1) = IRQ1_LIN;
  __asm volatile("wfi");
}

static void cfg(uint32_t port, int pin, uint32_t m) {
  uint32_t reg = port + (pin >= 8 ? GPIO_CRH : GPIO_CRL), sh = (pin & 7) * 4;
  R(reg) = (R(reg) & ~(0xFu << sh)) | (m << sh);
}
#define IN_PULL 0x8u
#define OUT_PP 0x2u
#define AF_PP 0xAu

extern uint32_t _sdata, _edata, _sidata, _sbss, _ebss;

void reset_handler(void) {
  __asm volatile("cpsid i");
  R(SCB_VTOR) = FLASH_START; // also started by the bootloader's Go command, with BOOT0 high
  R(SYST_CSR) = 0;         // SysTick off, in case the bootloader left it on

  // The first job: until PA2 is driven, the TJA1021's TXD pull-down makes LIN dominant.
  R(RCC + RCC_APB2ENR) |= (1u << 0) | (1u << 2) | (1u << 3);
  R(GPIOA + GPIO_BSRR) = 1u << 2;
  cfg(GPIOA, 2, OUT_PP);

  // Before the watchdog starts: it can't be stopped, and the ROM doesn't refresh it. PA2 stays
  // driven high, so LIN stays recessive during an update.
  if (ROM_FLAG == ROM_MAGIC) {
    ROM_FLAG = 0;
    R(SCB_VTOR) = ROM_BASE;
    __asm volatile("msr msp, %0\n cpsie i\n bx %1" : : "r"(R(ROM_BASE)), "r"(R(ROM_BASE + 4)));
    for (;;);
  }
  ROM_FLAG = 0;

  for (uint32_t *s = &_sidata, *d = &_sdata; d < &_edata;) *d++ = *s++;
  for (uint32_t *d = &_sbss; d < &_ebss;) *d++ = 0;

  R(RCC + RCC_CR) |= 1;                      // HSI on, the only clock assumed
  while (!(R(RCC + RCC_CR) & 2));
  R(RCC + RCC_CFGR) &= ~3u;                // SYSCLK = HSI, 8 MHz
  while (R(RCC + RCC_CFGR) & 0xC);
  R(FLASH + FLASH_ACR) = 1u << 4;               // flash prefetch on, required with an AHB prescaler
  R(RCC + RCC_CFGR) = SYSCLK_HPRE;         // 2 MHz, APB prescalers 1
  R(RCC + RCC_CR) &= ~(1u << 24);            // PLL off
  R(RCC + RCC_APB2ENR) |= 1u << 14;        // USART1
  R(RCC + RCC_APB1ENR) |= (1u << 17) | 1u; // USART2, TIM2
  R(AFIO_MAPR) = (R(AFIO_MAPR) & ~(7u << 24)) | (2u << 24); // JTAG off, SWD kept: frees PA15

  int loaded = settings_load();

  // PB8, PB9 low: the RESET pad and the master pull-up stay off
  R(GPIOB + GPIO_BSRR) = (1u << (8 + 16)) | (1u << (9 + 16));
  cfg(GPIOB, 8, OUT_PP);
  cfg(GPIOB, 9, OUT_PP);
  // STA
  cfg(GPIOA, 15, OUT_PP);

  // 1 us timer
  R(TIM2 + TIM_CR1) = 0;
  R(TIM2 + TIM_PSC) = 1;
  R(TIM2 + TIM_ARR) = 0xFFFF;
  R(TIM2 + TIM_EGR) = 1;
  R(TIM2 + TIM_DIER) = 1u << 1; // CC1 wakes the loop
  R(TIM2 + TIM_CR1) = 1;

  // console
  R(USART1 + USART_CR1) = 0;
  R(USART1 + USART_BRR) = 35; // 2 MHz / 57600, -0.8%
  R(USART1 + USART_CR2) = 0;
  R(RCC + RCC_AHBENR) |= 1u;              // DMA1
  R(DMA1_CH5 + DMA_CCR) = 0;
  R(DMA1_CH5 + DMA_CNDTR) = sizeof rxq;
  R(DMA1_CH5 + DMA_CPAR) = USART1 + USART_DR;
  R(DMA1_CH5 + DMA_CMAR) = (uint32_t)rxq;
  R(DMA1_CH5 + DMA_CCR) = (1u << 7) | (1u << 5) | 1u; // memory increment, circular, on
  R(USART1 + USART_CR3) = 1u << 6;        // RX through DMA
  R(USART1 + USART_CR1) = (1u << 13) | (1u << 3) | (1u << 2); // 8N1
  R(GPIOA + GPIO_BSRR) = 1u << 10;
  cfg(GPIOA, 10, IN_PULL);
  cfg(GPIOA, 9, AF_PP);

  // LIN: 19200 8N1 with 11-bit break detection. RXD is open drain.
  R(USART2 + USART_CR1) = 0;
  R(USART2 + USART_BRR) = 104; // 2 MHz / 19200, +0.16%
  R(USART2 + USART_CR3) = 0;
  R(USART2 + USART_CR2) = (1u << 14) | (1u << 6) | (1u << 5); // LIN mode, 11-bit break, break wakes
  R(USART2 + USART_CR1) = (1u << 13) | (1u << 5) | (1u << 3) | (1u << 2); // RXNE wakes
  R(GPIOA + GPIO_BSRR) = 1u << 3;
  cfg(GPIOA, 3, IN_PULL);
  cfg(GPIOA, 2, AF_PP);

  // Watchdog: 40 kHz LSI / 16, 500 counts, nominally 200 ms
  R(IWDG + IWDG_KR) = 0x5555;
  R(IWDG + IWDG_PR) = 2;
  R(IWDG + IWDG_RLR) = 500;
  R(IWDG + IWDG_KR) = 0xCCCC;

  R(NVIC_ISER0) = IRQ0_TIM2;
  R(NVIC_ISER1) = IRQ1_LIN;
  tim_last = us();
  dputs("\ncharger_emu " VERSION ", settings: ");
  dputs(loaded ? "saved\n" : "defaults\n");
  uint32_t t_report = 0;
  for (;;) {
    R(IWDG + IWDG_KR) = 0xAAAA;
    tick();
    lin_poll();
    master_poll();
    console_poll();
    dbg_tx();
    battery_poll();
    mon_poll();
    evq_poll();
    led_poll();
    if (s_stats_s && now_ms - t_report >= (uint32_t)s_stats_s * 1000) { t_report = now_ms; print_stats(); }
    idle();
  }
}

static void fault(void) { for (;;); } // the watchdog resets the chip

__attribute__((section(".vectors"), used)) static void *const vectors[16] = {
    (void *)0x20004FF8u, (void *)reset_handler, (void *)fault, (void *)fault,
    (void *)fault, (void *)fault, (void *)fault, 0, 0, 0, 0,
    (void *)fault, (void *)fault, 0, (void *)fault, (void *)fault,
};
