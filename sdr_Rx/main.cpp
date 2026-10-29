/** @file RX-only continuous capture for the framed shared-PPS bench PoC. */
#include "poc_protocol.hpp"
#include "shared_pps.hpp"
#include <uhd/utils/safe_main.hpp>
#include <uhd/utils/thread.hpp>
#include <uhd/types/tune_request.hpp>
#include <csignal>
#include <iomanip>
#include <iostream>

static volatile std::sig_atomic_t stopped = 0;
static void stopRx(int) { stopped = 1; }

/** Configure only RX RF, without creating a TX stream or applying TX gain. */
static uhd::usrp::multi_usrp::sptr setupReceiver(const YAML::Node& config, const poc::Plan& plan,
                                               size_t& channel) {
  const auto dev = config["DEVICE"];
  const auto rf = config["RF0"];
  const auto channels = dev["rx_channels"].as<std::string>("0");
  size_t parsed = 0;
  channel = std::stoul(channels, &parsed);
  if (parsed != channels.size() || channels.find('-') != std::string::npos)
    throw std::invalid_argument("PoC requires a single RX channel.");
  auto radio = uhd::usrp::multi_usrp::make(dev["device_args"].as<std::string>("type=b200"));
  radio->set_clock_source("internal");
  radio->set_time_source("external");
  radio->set_rx_subdev_spec(dev["subdev"].as<std::string>("A:A"));
  radio->set_master_clock_rate(dev["clk_rate"].as<double>());
  if (channel >= radio->get_rx_num_channels()) throw std::invalid_argument("Invalid RX channel.");
  radio->set_rx_rate(plan.rate, channel);
  uhd::tune_request_t tuning(plan.frequency, rf["lo_offset"].as<double>(0));
  tuning.args = uhd::device_addr_t(rf["tuning_args"].as<std::string>(""));
  radio->set_rx_freq(tuning, channel);
  radio->set_rx_gain(rf["rx_gain"].as<double>(), channel);
  const double bandwidth = rf["bw"].as<double>(0);
  if (bandwidth > 0) radio->set_rx_bandwidth(bandwidth, channel);
  radio->set_rx_antenna(rf["rx_ant"].as<std::string>("RX2"), channel);
  if (std::abs(radio->get_rx_rate(channel) - plan.rate) > plan.rate * 1e-6)
    throw std::runtime_error("Requested RX sample rate could not be provided.");
  const auto sensors = radio->get_rx_sensor_names(channel);
  if (std::find(sensors.begin(), sensors.end(), "lo_locked") != sensors.end()) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!radio->get_rx_sensor("lo_locked", channel).to_bool()) {
      if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("RX LO lock timeout.");
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  return radio;
}

/** Write continuous IQ and sample timestamps; a gap invalidates the capture instead of hiding lost pulses. */
static void capture(uhd::usrp::multi_usrp::sptr radio, uhd::rx_streamer::sptr stream,
                     const poc::Plan& plan, const std::filesystem::path& output_dir) {
  std::ofstream iq(output_dir / "rx_iq.bin", std::ios::binary);
  std::ofstream timestamps(output_dir / "rx_timestamps.csv");
  if (!iq || !timestamps) throw std::runtime_error("Cannot create capture output files.");
  timestamps << "sample_index,device_time_s,sample_count\n" << std::setprecision(17);
  YAML::Node metadata;
  metadata["timebase"] = "first_shared_pps_zero";
  metadata["sample_rate"] = plan.rate;
  metadata["requested_start_time"] = plan.captureStart();
  metadata["requested_samples"] = plan.captureSamples();
  metadata["complete"] = false;
  metadata["samples"] = uint64_t(0);
  metadata["error"] = "capture has not completed";
  auto saveMetadata = [&]() {
    std::ofstream output(output_dir / "capture.yaml");
    output << metadata << '\n';
    if (!output) throw std::runtime_error("Capture metadata write failed.");
  };
  saveMetadata();
  uhd::stream_cmd_t start(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
  start.stream_now = false;
  start.time_spec = uhd::time_spec_t(plan.captureStart());
  // Keep the first blocking receive short enough for graceful Ctrl-C shutdown.
  while (!stopped && radio->get_time_now().get_real_secs() < plan.captureStart() - 0.5)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (stopped) throw std::runtime_error("Stopped before RX streaming.");
  if (radio->get_time_now().get_real_secs() >= plan.captureStart() - 0.1)
    throw std::runtime_error("Missed RX scheduling deadline.");
  stream->issue_stream_cmd(start);
  std::vector<poc::Sample> buffer(65536);
  uint64_t samples = 0;
  double first_time = 0;
  auto stopStream = [&]() {
    uhd::stream_cmd_t stop(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
    stop.stream_now = true;
    stream->issue_stream_cmd(stop);
  };
  try {
    while (!stopped && samples < plan.captureSamples()) {
      const size_t wanted = std::min<uint64_t>(buffer.size(), plan.captureSamples() - samples);
      uhd::rx_metadata_t md;
      const double timeout = samples == 0 ? std::max(1.0, plan.captureStart() - radio->get_time_now().get_real_secs() + 1.0) : 1.0;
      const size_t count = stream->recv(buffer.data(), wanted, md, timeout, false);
      if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE)
        throw std::runtime_error("RX error: " + md.strerror());
      if (!count || !md.has_time_spec) throw std::runtime_error("RX returned no timestamped samples.");
      const double time = md.time_spec.get_real_secs();
      if (samples == 0) {
        first_time = time;
        metadata["first_device_time"] = first_time;
        if (std::abs(time - plan.captureStart()) * plan.rate > 2)
          throw std::runtime_error("First RX sample did not match the scheduled start.");
      } else if (std::abs(time - (first_time + double(samples) / plan.rate)) * plan.rate > 2) {
        throw std::runtime_error("RX timestamp discontinuity: capture contains missing samples.");
      }
      iq.write(reinterpret_cast<const char*>(buffer.data()), count * sizeof(poc::Sample));
      timestamps << samples << ',' << time << ',' << count << '\n';
      if (!iq || !timestamps) throw std::runtime_error("RX output write failed.");
      samples += count;
    }
    stopStream();
    if (stopped) throw std::runtime_error("Capture interrupted; partial IQ retained.");
    iq.flush(); timestamps.flush();
    if (!iq || !timestamps) throw std::runtime_error("RX output flush failed.");
    metadata["complete"] = true;
    metadata["error"] = "";
    metadata["samples"] = samples;
    saveMetadata();
  } catch (const std::exception& error) {
    stopStream();
    metadata["samples"] = samples;
    metadata["error"] = error.what();
    saveMetadata();
    throw;
  }
  std::cout << "[RX] Saved " << samples << " complex IQ samples to " << output_dir << std::endl;
}

/** Validate without hardware, or open one receiver and arm it for the shared PPS edge. */
int UHD_SAFE_MAIN(int argc, char* argv[]) {
  if (argc < 4) throw std::invalid_argument("Usage: radar_rx CONFIG ROOT OUTPUT_DIR [--validate] [--device-args ARGS]");
  auto config = YAML::LoadFile(argv[1]);
  bool validate = false;
  for (int i = 4; i < argc; ++i) {
    const std::string option = argv[i];
    if (option == "--validate") validate = true;
    else if (option == "--device-args" && i + 1 < argc) config["DEVICE"]["device_args"] = argv[++i];
    else throw std::invalid_argument("Unknown/missing RX option: " + option);
  }
  const poc::Plan plan(config);
  if (validate) {
    std::cout << "[RX] PoC validated: " << plan.captureSamples() << " planned samples." << std::endl;
    return EXIT_SUCCESS;
  }
  const std::filesystem::path output_dir(argv[3]);
  std::filesystem::create_directories(output_dir);
  if (std::filesystem::exists(output_dir / "rx_iq.bin")) throw std::runtime_error("Capture already exists; choose a new output directory.");
  std::ofstream snapshot(output_dir / "config.yaml");
  snapshot << config << '\n'; snapshot.close();
  if (!snapshot) throw std::runtime_error("Cannot write the capture config snapshot.");
  std::signal(SIGINT, stopRx);
  std::signal(SIGTERM, stopRx);
  uhd::set_thread_priority_safe();
  size_t channel = 0;
  auto radio = setupReceiver(config, plan, channel);
  uhd::stream_args_t args("fc32", config["DEVICE"]["otw_format"].as<std::string>("sc16"));
  args.channels = {channel};
  auto stream = radio->get_rx_stream(args);
  poc::armSharedPps(radio, plan, stopped);
  capture(radio, stream, plan, output_dir);
  return EXIT_SUCCESS;
}
