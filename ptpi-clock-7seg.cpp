
// PTP LED Clock for Raspberry Pi
// - Listens to PTPv2 multicast (224.0.1.129) on ports 319/320
// - Supports both 1-step and 2-step clocks
// - Uses Announce currentUtcOffset (TAI - UTC) to convert TAI -> UTC
// - Uses kernel software RX timestamps (SO_TIMESTAMPNS) to reduce socket timing error
// - Smooths between packets using CLOCK_MONOTONIC
// - Displays:
//      HH:MM:SS
//      :nnnnnnnnn
// - Uses font for status/error text:
// - Takes interface argument and optional clock RGB color arguments
// - Logging is disabled by default; enable with -log
// - Timezone defaults to UTC; override with -tz N

#include <iostream>
#include <string>
#include <cstdint>
#include <cstring>
#include <csignal>
#include <cerrno>
#include <cstdlib>

#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include <ifaddrs.h>
#include <net/if.h>

#include "led-matrix.h"
#include "graphics.h"

static const int PANEL_ROWS = 32;
static const int PANEL_COLS = 64;
static const int CHAIN_LENGTH = 2;

static const int BRIGHTNESS = 100;

// Default clock color: red
static uint8_t CLOCK_R = 165;
static uint8_t CLOCK_G = 40;
static uint8_t CLOCK_B = 0;

// Status/error color
static const uint8_t STATUS_R = 255;
static const uint8_t STATUS_G = 0;
static const uint8_t STATUS_B = 0;

// Default timezone: UTC
static int TIMEZONE_OFFSET_SECONDS = 0;

static const double FRAME_INTERVAL_SEC = 0.01;

// If no new valid PTP time packet error timeout.
static const uint64_t PTP_TIMEOUT_NS = 1500000000ULL; // 1.5 seconds

static const char *PTP_MCAST_ADDR = "224.0.1.129";
static const uint16_t PTP_EVENT_PORT = 319;
static const uint16_t PTP_GENERAL_PORT = 320;

// Status font
static const char *STATUS_FONT_PATH = "/opt/ptpi-clock/fonts/7x14B.bdf";
static const int STATUS_CHAR_WIDTH = 7;

static std::string g_iface_name;
static std::string g_iface_ipv4;
static bool g_enable_logging = false;

struct PtpState
{
    int16_t utc_offset = 37;
    bool utc_offset_valid = false;

    uint64_t last_utc_sec = 0;
    uint32_t last_utc_nsec = 0;
    uint64_t last_mono_ns = 0;

    uint64_t last_ptp_rx_ns = 0;
    bool have_ptp_time = false;
};

static PtpState g_ptp;
static volatile bool g_running = true;

static inline void logmsg(const std::string &msg)
{
    if (g_enable_logging)
    {
        std::cout << msg << std::endl;
    }
}

static inline void log_errno(const std::string &prefix)
{
    if (g_enable_logging)
    {
        std::cerr << prefix << ": " << std::strerror(errno) << std::endl;
    }
}

static inline uint64_t timespec_to_ns(const struct timespec &ts)
{
    return uint64_t(ts.tv_sec) * 1000000000ULL + uint64_t(ts.tv_nsec);
}

static inline uint64_t get_monotonic_ns()
{
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return timespec_to_ns(ts);
}

static inline uint64_t get_realtime_ns()
{
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return timespec_to_ns(ts);
}

static uint64_t realtime_ns_to_monotonic_ns(uint64_t realtime_ns)
{
    uint64_t mono_now = get_monotonic_ns();
    uint64_t real_now = get_realtime_ns();

    int64_t offset = int64_t(mono_now) - int64_t(real_now);
    int64_t mono_est = int64_t(realtime_ns) + offset;
    if (mono_est < 0)
        return 0;
    return uint64_t(mono_est);
}

static bool get_ipv4_for_interface(const std::string &iface, std::string &out_ip)
{
    struct ifaddrs *ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1)
        return false;

    bool found = false;
    for (struct ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == nullptr)
            continue;
        if (iface != ifa->ifa_name)
            continue;

        if (ifa->ifa_addr->sa_family == AF_INET)
        {
            char buf[INET_ADDRSTRLEN];
            void *addr = &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
            if (inet_ntop(AF_INET, addr, buf, sizeof(buf)))
            {
                out_ip = buf;
                found = true;
                break;
            }
        }
    }

    freeifaddrs(ifaddr);
    return found;
}

static inline uint16_t read_be16(const uint8_t *buf, size_t offset)
{
    return uint16_t(buf[offset] << 8) | uint16_t(buf[offset + 1]);
}

static inline uint32_t read_be32(const uint8_t *buf, size_t offset)
{
    return (uint32_t(buf[offset]) << 24) |
           (uint32_t(buf[offset + 1]) << 16) |
           (uint32_t(buf[offset + 2]) << 8) |
           (uint32_t(buf[offset + 3]));
}

static inline uint64_t read_be48(const uint8_t *buf, size_t offset)
{
    uint64_t b0 = buf[offset];
    uint64_t b1 = buf[offset + 1];
    uint64_t b2 = buf[offset + 2];
    uint64_t b3 = buf[offset + 3];
    uint64_t b4 = buf[offset + 4];
    uint64_t b5 = buf[offset + 5];
    return (b0 << 40) | (b1 << 32) | (b2 << 24) | (b3 << 16) | (b4 << 8) | b5;
}

static bool parse_u8_arg(const char *s, uint8_t &out)
{
    char *end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 0 || v > 255)
        return false;
    out = static_cast<uint8_t>(v);
    return true;
}

static bool parse_int_arg(const char *s, int &out)
{
    char *end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (end == s || *end != '\0')
        return false;
    out = static_cast<int>(v);
    return true;
}

//  -----------------------------------------------------------------------------
//  PTP time update & extrapolation
//  -----------------------------------------------------------------------------

static void update_utc_from_ptp(uint64_t utc_sec, uint32_t utc_nsec, uint64_t rx_mono_ns)
{
    bool first_lock = !g_ptp.have_ptp_time;

    g_ptp.last_utc_sec = utc_sec;
    g_ptp.last_utc_nsec = utc_nsec;
    g_ptp.last_mono_ns = rx_mono_ns;
    g_ptp.last_ptp_rx_ns = get_monotonic_ns();
    g_ptp.have_ptp_time = true;

    if (first_lock)
    {
        logmsg("[PTP] First valid UTC time lock");
    }
}

static bool get_current_utc(uint64_t &utc_sec, uint32_t &utc_nsec)
{
    if (!g_ptp.have_ptp_time)
        return false;

    uint64_t now_mono = get_monotonic_ns();
    if ((now_mono - g_ptp.last_ptp_rx_ns) > PTP_TIMEOUT_NS)
        return false;

    uint64_t delta_ns = now_mono - g_ptp.last_mono_ns;
    uint64_t base_ns = uint64_t(g_ptp.last_utc_nsec) + delta_ns;

    utc_sec = g_ptp.last_utc_sec + (base_ns / 1000000000ULL);
    utc_nsec = uint32_t(base_ns % 1000000000ULL);
    return true;
}

static bool get_current_local_time(int &hh, int &mm, int &ss, uint32_t &nsec)
{
    uint64_t utc_sec;
    uint32_t utc_nsec;
    if (!get_current_utc(utc_sec, utc_nsec))
        return false;

    int64_t local_sec = int64_t(utc_sec) + int64_t(TIMEZONE_OFFSET_SECONDS);
    int64_t sec_of_day = local_sec % 86400;
    if (sec_of_day < 0)
        sec_of_day += 86400;

    hh = int(sec_of_day / 3600);
    mm = int((sec_of_day % 3600) / 60);
    ss = int(sec_of_day % 60);
    nsec = utc_nsec;
    return true;
}

// -----------------------------------------------------------------------------
// PTP packet parsing
// -----------------------------------------------------------------------------

enum PtpMessageType : uint8_t
{
    PTP_SYNC = 0x0,
    PTP_FOLLOW_UP = 0x8,
    PTP_ANNOUNCE = 0xB
};

static void handle_announce(const uint8_t *buf, size_t len)
{
    if (len < 46)
        return;

    int16_t current_utc_offset = (int16_t)read_be16(buf, 44);
    g_ptp.utc_offset = current_utc_offset;
    g_ptp.utc_offset_valid = true;

    logmsg("[PTP] Announce: currentUtcOffset = " + std::to_string(g_ptp.utc_offset));
}

static void handle_sync(const uint8_t *buf, size_t len, uint64_t rx_mono_ns)
{
    if (len < 44)
        return;

    uint8_t version_ptp = buf[1] & 0x0F;
    if (version_ptp != 2)
        return;

    uint16_t flag_field = read_be16(buf, 6);
    bool two_step_flag = (flag_field & (1 << 9)) != 0;
    if (two_step_flag)
        return;

    uint64_t tai_sec = read_be48(buf, 34);
    uint32_t nsec = read_be32(buf, 40);

    int16_t offset = g_ptp.utc_offset_valid ? g_ptp.utc_offset : 37;
    uint64_t utc_sec = (tai_sec >= (uint64_t)offset) ? (tai_sec - (uint64_t)offset) : tai_sec;

    if (g_enable_logging)
    {
        std::cout << "[PTP] Sync (1-step): TAI=" << tai_sec
                  << " ns=" << nsec << " -> UTC=" << utc_sec
                  << " rx_mono_ns=" << rx_mono_ns << std::endl;
    }

    update_utc_from_ptp(utc_sec, nsec, rx_mono_ns);
}

static void handle_follow_up(const uint8_t *buf, size_t len, uint64_t rx_mono_ns)
{
    if (len < 44)
        return;

    uint8_t version_ptp = buf[1] & 0x0F;
    if (version_ptp != 2)
        return;

    uint64_t tai_sec = read_be48(buf, 34);
    uint32_t nsec = read_be32(buf, 40);

    int16_t offset = g_ptp.utc_offset_valid ? g_ptp.utc_offset : 37;
    uint64_t utc_sec = (tai_sec >= (uint64_t)offset) ? (tai_sec - (uint64_t)offset) : tai_sec;

    if (g_enable_logging)
    {
        std::cout << "[PTP] Follow_Up (2-step): TAI=" << tai_sec
                  << " ns=" << nsec << " -> UTC=" << utc_sec
                  << " rx_mono_ns=" << rx_mono_ns << std::endl;
    }

    update_utc_from_ptp(utc_sec, nsec, rx_mono_ns);
}

static void parse_ptp_packet(const uint8_t *buf, size_t len, uint64_t rx_mono_ns)
{
    if (len < 34)
        return;

    uint8_t message_type = buf[0] & 0x0F;
    uint8_t version_ptp = buf[1] & 0x0F;
    if (version_ptp != 2)
        return;

    switch (message_type)
    {
    case PTP_ANNOUNCE:
        handle_announce(buf, len);
        break;
    case PTP_SYNC:
        handle_sync(buf, len, rx_mono_ns);
        break;
    case PTP_FOLLOW_UP:
        handle_follow_up(buf, len, rx_mono_ns);
        break;
    default:
        break;
    }
}

// -----------------------------------------------------------------------------
// Socket setup
// -----------------------------------------------------------------------------

static int make_ptp_socket(uint16_t port)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0)
    {
        log_errno("socket");
        return -1;
    }

    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
    {
        log_errno("setsockopt(SO_REUSEADDR)");
        close(sock);
        return -1;
    }

    int enable_ts = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_TIMESTAMPNS, &enable_ts, sizeof(enable_ts)) < 0)
    {
        log_errno("setsockopt(SO_TIMESTAMPNS)");
        close(sock);
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        log_errno("bind");
        close(sock);
        return -1;
    }

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(PTP_MCAST_ADDR);
    mreq.imr_interface.s_addr = inet_addr(g_iface_ipv4.c_str());

    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
    {
        log_errno("setsockopt(IP_ADD_MEMBERSHIP)");
        close(sock);
        return -1;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0)
        flags = 0;
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        log_errno("fcntl(O_NONBLOCK)");
        close(sock);
        return -1;
    }

    return sock;
}

static ssize_t recv_ptp_packet_with_timestamp(
    int sock,
    uint8_t *buf,
    size_t buf_size,
    uint64_t &rx_mono_ns)
{
    char control[256];
    struct iovec iov{};
    iov.iov_base = buf;
    iov.iov_len = buf_size;

    struct msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    ssize_t n = recvmsg(sock, &msg, 0);
    if (n < 0)
    {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            return 0;
        return -1;
    }

    bool found_ts = false;
    struct timespec ts_rx{};

    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
         cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&msg, cmsg))
    {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMPNS)
        {
            std::memcpy(&ts_rx, CMSG_DATA(cmsg), sizeof(ts_rx));
            found_ts = true;
            break;
        }
    }

    if (found_ts)
    {
        uint64_t rx_real_ns = timespec_to_ns(ts_rx);
        rx_mono_ns = realtime_ns_to_monotonic_ns(rx_real_ns);
    }
    else
    {
        rx_mono_ns = get_monotonic_ns();
    }

    return n;
}

// -----------------------------------------------------------------------------
// 7-segment renderer
// -----------------------------------------------------------------------------

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

// Segment bits: a b c d e f g
static const uint8_t DIGIT_MASKS[10] = {
    0b1111110, // 0
    0b0110000, // 1
    0b1101101, // 2
    0b1111001, // 3
    0b0110011, // 4
    0b1011011, // 5
    0b1011111, // 6
    0b1110000, // 7
    0b1111111, // 8
    0b1111011  // 9
};

static void draw_7seg_digit(
    rgb_matrix::Canvas *canvas,
    int x,
    int y,
    int digit,
    int seg_len,
    int seg_thick,
    const rgb_matrix::Color &color)
{
    if (digit < 0 || digit > 9)
        return;

    const uint8_t mask = DIGIT_MASKS[digit];
    const int w = seg_len;
    const int t = seg_thick;

    // Slight overlap at segment joints makes the style look more unified.
    const int overlap = (t > 1) ? 1 : 0;
    // horizontal
    if (mask & 0b1000000)
        fill_rect(canvas, x + t - overlap, y, w + 2 * overlap, t, color); // a
    if (mask & 0b0000001)
        fill_rect(canvas, x + t - overlap, y + t + w, w + 2 * overlap, t, color); // g
    if (mask & 0b0001000)
        fill_rect(canvas, x + t - overlap, y + 2 * t + 2 * w, w + 2 * overlap, t, color); // d

    // vertical
    if (mask & 0b0000010)
        fill_rect(canvas, x, y + t - overlap, t, w + 2 * overlap, color); // f
    if (mask & 0b0100000)
        fill_rect(canvas, x + t + w, y + t - overlap, t, w + 2 * overlap, color); // b
    if (mask & 0b0000100)
        fill_rect(canvas, x, y + 2 * t + w - overlap, t, w + 2 * overlap, color); // e
    if (mask & 0b0010000)
        fill_rect(canvas, x + t + w, y + 2 * t + w - overlap, t, w + 2 * overlap, color); // c

    // Compensation so 1, 4, and 7 visually match full-height digits.
    if (digit == 1 || digit == 4 || digit == 7)
    {
        fill_rect(canvas, x + t + w - 1, y, t + 1, t, color);
        fill_rect(canvas, x + t + w - 1, y + 2 * t + 2 * w, t + 1, t, color);
    }
}

static void draw_7seg_colon(
    rgb_matrix::Canvas *canvas,
    int x,
    int y,
    int dot_size,
    int gap,
    bool on,
    const rgb_matrix::Color &color)
{
    if (!on)
        return;
    fill_rect(canvas, x, y, dot_size, dot_size, color);
    fill_rect(canvas, x, y + dot_size + gap, dot_size, dot_size, color);
}

static int calc_digit_width(int seg_len, int seg_thick)
{
    return seg_len + 2 * seg_thick;
}

static void draw_7seg_time(rgb_matrix::Canvas *canvas, int x, int y, int hh, int mm, int ss, bool colon_on, const rgb_matrix::Color &color)
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

    int d[6] = {
        hh / 10,
        hh % 10,
        mm / 10,
        mm % 10,
        ss / 10,
        ss % 10};

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

static void draw_7seg_digits_row(
    rgb_matrix::Canvas *canvas,
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
        if ((strcmp(argv[i], "-i") == 0) &&
            i + 1 < argc)
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

    if (!get_ipv4_for_interface(g_iface_name, g_iface_ipv4))
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

    int sock_event = make_ptp_socket(PTP_EVENT_PORT);
    int sock_general = make_ptp_socket(PTP_GENERAL_PORT);

    if (sock_event < 0 || sock_general < 0)
    {
        logmsg("ERROR: Failed to create PTP sockets");
        return 1;
    }

    logmsg(std::string("[PTP] Listening on ") + PTP_MCAST_ADDR +
           ":" + std::to_string(PTP_EVENT_PORT) +
           " and :" + std::to_string(PTP_GENERAL_PORT) +
           " via " + g_iface_ipv4);

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
        close(sock_event);
        close(sock_general);
        return 1;
    }

    int width = matrix->width();

    rgb_matrix::FrameCanvas *canvas = matrix->CreateFrameCanvas();

    rgb_matrix::Font status_font;
    if (!status_font.LoadFont(STATUS_FONT_PATH))
    {
        logmsg(std::string("ERROR: Failed to load status font: ") + STATUS_FONT_PATH);
        delete matrix;
        close(sock_event);
        close(sock_general);
        return 1;
    }

    // Panel/channel mapping note:
    // This display has green/blue reversed relative to logical RGB input,
    // so swap G and B when creating colors.
    rgb_matrix::Color clock_color(CLOCK_R, CLOCK_B, CLOCK_G);
    rgb_matrix::Color status_color(STATUS_R, STATUS_B, STATUS_G);

    uint8_t buf[512];
    uint64_t last_frame_ns = get_monotonic_ns();
    const uint64_t frame_interval_ns = (uint64_t)(FRAME_INTERVAL_SEC * 1e9);

    while (g_running)
    {
        for (int sock : {sock_event, sock_general})
        {
            while (true)
            {
                uint64_t rx_mono_ns = 0;
                ssize_t n = recv_ptp_packet_with_timestamp(sock, buf, sizeof(buf), rx_mono_ns);
                if (n < 0)
                {
                    log_errno("recvmsg");
                    break;
                }
                if (n == 0)
                {
                    break;
                }

                parse_ptp_packet(buf, (size_t)n, rx_mono_ns);
            }
        }

        uint64_t now_ns = get_monotonic_ns();
        if (now_ns - last_frame_ns < frame_interval_ns)
        {
            uint64_t sleep_ns = frame_interval_ns - (now_ns - last_frame_ns);
            struct timespec ts_sleep{};
            ts_sleep.tv_sec = sleep_ns / 1000000000ULL;
            ts_sleep.tv_nsec = sleep_ns % 1000000000ULL;
            nanosleep(&ts_sleep, nullptr);
        }
        last_frame_ns = get_monotonic_ns();

        canvas->Clear();

        uint64_t tmp_sec;
        uint32_t tmp_nsec;
        bool have_time = get_current_utc(tmp_sec, tmp_nsec);
        rgb_matrix::Color color = have_time ? clock_color : status_color;

        if (!g_ptp.have_ptp_time)
        {
            const char *msg = "Waiting for PTP...";
            int text_width = int(std::strlen(msg)) * STATUS_CHAR_WIDTH;
            int x = (width - text_width) / 2;
            if (x < 0)
                x = 0;
            rgb_matrix::DrawText(canvas, status_font, x, 22, color, msg);
        }
        else if (!have_time)
        {
            const char *msg = "Lost PTP!";
            int text_width = int(std::strlen(msg)) * STATUS_CHAR_WIDTH;
            int x = (width - text_width) / 2;
            if (x < 0)
                x = 0;
            rgb_matrix::DrawText(canvas, status_font, x, 22, color, msg);
        }
        else
        {
            int hh, mm, ss;
            uint32_t nsec;
            if (get_current_local_time(hh, mm, ss, nsec))
            {
                const int top_seg_len = 7;
                const int top_seg_thick = 3;
                const int top_digit_w = calc_digit_width(top_seg_len, top_seg_thick); // 13
                const int top_digit_gap = 2;
                const int top_colon_w = 2;
                const int top_colon_gap_lr = 2;

                // 6 digits + 3 digit gaps + 2 colon sections
                const int top_width =
                    (6 * top_digit_w) +
                    (3 * top_digit_gap) +
                    (2 * (top_colon_gap_lr + top_colon_w + top_colon_gap_lr));

                const int top_x = (width - top_width) / 2;
                const int top_y = 0;

                draw_7seg_time(canvas, top_x, top_y, hh, mm, ss, true, color);

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
                const int bottom_width = nano_colon_w + nano_colon_gap_lr + nano_digits_width;

                int bottom_x = (width - bottom_width) / 2;
                if (bottom_x < 0)
                    bottom_x = 0;

                draw_7seg_colon(canvas, bottom_x, nano_y + 1, 1, 2, true, color);

                int nano_digits_x = bottom_x + nano_colon_w + nano_colon_gap_lr;
                draw_7seg_digits_row(canvas, nano_digits_x, nano_y, bottom_line,
                                     nano_seg_len, nano_seg_thick, nano_digit_gap, color);
            }
            else
            {
                const char *msg = "ERROR!!";
                int text_width = int(std::strlen(msg)) * STATUS_CHAR_WIDTH;
                int x = (width - text_width) / 2;
                if (x < 0)
                    x = 0;
                rgb_matrix::DrawText(canvas, status_font, x, 22, color, msg);
            }
        }

        canvas = matrix->SwapOnVSync(canvas);
    }

    matrix->Clear();
    delete matrix;
    close(sock_event);
    close(sock_general);

    logmsg("PTP LED Clock exiting.");
    return 0;
}