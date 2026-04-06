// PTP LED Clock for Raspberry Pi
// - Uses the shared PTP packet-processing library
// - Displays HH:MM:SS.nnnnnnnnn on a 128x32 RGB matrix (2 x 64x32 chained)
// - Takes interface argument and optional clock RGB color arguments
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

static uint8_t CLOCK_R = 165;
static uint8_t CLOCK_G = 40;
static uint8_t CLOCK_B = 0;

static const uint8_t STATUS_R = 255;
static const uint8_t STATUS_G = 0;
static const uint8_t STATUS_B = 0;

static int TIMEZONE_OFFSET_SECONDS = 0;

static const double FRAME_INTERVAL_SEC = 0.01;
static const char *FONT_PATH = "/opt/ptpi-clock/fonts/7x14B.bdf";

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
  options.led_rgb_sequence = "RBG";
  options.hardware_mapping = "adafruit-hat-pwm";
  options.pwm_bits = 3;

  rgb_matrix::RuntimeOptions rt_opts;
  rgb_matrix::RGBMatrix *matrix = rgb_matrix::CreateMatrixFromOptions(options, rt_opts);
  if (!matrix)
  {
    logmsg("ERROR: Could not create RGBMatrix. Check wiring/options.");
    return 1;
  }

  const int width = matrix->width();
  const int height = matrix->height();
  rgb_matrix::FrameCanvas *canvas = matrix->CreateFrameCanvas();

  rgb_matrix::Font font;
  if (!font.LoadFont(FONT_PATH))
  {
    logmsg(std::string("ERROR: Failed to load font: ") + FONT_PATH);
    delete matrix;
    return 1;
  }

  rgb_matrix::Color clock_color(CLOCK_R, CLOCK_G, CLOCK_B);
  rgb_matrix::Color status_color(STATUS_R, STATUS_G, STATUS_B);
  const int baseline_y = height / 2 + 4;

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

    char line[64];
    if (!snapshot.have_ptp_time)
    {
      std::snprintf(line, sizeof(line), "Waiting for PTP...");
    }
    else if (!snapshot.time_valid)
    {
      std::snprintf(line, sizeof(line), "Lost PTP!");
    }
    else
    {
      int hh = 0;
      int mm = 0;
      int ss = 0;
      uint32_t nsec = 0;
      if (ptp.GetCurrentLocalTime(TIMEZONE_OFFSET_SECONDS, hh, mm, ss, nsec))
      {
        std::snprintf(line, sizeof(line), "%02d:%02d:%02d.%09u", hh, mm, ss, nsec);
      }
      else
      {
        std::snprintf(line, sizeof(line), "ERROR!!");
      }
    }

    const int char_width = 7;
    int x = (width - int(std::strlen(line)) * char_width) / 2;
    if (x < 0)
    {
      x = 0;
    }

    rgb_matrix::DrawText(canvas, font, x, baseline_y, color, line);
    canvas = matrix->SwapOnVSync(canvas);
  }

  matrix->Clear();
  delete matrix;
  ptp.Close();

  logmsg("PTP LED Clock exiting.");
  return 0;
}
