/** @file Standalone serial GPS logger derived from sdr/gps_test.cpp. */
#include <boost/asio.hpp>
#include <poll.h>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

static volatile std::sig_atomic_t stopped = 0;

/** Stop polling when the runner terminates this process. */
void stopGps(int) { stopped = 1; }

/** Convert a nonempty NMEA latitude or longitude to signed degrees. */
double toDecimal(const std::string& value, const std::string& direction) {
  const double raw = std::stod(value);
  const int degrees = static_cast<int>(raw / 100);
  const double result = degrees + (raw - degrees * 100) / 60;
  return direction == "S" || direction == "W" ? -result : result;
}

/** Validate a complete GGA sentence and write the original timestamp/lat/lon/alt layout. */
void logPosition(const std::string& line, std::ofstream& output) {
  if (line.rfind("$GNGGA,", 0) != 0 && line.rfind("$GPGGA,", 0) != 0) return;
  const auto star = line.find('*');
  if (star == std::string::npos || star + 3 != line.size()) return;
  unsigned checksum = 0;
  for (size_t i = 1; i < star; ++i) checksum ^= static_cast<unsigned char>(line[i]);
  try {
    if (checksum != std::stoul(line.substr(star + 1), nullptr, 16)) return;
    std::stringstream parser(line.substr(0, star));
    std::vector<std::string> fields;
    std::string field;
    while (std::getline(parser, field, ',')) fields.push_back(field);
    if (fields.size() < 11 || fields[6].empty() || std::stoi(fields[6]) == 0 ||
        fields[2].empty() || fields[4].empty() || fields[9].empty() ||
        (fields[3] != "N" && fields[3] != "S") ||
        (fields[5] != "E" && fields[5] != "W")) return;
    const double latitude = toDecimal(fields[2], fields[3]);
    const double longitude = toDecimal(fields[4], fields[5]);
    const double altitude = std::stod(fields[9]);
    if (!std::isfinite(latitude) || !std::isfinite(longitude) || !std::isfinite(altitude) ||
        std::abs(latitude) > 90 || std::abs(longitude) > 180) return;
    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    output << now_us << ',' << std::fixed << std::setprecision(9)
           << latitude << ',' << longitude << ',' << altitude << std::endl;
  } catch (const std::invalid_argument&) {
    // Ignore malformed/no-fix sentences rather than stopping the transmitter.
  } catch (const std::out_of_range&) {
  }
}

/** Read serial GPS independently of the TX process, with bounded signal response. */
int main(int argc, char* argv[]) {
  if (argc != 3) {
    std::cerr << "Usage: gps_logger SERIAL_DEVICE OUTPUT_FILE\n";
    return 1;
  }
  std::signal(SIGINT, stopGps);
  std::signal(SIGTERM, stopGps);
  try {
    boost::asio::io_service io;
    boost::asio::serial_port serial(io, argv[1]);
    using namespace boost::asio;
    serial.set_option(serial_port_base::baud_rate(115200));
    serial.set_option(serial_port_base::character_size(8));
    serial.set_option(serial_port_base::parity(serial_port_base::parity::none));
    serial.set_option(serial_port_base::stop_bits(serial_port_base::stop_bits::one));
    serial.set_option(serial_port_base::flow_control(serial_port_base::flow_control::none));
    std::ofstream output(argv[2]);
    if (!output) throw std::runtime_error("Cannot open GPS log file.");
    std::cout << "[GPS] Reading " << argv[1] << std::endl;
    std::string line;
    while (!stopped) {
      pollfd descriptor{serial.native_handle(), POLLIN, 0};
      const int result = ::poll(&descriptor, 1, 100);
      if (result < 0) {
        if (errno == EINTR) continue;
        throw std::runtime_error("GPS poll failed.");
      }
      if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) throw std::runtime_error("GPS disconnected.");
      if (result == 0 || !(descriptor.revents & POLLIN)) continue;
      char data[512];
      boost::system::error_code error;
      const auto count = serial.read_some(buffer(data), error);
      if (error) throw boost::system::system_error(error);
      for (size_t i = 0; i < count; ++i) {
        if (data[i] == '\n') {
          logPosition(line, output);
          if (!output) throw std::runtime_error("GPS log write failed.");
          line.clear();
        } else if (data[i] != '\r') {
          line += data[i];
          if (line.size() > 1024) line.clear();
        }
      }
    }
  } catch (const std::exception& error) {
    std::cerr << "[GPS] " << error.what() << '\n';
    return 1;
  }
  return 0;
}
