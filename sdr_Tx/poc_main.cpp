/** @file TX-only framed preamble/chirp PoC, armed by the first shared PPS edge. */
#include "sdr.hpp"
#include "poc_protocol.hpp"
#include "shared_pps.hpp"
#include <uhd/utils/safe_main.hpp>
#include <uhd/utils/thread.hpp>
#include <csignal>
#include <iostream>

static volatile std::sig_atomic_t stopped = 0;
static void stopTx(int) { stopped = 1; }

/** Surface transport errors instead of silently losing pulse/frame indices. */
static void txEvents(uhd::tx_streamer::sptr stream, uint32_t& acknowledged, double timeout = 0) {
  uhd::async_metadata_t event;
  if (!stream->recv_async_msg(event, timeout)) return;
  do {
    if (event.event_code != uhd::async_metadata_t::EVENT_CODE_BURST_ACK)
      throw std::runtime_error("TX async error: " + std::to_string(int(event.event_code)));
    ++acknowledged;
  } while (stream->recv_async_msg(event, 0));
}

/** Emit synthetic noiseless RF samples for an end-to-end test without opening a radio. */
static void emitFrames(const poc::Plan& plan, const std::vector<poc::Sample>& chirp,
                        const std::string& filename) {
  std::ofstream output(filename, std::ios::binary);
  if (!output) throw std::runtime_error("Cannot open emitted waveform file.");
  std::mt19937 phases(plan.phase_seed);
  for (uint32_t i = 0; i < plan.frames; ++i) {
    const auto frame = poc::makeFrame(plan, i, chirp, phases);
    output.write(reinterpret_cast<const char*>(frame.data()), frame.size() * sizeof(poc::Sample));
    if (!output) throw std::runtime_error("Emitted waveform write failed.");
  }
}

/** Send finite, timed frames while bounding queued future transmission. */
static void transmitFrames(Sdr& sdr, const poc::Plan& plan, const std::vector<poc::Sample>& chirp) {
  const auto radio = sdr.getUsrp();
  const auto stream = sdr.getTxStream();
  std::mt19937 phases(plan.phase_seed);
  uint32_t acknowledged = 0, sent_frames = 0;
  try {
    for (uint32_t i = 0; i < plan.frames && !stopped; ++i) {
      const auto frame = poc::makeFrame(plan, i, chirp, phases);
      const double send_at = plan.frameTime(i);
      while (!stopped && send_at - radio->get_time_now().get_real_secs() > 0.1) {
        txEvents(stream, acknowledged);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      if (stopped) break;
      if (radio->get_time_now().get_real_secs() >= send_at)
        throw std::runtime_error("Missed a TX frame deadline; reduce rate or frame workload.");
      uhd::tx_metadata_t metadata;
      metadata.has_time_spec = true;
      metadata.start_of_burst = true;
      metadata.end_of_burst = true;
      metadata.time_spec = uhd::time_spec_t(send_at);
      const auto count = stream->send(frame.data(), frame.size(), metadata, 1.0);
      if (count != frame.size()) throw std::runtime_error("Incomplete TX frame: send timed out.");
      ++sent_frames;
      txEvents(stream, acknowledged);
    }
    const double end_at = plan.frameTime(plan.frames) + 2;
    while (!stopped && acknowledged < sent_frames) {
      txEvents(stream, acknowledged, 0.02);
      if (radio->get_time_now().get_real_secs() > end_at)
        throw std::runtime_error("Missing TX burst acknowledgement.");
    }
  } catch (...) {
    uhd::tx_metadata_t end;
    end.end_of_burst = true;
    stream->send(chirp.data(), 0, end, 0.1);
    throw;
  }
  uhd::tx_metadata_t end;
  end.end_of_burst = true;
  stream->send(chirp.data(), 0, end, 0.1);
  std::cout << "[TX] Frames queued: " << sent_frames << "; acknowledged: " << acknowledged << std::endl;
}

/** Prepare/validate/emit and live modes all consume the identical shared experiment file. */
int UHD_SAFE_MAIN(int argc, char* argv[]) {
  if (argc < 3) throw std::invalid_argument("Usage: radar_tx_poc CONFIG ROOT [--validate | --emit FILE] [--device-args ARGS]");
  auto config = YAML::LoadFile(argv[1]);
  bool validate = false;
  std::string emit;
  for (int i = 3; i < argc; ++i) {
    const std::string option = argv[i];
    if (option == "--validate") validate = true;
    else if (option == "--emit" && i + 1 < argc) emit = argv[++i];
    else if (option == "--device-args" && i + 1 < argc) config["DEVICE"]["device_args"] = argv[++i];
    else throw std::invalid_argument("Unknown/missing TX option: " + option);
  }
  const poc::Plan plan(config);
  Sdr sdr(config); // TX-only setup; no RX stream is constructed.
  const auto chirp = poc::loadChirp(config, plan, argv[2]);
  if (validate) {
    std::cout << "[TX] PoC validated: " << plan.frames << " frames, " << plan.total_pulses << " pulses." << std::endl;
    return EXIT_SUCCESS;
  }
  if (!emit.empty()) {
    emitFrames(plan, chirp, emit);
    return EXIT_SUCCESS;
  }
  std::signal(SIGINT, stopTx);
  std::signal(SIGTERM, stopTx);
  uhd::set_thread_priority_safe();
  sdr.setupUsrp();
  poc::armSharedPps(sdr.getUsrp(), plan, stopped);
  transmitFrames(sdr, plan, chirp);
  return stopped ? 130 : EXIT_SUCCESS;
}
