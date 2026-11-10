/** @file One-way bistatic radar: shared PPS, RF sync, finite chirp trains, repeated cycles. */
#include "radio.hpp"
#include "train.hpp"
#include "rx_timeline.hpp"
#include <csignal>
#include <iomanip>
#include <random>
#include <sstream>

namespace bistatic {
std::atomic<bool> interrupted{false};

void signalHandler(int) { interrupted = true; }

void saveYaml(const fs::path& filename, const YAML::Node& node) {
  YAML::Emitter emitter;
  emitter << node;
  auto temporary = filename.string() + ".tmp";
  std::ofstream destination(temporary);
  destination << emitter.c_str() << '\n';
  destination.close();
  if (!destination) throw std::runtime_error("Cannot save " + filename.string());
  fs::rename(temporary, filename);
}

/** Wait in device time while the independent PPS watchdog remains active. */
void waitUntil(Radio& radio, PpsClock& clock, double time) {
  while (radio.device->get_time_now().get_real_secs() < time) {
    clock.check();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

struct SyncPlan {
  Packet announcement;
  uint64_t target_edge;
  double carrier_hz;
  double correlation;
};

/** Broadcast redundant sync copies and proceed independently of receiver readiness. */
SyncPlan synchronizeTx(Radio& radio, PpsClock& clock, const Config& config,
                       uint64_t session, uint64_t cycle, uint64_t& attempt) {
  Edge anchor = clock.latest();
  double ratio = clock.ratio();
  if (radio.device->get_time_now().get_real_secs() > anchor.time + 0.10 * ratio)
    anchor = clock.waitEdge(anchor.sequence + 1);
  Packet announcement{session, cycle, ++attempt, config.fingerprint};
  std::cout << "[SYNC] Broadcasting cycle=" << cycle << " target PPS edge=" << anchor.sequence + kLeadEdges << std::endl;
  for (size_t repetition = 0; repetition < kSyncPhases.size(); ++repetition) {
    announcement.repetition = static_cast<uint8_t>(repetition);
    radio.sendControl(announcement, anchor.time + kSyncPhases[repetition] * ratio, clock);
  }
  return {announcement, anchor.sequence + kLeadEdges, 0, 0};
}

/** RX maps the preamble to its own preceding physical edge; integer epochs may differ. */
SyncPlan synchronizeRx(Radio& radio, PpsClock& clock, const Config& config,
                       std::optional<Packet>& previous) {
  radio.startReceive();
  while (!interrupted) {
    auto announcement = radio.receiveUntil(radio.device->get_time_now().get_real_secs() + 1, clock, [&](const Received& received) {
      const auto& packet = received.packet;
      if (packet.config != config.fingerprint) {
        std::cout << "[SYNC] Configuration/waveform mismatch; TX fingerprint=" << packet.config
                  << " RX fingerprint=" << config.fingerprint << "; ignoring sync" << std::endl;
        return false;
      }
      Edge anchor = clock.before(received.time);
      double phase = (received.time - anchor.time) / clock.ratio();
      if (std::abs(phase - kSyncPhases[packet.repetition]) > config.tolerance) {
        std::cout << "[SYNC] Rejected PPS phase: measured_ms=" << phase * 1000
                  << " expected_ms=" << kSyncPhases[packet.repetition] * 1000
                  << " tolerance_ms=" << config.tolerance * 1000
                  << "; verify both radios share aligned PPS edges" << std::endl;
        return false;
      }
      if (validAnnouncement(packet, config.fingerprint, phase, config.tolerance, previous)) return true;
      std::cout << "[SYNC] Rejected duplicate or stale packet: session=" << packet.session
                << " cycle=" << packet.cycle << " attempt=" << packet.attempt << std::endl;
      return false;
    });
    if (!announcement) continue;
    Edge anchor = clock.before(announcement->time);
    double ratio = clock.ratio();
    if (radio.device->get_time_now().get_real_secs() >= anchor.time + (kLeadEdges - 0.5) * ratio) {
      std::cout << "[SYNC] Announcement decoded too late to arm; waiting for next cycle" << std::endl;
      continue;
    }
    previous = announcement->packet;
    radio.stopReceive();
    std::cout << "[SYNC] Sync accepted; target PPS edge=" << anchor.sequence + kLeadEdges << std::endl;
    return {announcement->packet, anchor.sequence + kLeadEdges, announcement->carrier_hz, announcement->score};
  }
  throw std::runtime_error("Interrupted");
}

/** Stream one timed train instead of issuing 5,000 host commands per second. */
void transmitTrain(Radio& radio, PpsClock& clock, const Config& config, double first_time, double ratio) {
  TrainGenerator train(config.chirp, config.pulses, config.pri, config.rate, ratio, config.dither);
  uint64_t total = train.samples();
  std::vector<Sample> buffer(262144);
  size_t completed_bursts = 0;
  radio.drainTransmit();
  if (radio.device->get_time_now().get_real_secs() + 0.05 >= first_time) throw std::runtime_error("Too late to arm radar TX");
  for (uint64_t first = 0; first < total;) {
    clock.check();
    size_t count = size_t(std::min<uint64_t>(buffer.size(), total - first));
    train.fill(buffer, first, count);
    uhd::tx_metadata_t metadata;
    metadata.start_of_burst = first == 0;
    metadata.end_of_burst = first + count == total;
    metadata.has_time_spec = first == 0;
    metadata.time_spec = uhd::time_spec_t(first_time);
    if (radio.tx->send(buffer.data(), count, metadata, 2) != count) throw std::runtime_error("Short radar TX; cycle is incomplete");
    completed_bursts += radio.drainTransmit();
    first += count;
  }
  waitUntil(radio, clock, first_time + double(total) / config.rate + 0.05);
  auto deadline = Steady::now() + std::chrono::seconds(1);
  while (!completed_bursts && Steady::now() < deadline) {
    clock.check();
    completed_bursts += radio.drainTransmit();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (completed_bursts != 1 || train.generated() != config.pulses) throw std::runtime_error("Missing radar TX burst completion from UHD");
}

/** Extract only configured RX windows from a continuous timed stream, preserving file format. */
void receiveTrain(Radio& radio, PpsClock& clock, const Config& config, double first_time,
                  double ratio, const fs::path& filename, YAML::Node& metadata) {
  WindowExtractor windows(config.pulses, config.rx_samples, config.pri, config.rate, ratio, config.dither);
  PulseAverager averages(config.rx_samples, config.presums);
  RxTimeline timeline(first_time, config.rate, windows.samples());
  uint64_t total = timeline.requestedSamples();
  // Long trains exceed the B2xx 28-bit finite command count; stop them after total samples on the host.
  bool continuous = total > 0x0fffffffull;
  std::vector<Sample> buffer(262144);
  std::ofstream destination(filename, std::ios::binary);
  if (!destination) throw std::runtime_error("Cannot create RX IQ file");
  uhd::stream_cmd_t command(continuous ? uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS
                                     : uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);
  command.num_samps = total;
  command.stream_now = false;
  command.time_spec = uhd::time_spec_t(timeline.commandTime());
  metadata["rx_command_device_time"] = timeline.commandTime();
  metadata["rx_guard_samples"] = timeline.guardSamples();
  metadata["requested_stream_samples"] = total;
  std::cout << "[RADAR RX] rate_sps=" << config.rate << " wire_format="
            << config.yaml["DEVICE"]["otw_format"].as<std::string>()
            << " requested_samples=" << total << " max_packet_samples=" << radio.rx->get_max_num_samps()
            << " num_presums=" << config.presums << " capture_mode=" << (continuous ? "continuous" : "finite")
            << " first_time=" << std::to_string(first_time) << std::endl;
  if (radio.device->get_time_now().get_real_secs() + 0.05 >= timeline.commandTime()) throw std::runtime_error("Too late to arm radar RX");
  radio.rx->issue_stream_cmd(command);
  auto progress_deadline = Steady::now() + std::chrono::duration<double>(config.offset + 3);
  while (!timeline.complete()) {
    clock.check();
    uhd::rx_metadata_t packet;
    size_t count = radio.rx->recv(buffer.data(), size_t(std::min<uint64_t>(buffer.size(), timeline.remaining())), packet, 0.1, false);
    if (packet.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT && !count) {
      if (Steady::now() < progress_deadline) continue;
      throw std::runtime_error("Radar RX made no progress before timeout");
    }
    if (packet.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) throw std::runtime_error("Radar RX: " + packet.strerror());
    if (!count) continue;
    double expected = timeline.expectedTime();
    uint64_t received = timeline.receivedSamples();
    RxSlice slice;
    try {
      slice = timeline.accept(packet.has_time_spec, packet.time_spec.get_real_secs(), count);
    } catch (const std::runtime_error& error) {
      auto detail = metadata["timestamp_error"];
      detail["received_samples"] = received;
      detail["chunk_samples"] = count;
      detail["expected_time"] = expected;
      detail["has_time_spec"] = packet.has_time_spec;
      std::ostringstream message;
      message << std::setprecision(17) << error.what() << ": received_samples=" << received
              << " chunk_samples=" << count << " expected_time=" << expected;
      if (packet.has_time_spec) {
        double delta_samples = (packet.time_spec.get_real_secs() - expected) * config.rate;
        detail["actual_time"] = packet.time_spec.get_real_secs();
        detail["delta_samples"] = delta_samples;
        message << " actual_time=" << packet.time_spec.get_real_secs() << " delta_samples=" << delta_samples;
      } else message << " missing_timestamp=true";
      message << "; refusing a complete output file";
      throw std::runtime_error(message.str());
    }
    if (!received) {
      double offset = (timeline.firstStreamTime() - timeline.commandTime()) * config.rate;
      double alignment = (timeline.firstWindowTime() - first_time) * config.rate;
      metadata["first_stream_device_time"] = timeline.firstStreamTime();
      metadata["discarded_leading_samples"] = timeline.discardedSamples();
      metadata["actual_first_rx_device_time"] = timeline.firstWindowTime();
      metadata["rx_startup_offset_samples"] = offset;
      metadata["rx_alignment_error_samples"] = alignment;
      std::cout << "[RADAR RX] startup_offset_samples=" << offset
                << " discarded_leading_samples=" << timeline.discardedSamples()
                << " window_alignment_error_samples=" << alignment << std::endl;
    }
    progress_deadline = Steady::now() + std::chrono::seconds(2);
    windows.consume(buffer.data() + slice.offset, slice.count, [&](const std::vector<Sample>& record) {
      averages.consume(record, [&](const std::vector<Sample>& average) {
        destination.write(reinterpret_cast<const char*>(average.data()), std::streamsize(average.size() * sizeof(Sample)));
        if (!destination) throw std::runtime_error("RX IQ write failed");
      });
    });
    metadata["received_pulses"] = windows.extracted();
    metadata["saved_pulses"] = averages.saved();
  }
  // Finite commands also include a trailing guard; discard it before the next cycle.
  radio.stopReceive();
  destination.close();
  if (!destination || windows.extracted() != config.pulses || averages.buffered() ||
      averages.saved() != config.pulses / config.presums) throw std::runtime_error("Incomplete RX IQ output");
  // Completion requires another physical edge, detecting PPS loss near the end of a train.
  clock.waitEdge(clock.latest().sequence + 1);
  metadata["received_stream_samples"] = timeline.receivedSamples();
  metadata["radar_stream_samples"] = windows.samples();
}

/** Keep interrupted or failed recordings as .partial; successful files are atomically named. */
void runCycle(const std::string& role, Radio& radio, PpsClock& clock, const Config& config,
              const SyncPlan& plan, const fs::path& directory, uint64_t completed) {
  std::ostringstream name;
  name << "cycle_" << std::setfill('0') << std::setw(6) << completed + 1;
  auto prefix = directory / name.str();
  auto report_path = fs::path(prefix.string() + "_capture.yaml");
  auto partial = fs::path(prefix.string() + "_rx_samps.bin.partial");
  auto final = fs::path(prefix.string() + "_rx_samps.bin");
  if (fs::exists(report_path) || fs::exists(partial) || fs::exists(final)) throw std::runtime_error("Refusing to overwrite an existing cycle");
  YAML::Node report;
  report["complete"] = false;
  report["role"] = role;
  report["session"] = plan.announcement.session;
  report["cycle"] = plan.announcement.cycle;
  report["attempt"] = plan.announcement.attempt;
  report["sync_mode"] = "one_way_rf";
  report["protocol_version"] = 2;
  report["config_fingerprint"] = config.fingerprint;
  report["local_target_pps_edge"] = plan.target_edge;
  if (role == "rx") {
    report["sync_repetition"] = unsigned(plan.announcement.repetition);
    report["control_carrier_offset_hz"] = plan.carrier_hz;
    report["control_correlation"] = plan.correlation;
  }
  report["carrier_correction_applied"] = false;
  report["coherent_phase_lock"] = false;
  report["phase_dithering_removed"] = role == "rx" && config.dither;
  report["saved_pulses"] = 0;
  report["received_pulses"] = 0;
  report["num_presums"] = config.presums;
  report["expected_records"] = config.pulses / config.presums;
  saveYaml(report_path, report);
  saveYaml(prefix.string() + "_config.yaml", config.yaml);
  try {
    radio.radar();
    Edge edge = clock.waitEdge(plan.target_edge);
    double ratio = clock.ratio();
    double first_rx = edge.time + config.offset * ratio;
    double first_tx = edge.time + (config.offset - config.tx_lead) * ratio;
    report["pps_device_time"] = edge.time;
    report["clock_ratio"] = ratio;
    report["first_rx_device_time"] = first_rx;
    report["first_tx_device_time"] = first_tx;
    report["sample_rate"] = config.rate;
    report["samples_per_pulse"] = config.rx_samples;
    report["num_pulses"] = config.pulses;
    saveYaml(report_path, report);
    double host_time = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::ofstream log(prefix.string() + "_uhd_stdout.log");
    log << '[' << std::fixed << std::setprecision(6) << host_time << "]\t[START] Beginning main loop\n";
    std::cout << "[RADAR] " << role << " cycle=" << plan.announcement.cycle << " clock_error_ppm=" << (ratio - 1) * 1e6 << std::endl;
    if (role == "tx") {
      transmitTrain(radio, clock, config, first_tx, ratio);
      clock.waitEdge(clock.latest().sequence + 1);
    } else receiveTrain(radio, clock, config, first_rx, ratio, partial, report);
    double final_ratio = clock.ratio();
    report["final_clock_ratio"] = final_ratio;
    double duration = config.offset + double(config.pulses) * config.pri;
    if (std::abs(final_ratio - ratio) * duration > double(config.rx_samples) / config.rate / 4)
      throw std::runtime_error("Clock rate changed enough to compromise radar window alignment");
    if (role == "rx") {
      if (fs::file_size(partial) != (config.pulses / config.presums) * config.rx_samples * sizeof(Sample)) throw std::runtime_error("Wrong RX IQ file size");
      fs::rename(partial, final);
    }
    report["complete"] = true;
    saveYaml(report_path, report);
    std::cout << "[COMPLETE] " << prefix.string() << std::endl;
  } catch (const std::exception& error) {
    report["error"] = error.what();
    saveYaml(report_path, report);
    throw;
  }
}
}  // namespace bistatic

/** Two independently built role executables share the same validated protocol implementation. */
int main(int argc, char* argv[]) {
  using namespace bistatic;
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);
  try {
    if (argc < 4) throw std::invalid_argument("Usage: bistatic_rx|bistatic_tx CONFIG ROOT OUTPUT [--loops N] [--device-args ARGS] [--validate]");
    std::string role = BISTATIC_ROLE;
    uint64_t limit = 0;
    std::string device_args;
    bool validate = false;
    for (int i = 4; i < argc; ++i) {
      std::string option = argv[i];
      if (option == "--validate") validate = true;
      else if (option == "--loops" && i + 1 < argc) {
        std::string count = argv[++i];
        if (count.empty() || count[0] == '-' || count.find_first_not_of("0123456789") != std::string::npos)
          throw std::invalid_argument("--loops must be a nonnegative integer (0 means forever)");
        limit = std::stoull(count);
      } else if (option == "--device-args" && i + 1 < argc) device_args = argv[++i];
      else throw std::invalid_argument("Unknown or incomplete option: " + option);
    }
    Config config(fs::absolute(argv[1]), fs::absolute(argv[2]), role);
    std::cout << "[CONFIG] " << role << " fingerprint=" << config.fingerprint << " pulses/cycle=" << config.pulses
              << " num_presums=" << config.presums << " records/cycle=" << config.pulses / config.presums << std::endl;
    if (role == "rx" && config.presums > 1)
      std::cout << "[PRESUM] Averaging complex IQ without carrier-phase correction; RF phase drift can cancel the return" << std::endl;
    if (validate) return 0;
    fs::path output = fs::absolute(argv[3]);
    if (!fs::is_directory(output)) throw std::invalid_argument("Runner must create a new output directory before launch");
    Radio radio(config, device_args, role);
    PpsClock clock(radio.device, config.max_ppm);
    std::cout << "[PPS] Waiting for three physical 1 Hz edges" << std::endl;
    clock.waitEdge(3, 6);
    std::cout << "[PPS] Acquired; clock_error_ppm=" << (clock.ratio() - 1) * 1e6 << std::endl;
    std::random_device random;
    uint64_t session = (uint64_t(random()) << 32) ^ random();
    if (!session) session = 1;
    uint64_t completed = 0, attempt = 0;
    std::optional<Packet> previous;
    while (!interrupted && (!limit || completed < limit)) {
      SyncPlan plan = role == "tx" ? synchronizeTx(radio, clock, config, session, completed + 1, attempt)
                                   : synchronizeRx(radio, clock, config, previous);
      runCycle(role, radio, clock, config, plan, output, completed);
      ++completed;
      if (!limit || completed < limit) radio.control();
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "[ERROR] " << error.what() << std::endl;
    return bistatic::interrupted ? 130 : 1;
  }
}
