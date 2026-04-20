// PTP LED Clock for Raspberry Pi
// - Uses the shared PTP packet-processing library
// - Supports multiple clock-face renderers in one binary
// - Logging is disabled by default; enable with -log
// - Timezone defaults to UTC; override with -tz N

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
static const int BRIGHTNESS = 85;

static const uint8_t DEFAULT_CLOCK_R = 255;
static const uint8_t DEFAULT_CLOCK_G = 115;
static const uint8_t DEFAULT_CLOCK_B = 5;

static const uint8_t STATUS_R = 255;
static const uint8_t STATUS_G = 0;
static const uint8_t STATUS_B = 0;

static const double FRAME_INTERVAL_SEC = 0.01;

static const char *STATUS_FONT_PATH = "/opt/ptpi-clock/fonts/7x14B.bdf";
static const char *OG_FONT_PATH = "/opt/ptpi-clock/fonts/7x14B.bdf";
static const char *TOP_FONT_PATH = "/opt/ptpi-clock/fonts/10x20.bdf";
static const char *BOTTOM_FONT_PATH = "/opt/ptpi-clock/fonts/5x8.bdf";

static const int STATUS_CHAR_WIDTH = 7;
static const int OG_CHAR_WIDTH = 7;
static const int TOP_CHAR_WIDTH = 10;
static const int BOTTOM_CHAR_WIDTH = 5;

enum class ParseResult
{
  kOk,
  kExitSuccess,
  kExitFailure,
};

enum class ClockFace
{
  k7Seg,
  k2Line,
  kOg,
};

struct ProgramOptions
{
  std::string iface_name;
  uint8_t clock_r = DEFAULT_CLOCK_R;
  uint8_t clock_g = DEFAULT_CLOCK_G;
  uint8_t clock_b = DEFAULT_CLOCK_B;
  int timezone_offset_seconds = 0;
  bool enable_logging = false;
  ClockFace face = ClockFace::k7Seg;
};

struct Fonts
{
  rgb_matrix::Font status_font;
  rgb_matrix::Font og_font;
  rgb_matrix::Font top_font;
  rgb_matrix::Font bottom_font;
};

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

static const char *clock_face_to_string(ClockFace face)
{
  switch (face)
  {
  case ClockFace::k7Seg:
    return "7seg";
  case ClockFace::k2Line:
    return "2line";
  case ClockFace::kOg:
    return "og";
  }
  return "7seg";
}

static bool parse_face_arg(const char *value, ClockFace &out)
{
  if (std::strcmp(value, "7seg") == 0)
  {
    out = ClockFace::k7Seg;
    return true;
  }
  if (std::strcmp(value, "2line") == 0)
  {
    out = ClockFace::k2Line;
    return true;
  }
  if (std::strcmp(value, "og") == 0)
  {
    out = ClockFace::kOg;
    return true;
  }
  return false;
}

static void print_usage(const char *program)
{
  std::cerr << "Usage: sudo " << program
            << " -i <interface> [--clock 7seg|2line|og] [-r 0-255] [-g 0-255] [-b 0-255] [-tz N] [-log]\n\n"
            << "Examples:\n"
            << "  sudo " << program << " -i eth0\n"
            << "  sudo " << program << " -i eth0 --clock og\n"
            << "  sudo " << program << " -i eth0 -c 2line -r 0 -g 255 -b 0\n"
            << "  sudo " << program << " -i eth0 -tz -7\n"
            << "  sudo " << program << " -i eth0 -log\n";
}

static ParseResult parse_args(int argc, char **argv, ProgramOptions &options)
{
  if (argc == 2 && (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0))
  {
    print_usage(argv[0]);
    return ParseResult::kExitSuccess;
  }

  if (argc < 3)
  {
    print_usage(argv[0]);
    return ParseResult::kExitFailure;
  }

  for (int i = 1; i < argc; ++i)
  {
    if (std::strcmp(argv[i], "-i") == 0 && i + 1 < argc)
    {
      options.iface_name = argv[++i];
    }
    else if ((std::strcmp(argv[i], "--clock") == 0 || std::strcmp(argv[i], "-c") == 0) && i + 1 < argc)
    {
      if (!parse_face_arg(argv[++i], options.face))
      {
        std::cerr << "ERROR: --clock must be one of: 7seg, 2line, og\n";
        return ParseResult::kExitFailure;
      }
    }
    else if (std::strcmp(argv[i], "-r") == 0 && i + 1 < argc)
    {
      if (!parse_u8_arg(argv[++i], options.clock_r))
      {
        std::cerr << "ERROR: -r must be 0-255\n";
        return ParseResult::kExitFailure;
      }
    }
    else if (std::strcmp(argv[i], "-g") == 0 && i + 1 < argc)
    {
      if (!parse_u8_arg(argv[++i], options.clock_g))
      {
        std::cerr << "ERROR: -g must be 0-255\n";
        return ParseResult::kExitFailure;
      }
    }
    else if (std::strcmp(argv[i], "-b") == 0 && i + 1 < argc)
    {
      if (!parse_u8_arg(argv[++i], options.clock_b))
      {
        std::cerr << "ERROR: -b must be 0-255\n";
        return ParseResult::kExitFailure;
      }
    }
    else if (std::strcmp(argv[i], "-tz") == 0 && i + 1 < argc)
    {
      int tz_hours = 0;
      if (!parse_int_arg(argv[++i], tz_hours))
      {
        std::cerr << "ERROR: -tz must be an integer\n";
        return ParseResult::kExitFailure;
      }
      options.timezone_offset_seconds = tz_hours * 3600;
    }
    else if (std::strcmp(argv[i], "-log") == 0)
    {
      options.enable_logging = true;
    }
    else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)
    {
      print_usage(argv[0]);
      return ParseResult::kExitSuccess;
    }
    else
    {
      std::cerr << "Unknown argument: " << argv[i] << "\n";
      print_usage(argv[0]);
      return ParseResult::kExitFailure;
    }
  }

  if (options.iface_name.empty())
  {
    std::cerr << "ERROR: Must pass -i <interface>\n";
    return ParseResult::kExitFailure;
  }

  return ParseResult::kOk;
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

static void handle_sigint(int)
{
  g_running = false;
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

static void draw_centered_text(rgb_matrix::Canvas *canvas,
                               rgb_matrix::Font &font,
                               int width,
                               int baseline_y,
                               const rgb_matrix::Color &color,
                               const char *text,
                               int char_width)
{
  int x = (width - int(std::strlen(text)) * char_width) / 2;
  if (x < 0)
  {
    x = 0;
  }
  rgb_matrix::DrawText(canvas, font, x, baseline_y, color, text);
}

static bool load_fonts(const ProgramOptions &options, Fonts &fonts)
{
  if (!fonts.status_font.LoadFont(STATUS_FONT_PATH))
  {
    logmsg(std::string("ERROR: Failed to load status font: ") + STATUS_FONT_PATH);
    return false;
  }

  if (options.face == ClockFace::kOg)
  {
    if (!fonts.og_font.LoadFont(OG_FONT_PATH))
    {
      logmsg(std::string("ERROR: Failed to load font: ") + OG_FONT_PATH);
      return false;
    }
  }
  else if (options.face == ClockFace::k2Line)
  {
    if (!fonts.top_font.LoadFont(TOP_FONT_PATH))
    {
      logmsg(std::string("ERROR: Failed to load top font: ") + TOP_FONT_PATH);
      return false;
    }
    if (!fonts.bottom_font.LoadFont(BOTTOM_FONT_PATH))
    {
      logmsg(std::string("ERROR: Failed to load bottom font: ") + BOTTOM_FONT_PATH);
      return false;
    }
  }

  return true;
}

static void render_status(rgb_matrix::Canvas *canvas,
                          const ptpi::PtpSnapshot &snapshot,
                          rgb_matrix::Font &status_font,
                          int width,
                          const rgb_matrix::Color &color)
{
  const char *msg = snapshot.have_ptp_time ? "Lost PTP!" : "Waiting for PTP...";
  draw_centered_text(canvas, status_font, width, 22, color, msg, STATUS_CHAR_WIDTH);
}

static void render_og_face(rgb_matrix::Canvas *canvas,
                           rgb_matrix::Font &font,
                           int width,
                           int height,
                           const rgb_matrix::Color &color,
                           int hh,
                           int mm,
                           int ss,
                           uint32_t nsec)
{
  char line[64];
  std::snprintf(line, sizeof(line), "%02d:%02d:%02d.%09u", hh, mm, ss, nsec);
  const int baseline_y = height / 2 + 4;
  draw_centered_text(canvas, font, width, baseline_y, color, line, OG_CHAR_WIDTH);
}

static void render_2line_face(rgb_matrix::Canvas *canvas,
                              rgb_matrix::Font &top_font,
                              rgb_matrix::Font &bottom_font,
                              int width,
                              const rgb_matrix::Color &color,
                              int hh,
                              int mm,
                              int ss,
                              uint32_t nsec)
{
  char top_line[16];
  char bottom_line[16];
  std::snprintf(top_line, sizeof(top_line), "%02d:%02d:%02d", hh, mm, ss);
  std::snprintf(bottom_line, sizeof(bottom_line), ":%09u", nsec);

  const int top_baseline = 20;
  const int bottom_baseline = 31;
  int top_x = (width - int(std::strlen(top_line)) * TOP_CHAR_WIDTH) / 2;
  int bottom_x = (width - int(std::strlen(bottom_line)) * BOTTOM_CHAR_WIDTH) / 2;
  if (top_x < 0)
  {
    top_x = 0;
  }
  if (bottom_x < 0)
  {
    bottom_x = 0;
  }

  rgb_matrix::DrawText(canvas, top_font, top_x, top_baseline, color, top_line);
  rgb_matrix::DrawText(canvas, top_font, top_x + 1, top_baseline, color, top_line);
  rgb_matrix::DrawText(canvas, bottom_font, bottom_x, bottom_baseline, color, bottom_line);
}

static void render_7seg_face(rgb_matrix::Canvas *canvas,
                             int width,
                             const rgb_matrix::Color &color,
                             int hh,
                             int mm,
                             int ss,
                             uint32_t nsec)
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

  int top_x = (width - top_width) / 2;
  if (top_x < 0)
  {
    top_x = 0;
  }
  draw_7seg_time(canvas, top_x, 0, hh, mm, ss, true, color);

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

static void render_time_face(rgb_matrix::Canvas *canvas,
                             const ProgramOptions &options,
                             Fonts &fonts,
                             int width,
                             int height,
                             const rgb_matrix::Color &color,
                             int hh,
                             int mm,
                             int ss,
                             uint32_t nsec)
{
  switch (options.face)
  {
  case ClockFace::k7Seg:
    render_7seg_face(canvas, width, color, hh, mm, ss, nsec);
    break;
  case ClockFace::k2Line:
    render_2line_face(canvas, fonts.top_font, fonts.bottom_font, width, color, hh, mm, ss, nsec);
    break;
  case ClockFace::kOg:
    render_og_face(canvas, fonts.og_font, width, height, color, hh, mm, ss, nsec);
    break;
  }
}

int main(int argc, char **argv)
{
  ProgramOptions options;
  const ParseResult parse_result = parse_args(argc, argv, options);
  if (parse_result != ParseResult::kOk)
  {
    return parse_result == ParseResult::kExitSuccess ? 0 : 1;
  }

  g_enable_logging = options.enable_logging;

  if (!ptpi::GetIpv4ForInterface(options.iface_name, g_iface_ipv4))
  {
    std::cerr << "ERROR: Could not get IPv4 for interface: " << options.iface_name << "\n";
    return 1;
  }

  logmsg("PTP LED Clock starting...");
  logmsg("Using interface: " + options.iface_name + " (IP: " + g_iface_ipv4 + ")");
  logmsg("Face: " + std::string(clock_face_to_string(options.face)));
  logmsg("Clock color: R=" + std::to_string(int(options.clock_r)) +
         " G=" + std::to_string(int(options.clock_g)) +
         " B=" + std::to_string(int(options.clock_b)));
  logmsg("Timezone offset hours: " + std::to_string(options.timezone_offset_seconds / 3600));

  std::signal(SIGINT, handle_sigint);
  std::signal(SIGTERM, handle_sigint);

  ptpi::PtpClockReceiver ptp({g_iface_ipv4}, logmsg);
  if (!ptp.Open())
  {
    logmsg("ERROR: Failed to create PTP sockets");
    return 1;
  }

  rgb_matrix::RGBMatrix::Options matrix_options;
  matrix_options.rows = PANEL_ROWS;
  matrix_options.cols = PANEL_COLS;
  matrix_options.chain_length = CHAIN_LENGTH;
  matrix_options.parallel = 1;
  matrix_options.brightness = BRIGHTNESS;
  matrix_options.led_rgb_sequence = "RBG";
  matrix_options.hardware_mapping = "adafruit-hat-pwm";
  matrix_options.pwm_bits = 3;

  rgb_matrix::RuntimeOptions rt_opts;
  rgb_matrix::RGBMatrix *matrix = rgb_matrix::CreateMatrixFromOptions(matrix_options, rt_opts);
  if (!matrix)
  {
    logmsg("ERROR: Could not create RGBMatrix. Check wiring/options.");
    ptp.Close();
    return 1;
  }

  Fonts fonts;
  if (!load_fonts(options, fonts))
  {
    delete matrix;
    ptp.Close();
    return 1;
  }

  const int width = matrix->width();
  const int height = matrix->height();
  rgb_matrix::FrameCanvas *canvas = matrix->CreateFrameCanvas();

  rgb_matrix::Color clock_color(options.clock_r, options.clock_g, options.clock_b);
  rgb_matrix::Color status_color(STATUS_R, STATUS_G, STATUS_B);
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

    if (!snapshot.have_ptp_time || !snapshot.time_valid)
    {
      render_status(canvas, snapshot, fonts.status_font, width, color);
    }
    else
    {
      int hh = 0;
      int mm = 0;
      int ss = 0;
      uint32_t nsec = 0;
      if (ptp.GetCurrentLocalTime(options.timezone_offset_seconds, hh, mm, ss, nsec))
      {
        render_time_face(canvas, options, fonts, width, height, color, hh, mm, ss, nsec);
      }
      else
      {
        draw_centered_text(canvas, fonts.status_font, width, 22, status_color, "ERROR!!", STATUS_CHAR_WIDTH);
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
