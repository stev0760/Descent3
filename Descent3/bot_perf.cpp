/*
 * Descent 3 - Matcen multiplayer bots
 *
 * bot_perf — slow-frame attribution for the bot layer. See bot_perf.h.
 */

#include "bot_perf.h"
#include "game.h" // Frametime, Gametime
#include "log.h"

#include <cstdio>

// A 60 fps server frame is 16.7 ms and a PPS-40 client expects a position every 25 ms: a bot layer
// that eats more than one packet interval is already a visible hitch.
#define BOT_PERF_SLOW_BOTS_MS 25.0
// The whole server frame (previous frame, sleep included) — catches stalls outside the bot layer too.
#define BOT_PERF_SLOW_FRAME_MS 50.0
#define BOT_PERF_SUMMARY_INTERVAL 60.0f

static const char *const Bot_perf_name[BPERF_COUNT] = {
    "frame", "state", "routed", "via",   "explore",  "troute",  "reach",  "objpoll", "build", "union",
    "theta", "qvia",  "skel",   "route", "crossing", "roomaim", "ograph", "findvia", "sweep",
};

static double Frame_ms[BPERF_COUNT];
static int Frame_calls[BPERF_COUNT];

static double Sum_ms[BPERF_COUNT];
static long Sum_calls[BPERF_COUNT];
static long Sum_frames = 0;
static long Sum_over[4];       // bot layer over 10 / 25 / 50 / 100 ms
static long Sum_frame_over[3]; // server frame over 50 / 100 / 250 ms
static double Sum_worst_bots = 0.0, Sum_worst_frame = 0.0;
static float Sum_last_time = -1.0f;

void BotPerfAdd(int id, double ms) {
  if (id < 0 || id >= BPERF_COUNT)
    return;
  Frame_ms[id] += ms;
  Frame_calls[id]++;
}

// "name ms(calls)" for every scope that cost at least min_ms, into buf.
static void PerfFormat(char *buf, int buflen, const double *ms, const long *calls, double min_ms) {
  int n = 0;
  buf[0] = '\0';
  for (int i = 1; i < BPERF_COUNT && n < buflen - 40; i++) {
    if (ms[i] < min_ms)
      continue;
    n += snprintf(buf + n, buflen - n, " %s %.0f(%ld)", Bot_perf_name[i], ms[i], calls[i]);
  }
}

void BotPerfFrameEnd(bool any_bot) {
  if (!any_bot) { // a bot-free server says nothing; drop the frame and restart the summary window
    for (int i = 0; i < BPERF_COUNT; i++) {
      Frame_ms[i] = 0.0;
      Frame_calls[i] = 0;
    }
    Sum_last_time = -1.0f;
    return;
  }
  const double bots_ms = Frame_ms[BPERF_FRAME];
  const double frame_ms = (double)Frametime * 1000.0; // the frame BEFORE this one, sleep included

  if (bots_ms > BOT_PERF_SLOW_BOTS_MS || frame_ms > BOT_PERF_SLOW_FRAME_MS) {
    char buf[512];
    long calls[BPERF_COUNT];
    for (int i = 0; i < BPERF_COUNT; i++)
      calls[i] = Frame_calls[i];
    PerfFormat(buf, sizeof(buf), Frame_ms, calls, 1.0);
    LOG_INFO.printf("[Perf] slow frame: bots %.0f ms, prev server frame %.0f ms |%s", bots_ms, frame_ms, buf);
  }

  Sum_frames++;
  if (bots_ms > 10.0)
    Sum_over[0]++;
  if (bots_ms > 25.0)
    Sum_over[1]++;
  if (bots_ms > 50.0)
    Sum_over[2]++;
  if (bots_ms > 100.0)
    Sum_over[3]++;
  if (frame_ms > 50.0)
    Sum_frame_over[0]++;
  if (frame_ms > 100.0)
    Sum_frame_over[1]++;
  if (frame_ms > 250.0)
    Sum_frame_over[2]++;
  if (bots_ms > Sum_worst_bots)
    Sum_worst_bots = bots_ms;
  if (frame_ms > Sum_worst_frame)
    Sum_worst_frame = frame_ms;
  for (int i = 0; i < BPERF_COUNT; i++) {
    Sum_ms[i] += Frame_ms[i];
    Sum_calls[i] += Frame_calls[i];
    Frame_ms[i] = 0.0;
    Frame_calls[i] = 0;
  }

  // Gametime resets on a level change; the self-healing latch the other periodic blocks use.
  if (Sum_last_time < 0.0f || Gametime < Sum_last_time)
    Sum_last_time = Gametime;
  if (Gametime - Sum_last_time >= BOT_PERF_SUMMARY_INTERVAL && Sum_frames > 0) {
    char buf[512];
    PerfFormat(buf, sizeof(buf), Sum_ms, Sum_calls, 5.0);
    LOG_INFO.printf("[Perf] summary %.0fs: %ld frames (%.1f fps), bots avg %.2f ms worst %.0f ms, bot-layer frames "
                    ">10ms %ld >25ms %ld >50ms %ld >100ms %ld | server frames >50ms %ld >100ms %ld >250ms %ld worst "
                    "%.0f ms | totals ms(calls):%s",
                    Gametime - Sum_last_time, Sum_frames, Sum_frames / (Gametime - Sum_last_time),
                    Sum_ms[BPERF_FRAME] / Sum_frames, Sum_worst_bots, Sum_over[0], Sum_over[1], Sum_over[2],
                    Sum_over[3], Sum_frame_over[0], Sum_frame_over[1], Sum_frame_over[2], Sum_worst_frame, buf);
    for (int i = 0; i < BPERF_COUNT; i++) {
      Sum_ms[i] = 0.0;
      Sum_calls[i] = 0;
    }
    Sum_frames = 0;
    Sum_over[0] = Sum_over[1] = Sum_over[2] = Sum_over[3] = 0;
    Sum_frame_over[0] = Sum_frame_over[1] = Sum_frame_over[2] = 0;
    Sum_worst_bots = Sum_worst_frame = 0.0;
    Sum_last_time = Gametime;
  }
}
