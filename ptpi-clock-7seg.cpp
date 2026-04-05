// PTP LED Clock for Raspberry Pi
// - Uses the shared PTP packet-processing library
// - Displays:
//      HH:MM:SS
//      :nnnnnnnnn

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <time.h>

#include "graphics.h"
#include "led-matrix.h"
#include "ptp_clock_ptp.h"

static const int PANEL_ROWS = 32;
static const int PANEL_COLS = 64;
static const int CHAIN_LENGTH = 2;
static const int BRIGHTNESS = 100;

static uint8_t CLOCK_R = 165;
static uint8_t CLOCK_G = 40;
static uint8_t CLOCK_B = 0;

static const uint8_t STATUS_R = 255;
static const uint8_t STATUS_G = 0;
static const uint8_t STATUS_B = 0;

static int TIMEZONE_OFFSET_SECONDS = 0;

static const double FRAME_INTERVAL_SEC = 0.01;
static const char *STATUS_FONT_PATH = "/opt/ptpi-clock/fonts/7x14B.bdf";
static const int STATUS_CHAR_WIDTH = 7;

static std::string g_iface_name;
static std::string g_iface_ipv4;
static bool g_enable_logging = false;
static volatile bool g_running = true;

static inline void logmsg(const std::string &msg)
{
  if (g_enable_logging)
  {
    std::cout << msg << std::endl;
  }
}

static bool parse_u8_arg(const char *s, uint8_t &out)
{
  char *end = nullptr;
  long v = std::strtol(s, &end, 10);
  if (end == s || *end != '\0' || v < 0 || v > 255)
  {
    return false;
  }
  out = static_cast<uint8_t>(v);
  return true;
}

static bool parse_int_arg(const char *s, int &out)
{
  char *end = nullptr;
  long v = std::strtol(s, &end, 10);
  if (end == s || *end != '\0')
  {
    return false;
  }
  out = static_cast<int>(v);
  return true;
}

static void sleep_until_next_frame(uint64_t &last_frame_ns)
{
  const uint64_t frame_interval_ns = static_cast<uint64_t>(FRAME_INTERVAL_SEC * 1e9);
  const uint64_t now_ns = ptpi::GetMonotonicNs();
  if (now_ns - last_frame_ns < frame_interval_ns)
  {
    const uint64_t sleep_ns = frame_interval_ns - (now_ns - last_frame_ns);
    struct timespec ts_sleep{};
    ts_sleep.tv_sec = sleep_ns / 1000000000ULL;
    ts_sleep.tv_nsec = sleep_ns % 1000000000ULL;
    nanosleep(&ts_sleep, nullptr);
  }
  last_frame_ns = ptpi::GetMonotonicNs();
}

static void fill_rect(rgb_matrix::Canvas *c, int x, int y, int w, int h, const rgb_matrix::Color &color)
{
  for (int yy = y; yy < y + h; ++yy)
  {
    for (int xx = x; xx < x + w; ++xx)
    {
      c->SetPixel(xx, yy, color.r, color.g, color.b);
    }
  }
}

static const uint8_t DIGIT_MASKS[10] = {
    0b1111110,
    0b0110000,
    0b1101101,
    0b1111001,
    0b0110011,
    0b1011011,
    0b1011111,
    0b1110000,
    0b1111111,
    0b1111011};

static void draw_7seg_digit(rgb_matrix::Canvas *canvas,
                            int x,
                            int y,
                            int digit,
                            int seg_len,
                            int seg_thick,
                            const rgb_matrix::Color &color)
{
  if (digit < 0 || digit > 9)
  {
    return;
  }

  const uint8_t mask = DIGIT_MASKS[digit];
  const int w = seg_len;
  const int t = seg_thick;
  const int overlap = (t > 1) ? 1 : 0;

  if (mask & 0b1000000)
    fill_rect(canvas, x + t - overlap, y, w + 2 * overlap, t, color);
  if (mask & 0b0000001)
    fill_rect(canvas, x + t - overlap, y + t + w, w + 2 * overlap, t, color);
  if (mask & 0b0001000)
    fill_rect(canvas, x + t - overlap, y + 2 * t + 2 * w, w + 2 * overlap, t, color);

  if (mask & 0b0000010)
    fill_rect(canvas, x, y + t - overlap, t, w + 2 * overlap, color);
  if (mask & 0b0100000)
    fill_rect(canvas, x + t + w, y + t - overlap, t, w + 2 * overlap, color);
  if (mask & 0b0000100)
    fill_rect(canvas, x, y + 2 * t + w - overlap, t, w + 2 * overlap, color);
  if (mask & 0b0010000)
    fill_rect(canvas, x + t + w, y + 2 * t + w - overlap, t, w + 2 * overlap, color);

  if (digit == 1 || digit == 4 || digit == 7)
  {
    fill_rect(canvas, x + t + w - 1, y, t + 1, t, color);
    fill_rect(canvas, x + t + w - 1, y + 2 * t + 2 * w, t + 1, t, color);
  }
}

static void draw_7seg_colon(rgb_matrix::Canvas *canvas,
                            int x,
                            int y,
                            int dot_size,
                            int gap,
                            bool on,
                            const rgb_matrix::Color &color)
{
  if (!on)
  {
    return;
  }
  fill_rect(canvas, x, y, dot_size, dot_size, color);
  fill_rect(canvas, x, y + dot_size + gap, dot_size, dot_size, color);
}

static int calc_digit_width(int seg_len, int seg_thick)
{
  return seg_len + 2 * seg_thick;
}

static void draw_7seg_time(rgb_matrix::Canvas *canvas,
                           int x,
                           int y,
                           int hh,
                           int mm,
                           int ss,
                           bool colon_on,
                           const rgb_matrix::Color &color)
{
  const int seg_len = 7;
  const int seg_thick = 3;
  const int digit_w = calc_digit_width(seg_len, seg_thick);
  const int digit_h = 2 * seg_len + 3 * seg_thick;
  const int digit_gap = 2;
  const int colon_w = 2;
  const int colon_gap_lr = 2;
  const int colon_dot = 2;
  const int colon_gap_ud = 5;
  const int colon_height = colon_dot * 2 + colon_gap_ud;
  const int colon_y = y + (digit_h - colon_height) / 2;

  int d[6] = {hh / 10, hh % 10, mm / 10, mm % 10, ss / 10, ss % 10};
  int cx = x;

  draw_7seg_digit(canvas, cx, y, d[0], seg_len, seg_thick, color);
  cx += digit_w + digit_gap;
  draw_7seg_digit(canvas, cx, y, d[1], seg_len, seg_thick, color);
  cx += digit_w + colon_gap_lr;
  draw_7seg_colon(canvas, cx, colon_y, colon_dot, colon_gap_ud, colon_on, color);
  cx += colon_w + colon_gap_lr;
  draw_7seg_digit(canvas, cx, y, d[2], seg_len, seg_thick, color);
  cx += digit_w + digit_gap;
  draw_7seg_digit(canvas, cx, y, d[3], seg_len, seg_thick, color);
  cx += digit_w + colon_gap_lr;
  draw_7seg_colon(canvas, cx, colon_y, colon_dot, colon_gap_ud, colon_on, color);
  cx += colon_w + colon_gap_lr;
  draw_7seg_digit(canvas, cx, y, d[4], seg_len, seg_thick, color);
  cx += digit_w + digit_gap;
  draw_7seg_digit(canvas, cx, y, d[5], seg_len, seg_thick, color);
}

static void draw_7seg_digits_row(rgb_matrix::Canvas *canvas,
                                 int x,
                                 int y,
                                 const char *digits,
                                 int seg_len,
                                 int seg_thick,
                                 int digit_gap,
                                 const rgb_matrix::Color &color)
{
  const int digit_w = calc_digit_width(seg_len, seg_thick);
  int cx = x;

  for (size_t i = 0; digits[i] != '\0'; ++i)
  {
    if (digits[i] >= '0' && digits[i] <= '9')
    {
      draw_7seg_digit(canvas, cx, y, digits[i] - '0', seg_len, seg_thick, color);
    }
    cx += digit_w + digit_gap;
  }
}

static int calc_7seg_digits_row_width(int num_digits, int seg_len, int seg_thick, int digit_gap)
{
  const int digit_w = calc_digit_width(seg_len, seg_thick);
  return num_digits * digit_w + (num_digits - 1) * digit_gap;
}

static void handle_sigint(int)
{
  g_running = false;
}

int main(int argc, char **argv)
{
  if (argc < 3)
  {
    std::cerr << "Usage: sudo " << argv[0]
              << " -i <interface> [-r 0-255] [-g 0-255] [-b 0-255] [-tz N] [-log]\n\n"
              << "Examples:\n"
              << "  sudo " << argv[0] << " -i eth0\n"
              << "  sudo " << argv[0] << " -i eth0 -r 0 -g 255 -b 0\n"
              << "  sudo " << argv[0] << " -i eth0 -tz -7\n"
              << "  sudo " << argv[0] << " -i eth0 -r 255 -g 255 -b 255 -log\n";
    return 1;
  }

  for (int i = 1; i < argc; ++i)
  {
    if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
    {
      g_iface_name = argv[++i];
    }
    else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc)
    {
      if (!parse_u8_arg(argv[++i], CLOCK_R))
      {
        std::cerr << "ERROR: -r must be 0-255\n";
        return 1;
      }
    }
    else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc)
    {
      if (!parse_u8_arg(argv[++i], CLOCK_G))
      {
        std::cerr << "ERROR: -g must be 0-255\n";
        return 1;
      }
    }
    else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc)
    {
      if (!parse_u8_arg(argv[++i], CLOCK_B))
      {
        std::cerr << "ERROR: -b must be 0-255\n";
        return 1;
      }
    }
    else if (strcmp(argv[i], "-tz") == 0 && i + 1 < argc)
    {
      int tz_hours = 0;
      if (!parse_int_arg(argv[++i], tz_hours))
      {
        std::cerr << "ERROR: -tz must be an integer\n";
        return 1;
      }
      TIMEZONE_OFFSET_SECONDS = tz_hours * 3600;
    }
    else if (strcmp(argv[i], "-log") == 0)
    {
      g_enable_logging = true;
    }
    else
    {
      std::cerr << "Unknown argument: " << argv[i] << "\n";
      return 1;
    }
  }

  if (g_iface_name.empty())
  {
    std::cerr << "ERROR: Must pass -i <interface>\n";
    return 1;
  }

  if (!ptpi::GetIpv4ForInterface(g_iface_name, g_iface_ipv4))
  {
    std::cerr << "ERROR: Could not get IPv4 for interface: " << g_iface_name << "\n";
    return 1;
  }

  logmsg("PTP LED Clock starting...");
  logmsg("Using interface: " + g_iface_name + " (IP: " + g_iface_ipv4 + ")");
  logmsg("Clock color: R=" + std::to_string(int(CLOCK_R)) +
         " G=" + std::to_string(int(CLOCK_G)) +
         " B=" + std::to_string(int(CLOCK_B)));
  logmsg("Timezone offset hours: " + std::to_string(TIMEZONE_OFFSET_SECONDS / 3600));

  std::signal(SIGINT, handle_sigint);
  std::signal(SIGTERM, handle_sigint);

  ptpi::PtpClockReceiver ptp({g_iface_ipv4}, logmsg);
  if (!ptp.Open())
  {
    logmsg("ERROR: Failed to create PTP sockets");
    return 1;
  }

  rgb_matrix::RGBMatrix::Options options;
  options.rows = PANEL_ROWS;
  options.cols = PANEL_COLS;
  options.chain_length = CHAIN_LENGTH;
  options.parallel = 1;
  options.brightness = BRIGHTNESS;
  options.hardware_mapping = "adafruit-hat";

  rgb_matrix::RuntimeOptions rt_opts;
  rgb_matrix::RGBMatrix *matrix = rgb_matrix::CreateMatrixFromOptions(options, rt_opts);
  if (!matrix)
  {
    logmsg("ERROR: Could not create RGBMatrix. Check wiring/options.");
    return 1;
  }

  const int width = matrix->width();
  rgb_matrix::FrameCanvas *canvas = matrix->CreateFrameCanvas();

  rgb_matrix::Font status_font;
  if (!status_font.LoadFont(STATUS_FONT_PATH))
  {
    logmsg(std::string("ERROR: Failed to load status font: ") + STATUS_FONT_PATH);
    delete matrix;
    return 1;
  }

  rgb_matrix::Color clock_color(CLOCK_R, CLOCK_B, CLOCK_G);
  rgb_matrix::Color status_color(STATUS_R, STATUS_B, STATUS_G);

  uint64_t last_frame_ns = ptpi::GetMonotonicNs();

  while (g_running)
  {
    if (!ptp.Poll())
    {
      break;
    }

    sleep_until_next_frame(last_frame_ns);
    canvas->Clear();

    const ptpi::PtpSnapshot snapshot = ptp.GetSnapshot();
    const rgb_matrix::Color color = snapshot.time_valid ? clock_color : status_color;

    if (!snapshot.have_ptp_time)
    {
      const char *msg = "Waiting for PTP...";
      int x = (width - int(std::strlen(msg)) * STATUS_CHAR_WIDTH) / 2;
      if (x < 0)
      {
        x = 0;
      }
      rgb_matrix::DrawText(canvas, status_font, x, 22, color, msg);
    }
    else if (!snapshot.time_valid)
    {
      const char *msg = "Lost PTP!";
      int x = (width - int(std::strlen(msg)) * STATUS_CHAR_WIDTH) / 2;
      if (x < 0)
      {
        x = 0;
      }
      rgb_matrix::DrawText(canvas, status_font, x, 22, color, msg);
    }
    else
    {
      int hh = 0;
      int mm = 0;
      int ss = 0;
      uint32_t nsec = 0;
      if (ptp.GetCurrentLocalTime(TIMEZONE_OFFSET_SECONDS, hh, mm, ss, nsec))
      {
        const int top_seg_len = 7;
        const int top_seg_thick = 3;
        const int top_digit_w = calc_digit_width(top_seg_len, top_seg_thick);
        const int top_digit_gap = 2;
        const int top_colon_w = 2;
        const int top_colon_gap_lr = 2;
        const int top_width = (6 * top_digit_w) +
                              (3 * top_digit_gap) +
                              (2 * (top_colon_gap_lr + top_colon_w + top_colon_gap_lr));

        draw_7seg_time(canvas, (width - top_width) / 2, 0, hh, mm, ss, true, color);

        char bottom_line[16];
        std::snprintf(bottom_line, sizeof(bottom_line), "%09u", nsec);

        const int nano_seg_len = 2;
        const int nano_seg_thick = 1;
        const int nano_digit_gap = 2;
        const int nano_y = 25;
        const int nano_digits_width =
            calc_7seg_digits_row_width(9, nano_seg_len, nano_seg_thick, nano_digit_gap);
        const int nano_colon_w = 1;
        const int nano_colon_gap_lr = 2;
        int bottom_x = (width - (nano_colon_w + nano_colon_gap_lr + nano_digits_width)) / 2;
        if (bottom_x < 0)
        {
          bottom_x = 0;
        }

        draw_7seg_colon(canvas, bottom_x, nano_y + 1, 1, 2, true, color);
        draw_7seg_digits_row(canvas,
                             bottom_x + nano_colon_w + nano_colon_gap_lr,
                             nano_y,
                             bottom_line,
                             nano_seg_len,
                             nano_seg_thick,
                             nano_digit_gap,
                             color);
      }
      else
      {
        const char *msg = "ERROR!!";
        int x = (width - int(std::strlen(msg)) * STATUS_CHAR_WIDTH) / 2;
        if (x < 0)
        {
          x = 0;
        }
        rgb_matrix::DrawText(canvas, status_font, x, 22, color, msg);
      }
    }

    canvas = matrix->SwapOnVSync(canvas);
  }

  matrix->Clear();
  delete matrix;
  ptp.Close();

  logmsg("PTP LED Clock exiting.");
  return 0;
}
