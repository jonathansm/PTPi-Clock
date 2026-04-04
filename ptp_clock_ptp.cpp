#include "ptp_clock_ptp.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <ifaddrs.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <unordered_map>

namespace ptpi
{

namespace
{

constexpr const char *kPtpMcastAddr = "224.0.1.129";
constexpr uint16_t kPtpEventPort = 319;
constexpr uint16_t kPtpGeneralPort = 320;
constexpr int kSocketTimestampOpt = SO_TIMESTAMPNS;
constexpr int kSocketTimestampCmsg = SCM_TIMESTAMPNS;
constexpr uint32_t kNsecPerSec = 1000000000U;
constexpr size_t kPtpHeaderLen = 34;
constexpr size_t kTimestampFieldOffset = 34;
constexpr size_t kTimestampFieldLen = 10;
constexpr size_t kClockIdentityOffset = 20;
constexpr size_t kPortNumberOffset = 28;
constexpr size_t kSequenceIdOffset = 30;
constexpr size_t kCorrectionFieldOffset = 8;

enum PtpMessageType : uint8_t
{
  PTP_SYNC = 0x0,
  PTP_FOLLOW_UP = 0x8,
  PTP_ANNOUNCE = 0xB
};

inline uint64_t TimespecToNs(const struct timespec &ts)
{
  return uint64_t(ts.tv_sec) * uint64_t(kNsecPerSec) + uint64_t(ts.tv_nsec);
}

inline uint64_t GetRealtimeNs()
{
  struct timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  return TimespecToNs(ts);
}

inline uint64_t RealtimeNsToMonotonicNs(uint64_t realtime_ns)
{
  const uint64_t mono_now = GetMonotonicNs();
  const uint64_t real_now = GetRealtimeNs();

  const int64_t offset = int64_t(mono_now) - int64_t(real_now);
  const int64_t mono_est = int64_t(realtime_ns) + offset;
  return mono_est < 0 ? 0 : uint64_t(mono_est);
}

inline uint16_t ReadBe16(const uint8_t *buf, size_t offset)
{
  return uint16_t(buf[offset] << 8) | uint16_t(buf[offset + 1]);
}

inline uint32_t ReadBe32(const uint8_t *buf, size_t offset)
{
  return (uint32_t(buf[offset]) << 24) |
         (uint32_t(buf[offset + 1]) << 16) |
         (uint32_t(buf[offset + 2]) << 8) |
         uint32_t(buf[offset + 3]);
}

inline uint64_t ReadBe48(const uint8_t *buf, size_t offset)
{
  return (uint64_t(buf[offset]) << 40) |
         (uint64_t(buf[offset + 1]) << 32) |
         (uint64_t(buf[offset + 2]) << 24) |
         (uint64_t(buf[offset + 3]) << 16) |
         (uint64_t(buf[offset + 4]) << 8) |
         uint64_t(buf[offset + 5]);
}

inline int64_t ReadBe64Signed(const uint8_t *buf, size_t offset)
{
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i)
  {
    value = (value << 8) | uint64_t(buf[offset + i]);
  }
  return static_cast<int64_t>(value);
}

inline int64_t CorrectionFieldToNs(const uint8_t *buf, size_t offset)
{
  const int64_t scaled = ReadBe64Signed(buf, offset);
  if (scaled >= 0)
  {
    return (scaled + (1LL << 15)) >> 16;
  }
  return -(((-scaled) + (1LL << 15)) >> 16);
}

struct NormalizedTimestamp
{
  uint64_t sec = 0;
  uint32_t nsec = 0;
};

inline bool NormalizeTimestamp(uint64_t sec, uint32_t nsec, int64_t correction_ns, NormalizedTimestamp &out)
{
  int64_t corrected_nsec = int64_t(nsec) + correction_ns;
  int64_t corrected_sec = int64_t(sec);

  while (corrected_nsec >= int64_t(kNsecPerSec))
  {
    corrected_nsec -= int64_t(kNsecPerSec);
    ++corrected_sec;
  }
  while (corrected_nsec < 0)
  {
    corrected_nsec += int64_t(kNsecPerSec);
    --corrected_sec;
  }

  if (corrected_sec < 0)
  {
    return false;
  }

  out.sec = uint64_t(corrected_sec);
  out.nsec = uint32_t(corrected_nsec);
  return true;
}

struct PortIdentityKey
{
  std::array<uint8_t, 8> clock_identity{};
  uint16_t port_number = 0;
  uint16_t sequence_id = 0;
  uint8_t domain = 0;

  bool operator==(const PortIdentityKey &other) const
  {
    return clock_identity == other.clock_identity &&
           port_number == other.port_number &&
           sequence_id == other.sequence_id &&
           domain == other.domain;
  }
};

struct PortIdentityKeyHash
{
  size_t operator()(const PortIdentityKey &key) const
  {
    size_t hash = key.sequence_id;
    hash = (hash * 131) ^ key.port_number;
    hash = (hash * 131) ^ key.domain;
    for (uint8_t byte : key.clock_identity)
    {
      hash = (hash * 131) ^ byte;
    }
    return hash;
  }
};

struct PendingSync
{
  uint64_t rx_mono_ns = 0;
  uint64_t received_mono_ns = 0;
};

PortIdentityKey MakeSequenceKey(const uint8_t *buf)
{
  PortIdentityKey key{};
  std::memcpy(key.clock_identity.data(), buf + kClockIdentityOffset, key.clock_identity.size());
  key.port_number = ReadBe16(buf, kPortNumberOffset);
  key.sequence_id = ReadBe16(buf, kSequenceIdOffset);
  key.domain = buf[4];
  return key;
}

bool ValidatePtpMessage(const uint8_t *buf, size_t len)
{
  if (len < kPtpHeaderLen)
  {
    return false;
  }

  const uint8_t version_ptp = buf[1] & 0x0F;
  if (version_ptp != 2)
  {
    return false;
  }

  const uint16_t message_length = ReadBe16(buf, 2);
  return message_length >= kPtpHeaderLen && message_length <= len;
}

} // namespace

bool GetIpv4ForInterface(const std::string &iface, std::string &out_ip)
{
  struct ifaddrs *ifaddr = nullptr;
  if (getifaddrs(&ifaddr) == -1)
  {
    return false;
  }

  bool found = false;
  for (struct ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next)
  {
    if (ifa->ifa_addr == nullptr)
    {
      continue;
    }
    if (iface != ifa->ifa_name)
    {
      continue;
    }
    if (ifa->ifa_addr->sa_family != AF_INET)
    {
      continue;
    }

    char buf[INET_ADDRSTRLEN];
    void *addr = &reinterpret_cast<struct sockaddr_in *>(ifa->ifa_addr)->sin_addr;
    if (inet_ntop(AF_INET, addr, buf, sizeof(buf)) != nullptr)
    {
      out_ip = buf;
      found = true;
      break;
    }
  }

  freeifaddrs(ifaddr);
  return found;
}

uint64_t GetMonotonicNs()
{
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return TimespecToNs(ts);
}

struct PtpClockReceiver::Impl
{
  explicit Impl(Config cfg, Logger fn)
      : config(std::move(cfg)), logger(std::move(fn))
  {
    snapshot.utc_offset = config.default_utc_offset;
  }

  Config config;
  Logger logger;
  PtpSnapshot snapshot;
  int sock_event = -1;
  int sock_general = -1;
  std::unordered_map<PortIdentityKey, PendingSync, PortIdentityKeyHash> pending_syncs;

  void Log(const std::string &msg) const
  {
    if (logger)
    {
      logger(msg);
    }
  }

  void LogErrno(const std::string &prefix) const
  {
    if (logger)
    {
      logger(prefix + ": " + std::strerror(errno));
    }
  }

  void UpdateUtcFromPtp(uint64_t utc_sec, uint32_t utc_nsec, uint64_t rx_mono_ns)
  {
    const bool first_lock = !snapshot.have_ptp_time;

    snapshot.utc_sec = utc_sec;
    snapshot.utc_nsec = utc_nsec;
    snapshot.last_packet_mono_ns = GetMonotonicNs();
    snapshot.have_ptp_time = true;
    last_utc_sec = utc_sec;
    last_utc_nsec = utc_nsec;
    last_mono_ns = rx_mono_ns;

    if (first_lock)
    {
      Log("[PTP] First valid UTC time lock");
    }
  }

  void PrunePendingSyncs(uint64_t now_ns)
  {
    constexpr uint64_t kPendingSyncTtlNs = 3000000000ULL;
    for (auto it = pending_syncs.begin(); it != pending_syncs.end();)
    {
      if ((now_ns - it->second.received_mono_ns) > kPendingSyncTtlNs)
      {
        it = pending_syncs.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }

  bool DecodeUtcTimestamp(const uint8_t *buf,
                          size_t len,
                          uint64_t timestamp_offset,
                          int64_t extra_correction_ns,
                          uint64_t &utc_sec,
                          uint32_t &utc_nsec) const
  {
    if (len < (timestamp_offset + kTimestampFieldLen))
    {
      return false;
    }

    const uint64_t tai_sec = ReadBe48(buf, timestamp_offset);
    const uint32_t nsec = ReadBe32(buf, timestamp_offset + 6);
    if (nsec >= kNsecPerSec)
    {
      return false;
    }

    NormalizedTimestamp corrected{};
    const int64_t correction_ns = CorrectionFieldToNs(buf, kCorrectionFieldOffset) + extra_correction_ns;
    if (!NormalizeTimestamp(tai_sec, nsec, correction_ns, corrected))
    {
      return false;
    }

    const int16_t offset = snapshot.utc_offset_valid ? snapshot.utc_offset : config.default_utc_offset;
    utc_sec = (corrected.sec >= uint64_t(offset)) ? (corrected.sec - uint64_t(offset)) : corrected.sec;
    utc_nsec = corrected.nsec;
    return true;
  }

  void HandleAnnounce(const uint8_t *buf, size_t len)
  {
    if (len < 46)
    {
      return;
    }

    snapshot.utc_offset = static_cast<int16_t>(ReadBe16(buf, 44));
    snapshot.utc_offset_valid = true;
    Log("[PTP] Announce: currentUtcOffset = " + std::to_string(snapshot.utc_offset));
  }

  void HandleSync(const uint8_t *buf, size_t len, uint64_t rx_mono_ns)
  {
    if (len < 44)
    {
      return;
    }

    const uint16_t flag_field = ReadBe16(buf, 6);
    const bool two_step_flag = (flag_field & (1U << 9)) != 0;

    if (two_step_flag)
    {
      pending_syncs[MakeSequenceKey(buf)] = PendingSync{rx_mono_ns, GetMonotonicNs()};
      return;
    }

    uint64_t utc_sec = 0;
    uint32_t utc_nsec = 0;
    if (!DecodeUtcTimestamp(buf, len, kTimestampFieldOffset, 0, utc_sec, utc_nsec))
    {
      return;
    }

    Log("[PTP] Sync (1-step): UTC=" + std::to_string(utc_sec) +
        " ns=" + std::to_string(utc_nsec) +
        " rx_mono_ns=" + std::to_string(rx_mono_ns));
    UpdateUtcFromPtp(utc_sec, utc_nsec, rx_mono_ns);
  }

  void HandleFollowUp(const uint8_t *buf, size_t len)
  {
    if (len < 44)
    {
      return;
    }

    const PortIdentityKey key = MakeSequenceKey(buf);
    const auto pending = pending_syncs.find(key);
    if (pending == pending_syncs.end())
    {
      Log("[PTP] Follow_Up ignored: missing matching Sync");
      return;
    }

    uint64_t utc_sec = 0;
    uint32_t utc_nsec = 0;
    if (!DecodeUtcTimestamp(buf, len, kTimestampFieldOffset, 0, utc_sec, utc_nsec))
    {
      pending_syncs.erase(pending);
      return;
    }

    const uint64_t rx_mono_ns = pending->second.rx_mono_ns;
    pending_syncs.erase(pending);

    Log("[PTP] Follow_Up (2-step): UTC=" + std::to_string(utc_sec) +
        " ns=" + std::to_string(utc_nsec) +
        " rx_mono_ns=" + std::to_string(rx_mono_ns));
    UpdateUtcFromPtp(utc_sec, utc_nsec, rx_mono_ns);
  }

  void ParsePtpPacket(const uint8_t *buf, size_t len, uint64_t rx_mono_ns)
  {
    if (!ValidatePtpMessage(buf, len))
    {
      return;
    }

    switch (buf[0] & 0x0F)
    {
    case PTP_ANNOUNCE:
      HandleAnnounce(buf, len);
      break;
    case PTP_SYNC:
      HandleSync(buf, len, rx_mono_ns);
      break;
    case PTP_FOLLOW_UP:
      HandleFollowUp(buf, len);
      break;
    default:
      break;
    }
  }

  int MakePtpSocket(uint16_t port)
  {
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0)
    {
      LogErrno("socket");
      return -1;
    }

    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
    {
      LogErrno("setsockopt(SO_REUSEADDR)");
      close(sock);
      return -1;
    }

    int enable_ts = 1;
    if (setsockopt(sock, SOL_SOCKET, kSocketTimestampOpt, &enable_ts, sizeof(enable_ts)) < 0)
    {
      LogErrno("setsockopt(timestamp)");
      close(sock);
      return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0)
    {
      LogErrno("bind");
      close(sock);
      return -1;
    }

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(kPtpMcastAddr);
    mreq.imr_interface.s_addr = inet_addr(config.iface_ipv4.c_str());

    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
    {
      LogErrno("setsockopt(IP_ADD_MEMBERSHIP)");
      close(sock);
      return -1;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0)
    {
      flags = 0;
    }
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0)
    {
      LogErrno("fcntl(O_NONBLOCK)");
      close(sock);
      return -1;
    }

    return sock;
  }

  ssize_t RecvPtpPacketWithTimestamp(int sock, uint8_t *buf, size_t buf_size, uint64_t &rx_mono_ns)
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

    const ssize_t n = recvmsg(sock, &msg, 0);
    if (n < 0)
    {
      if (errno == EWOULDBLOCK || errno == EAGAIN)
      {
        return 0;
      }
      return -1;
    }

    bool found_ts = false;
    struct timespec ts_rx{};
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg))
    {
      if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == kSocketTimestampCmsg)
      {
        std::memcpy(&ts_rx, CMSG_DATA(cmsg), sizeof(ts_rx));
        found_ts = true;
        break;
      }
    }

    rx_mono_ns = found_ts ? RealtimeNsToMonotonicNs(TimespecToNs(ts_rx)) : GetMonotonicNs();
    return n;
  }

  bool OpenSockets()
  {
    CloseSockets();
    sock_event = MakePtpSocket(kPtpEventPort);
    sock_general = MakePtpSocket(kPtpGeneralPort);
    if (sock_event < 0 || sock_general < 0)
    {
      CloseSockets();
      return false;
    }

    Log(std::string("[PTP] Listening on ") + kPtpMcastAddr +
        ":" + std::to_string(kPtpEventPort) +
        " and :" + std::to_string(kPtpGeneralPort) +
        " via " + config.iface_ipv4);
    return true;
  }

  void CloseSockets()
  {
    if (sock_event >= 0)
    {
      close(sock_event);
      sock_event = -1;
    }
    if (sock_general >= 0)
    {
      close(sock_general);
      sock_general = -1;
    }
  }

  bool PollSockets()
  {
    if (sock_event < 0 || sock_general < 0)
    {
      return false;
    }

    uint8_t buf[512];
    for (int sock : {sock_event, sock_general})
    {
      while (true)
      {
        uint64_t rx_mono_ns = 0;
        const ssize_t n = RecvPtpPacketWithTimestamp(sock, buf, sizeof(buf), rx_mono_ns);
        if (n < 0)
        {
          LogErrno("recvmsg");
          return false;
        }
        if (n == 0)
        {
          break;
        }

        ParsePtpPacket(buf, size_t(n), rx_mono_ns);
      }
    }

    PrunePendingSyncs(GetMonotonicNs());
    return true;
  }

  bool GetCurrentUtcInternal(uint64_t &utc_sec, uint32_t &utc_nsec) const
  {
    if (!snapshot.have_ptp_time)
    {
      return false;
    }

    const uint64_t now_mono = GetMonotonicNs();
    if ((now_mono - snapshot.last_packet_mono_ns) > config.timeout_ns)
    {
      return false;
    }

    const uint64_t delta_ns = now_mono - last_mono_ns;
    const uint64_t base_ns = uint64_t(last_utc_nsec) + delta_ns;
    utc_sec = last_utc_sec + (base_ns / uint64_t(kNsecPerSec));
    utc_nsec = uint32_t(base_ns % uint64_t(kNsecPerSec));
    return true;
  }

  uint64_t last_utc_sec = 0;
  uint32_t last_utc_nsec = 0;
  uint64_t last_mono_ns = 0;
};

PtpClockReceiver::PtpClockReceiver(Config config, Logger logger)
    : impl_(new Impl(std::move(config), std::move(logger)))
{
}

PtpClockReceiver::~PtpClockReceiver()
{
  Close();
  delete impl_;
}

bool PtpClockReceiver::Open()
{
  return impl_->OpenSockets();
}

void PtpClockReceiver::Close()
{
  impl_->CloseSockets();
}

bool PtpClockReceiver::Poll()
{
  return impl_->PollSockets();
}

bool PtpClockReceiver::GetCurrentUtc(uint64_t &utc_sec, uint32_t &utc_nsec) const
{
  return impl_->GetCurrentUtcInternal(utc_sec, utc_nsec);
}

bool PtpClockReceiver::GetCurrentLocalTime(int timezone_offset_seconds,
                                           int &hh,
                                           int &mm,
                                           int &ss,
                                           uint32_t &nsec) const
{
  uint64_t utc_sec = 0;
  uint32_t utc_nsec = 0;
  if (!impl_->GetCurrentUtcInternal(utc_sec, utc_nsec))
  {
    return false;
  }

  int64_t local_sec = int64_t(utc_sec) + int64_t(timezone_offset_seconds);
  int64_t sec_of_day = local_sec % 86400;
  if (sec_of_day < 0)
  {
    sec_of_day += 86400;
  }

  hh = int(sec_of_day / 3600);
  mm = int((sec_of_day % 3600) / 60);
  ss = int(sec_of_day % 60);
  nsec = utc_nsec;
  return true;
}

PtpSnapshot PtpClockReceiver::GetSnapshot() const
{
  PtpSnapshot snapshot = impl_->snapshot;
  snapshot.time_valid = impl_->GetCurrentUtcInternal(snapshot.utc_sec, snapshot.utc_nsec);
  return snapshot;
}

} // namespace ptpi
