#ifndef PTPI_CLOCK_PTP_H
#define PTPI_CLOCK_PTP_H

#include <cstdint>
#include <functional>
#include <string>

namespace ptpi
{

struct PtpSnapshot
{
  bool have_ptp_time = false;
  bool time_valid = false;
  bool utc_offset_valid = false;
  int16_t utc_offset = 37;
  uint64_t utc_sec = 0;
  uint32_t utc_nsec = 0;
  uint64_t last_packet_mono_ns = 0;
};

bool GetIpv4ForInterface(const std::string &iface, std::string &out_ip);
uint64_t GetMonotonicNs();

class PtpClockReceiver
{
public:
  struct Config
  {
    std::string iface_ipv4;
    uint64_t timeout_ns = 1500000000ULL;
    int16_t default_utc_offset = 37;
  };

  using Logger = std::function<void(const std::string &)>;

  explicit PtpClockReceiver(Config config, Logger logger = {});
  ~PtpClockReceiver();

  bool Open();
  void Close();
  bool Poll();

  bool GetCurrentUtc(uint64_t &utc_sec, uint32_t &utc_nsec) const;
  bool GetCurrentLocalTime(int timezone_offset_seconds,
                           int &hh,
                           int &mm,
                           int &ss,
                           uint32_t &nsec) const;

  PtpSnapshot GetSnapshot() const;

private:
  struct Impl;
  Impl *impl_;
};

} // namespace ptpi

#endif
