/** @file TX-only version of sdr/main.cpp. GPS logging runs in a separate process. */
#include "main.hpp"

static volatile std::sig_atomic_t stop_signal_called = 0;

/** Request a bounded, graceful stop from SIGINT or SIGTERM. */
void sigIntHandler(int) { stop_signal_called = 1; }

/** Drain TX events, counting completed bursts and surfacing underruns/timing errors. */
void checkTxEvents(tx_streamer::sptr stream, uint64_t& completed, double timeout = 0.0) {
  async_metadata_t event;
  if (!stream->recv_async_msg(event, timeout)) return;
  do {
    if (event.event_code == async_metadata_t::EVENT_CODE_BURST_ACK) {
      ++completed;
    } else {
      throw std::runtime_error("TX streaming error (UHD async event " +
                          to_string(static_cast<int>(event.event_code)) + ").");
    }
  } while (stream->recv_async_msg(event, 0.0));
}

/** Send cached pulses on an independent hardware-time schedule; no receive progress dependency. */
void transmitWorker(Sdr& sdr, const Chirp& chirp,
                    const vector<complex<float>>& samples) {
  auto stream = sdr.getTxStream();
  auto usrp = sdr.getUsrp();
  vector<complex<float>> modulated(samples.size());
  uint64_t scheduled = 0, completed = 0;
  // Keep only a short window queued so Ctrl-C does not leave seconds of TX pending.
  const double kQueueWindow = 0.02;
  double device_origin = 0;
  chrono::steady_clock::time_point host_origin;
  // Avoid a USB control request for every pulse. Refresh the device/host clock
  // relationship periodically so long continuous runs do not accumulate drift.
  auto refreshTime = [&]() {
    const auto before = chrono::steady_clock::now();
    device_origin = usrp->get_time_now().get_real_secs();
    host_origin = before + (chrono::steady_clock::now() - before) / 2;
  };
  refreshTime();
  auto deviceTime = [&]() {
    if (chrono::steady_clock::now() - host_origin >= chrono::milliseconds(50)) refreshTime();
    return device_origin + chrono::duration<double>(chrono::steady_clock::now() - host_origin).count();
  };
  const double rate = sdr.getTxRate();
  const double first_time = deviceTime() + chirp.time_offset;
  double final_time = first_time;
  const size_t packet_size = stream->get_max_num_samps();
  cout << "[TX] Beginning pulse schedule; first device time: " << first_time << endl;
  try {
    while (!stop_signal_called &&
           (chirp.num_pulses == -1 || scheduled < static_cast<uint64_t>(chirp.num_pulses))) {
      const double pulse_time = first_time + scheduled * chirp.pulse_rep_int;
      // This time-based bound replaces the original RX pulse counter throttle.
      while (!stop_signal_called &&
             pulse_time - deviceTime() > kQueueWindow) {
        checkTxEvents(stream, completed);
        this_thread::sleep_for(chrono::milliseconds(1));
      }
      if (stop_signal_called) break;
      if (pulse_time <= deviceTime()) {
        throw std::runtime_error("TX scheduler fell behind. Increase PRI or reduce sample rate/load.");
      }
      const complex<float>* data = samples.data();
      if (chirp.phase_dither) {
        const auto phase = polar(1.0f, getNextPhase());
        transform(samples.begin(), samples.end(), modulated.begin(),
                  [phase](complex<float> sample) { return sample * phase; });
        data = modulated.data();
      }
      size_t offset = 0;
      while (offset < samples.size() && !stop_signal_called) {
        const size_t count = min(packet_size, samples.size() - offset);
        tx_metadata_t metadata;
        metadata.start_of_burst = offset == 0;
        metadata.end_of_burst = offset + count == samples.size();
        metadata.has_time_spec = offset == 0;
        metadata.time_spec = time_spec_t(pulse_time);
        // A timeout is a failed pulse: never silently count a partial waveform.
        const size_t sent = stream->send(data + offset, count, metadata, 0.2);
        if (sent != count) throw std::runtime_error("TX send timed out before the full pulse was queued.");
        offset += sent;
      }
      if (offset != samples.size()) break;
      ++scheduled;
      final_time = pulse_time + samples.size() / rate;
      checkTxEvents(stream, completed);
    }
    // send() queues samples; wait for hardware completion of the final burst.
    const double remaining = max(0.0, final_time - deviceTime());
    const auto deadline = chrono::steady_clock::now() +
        chrono::duration<double>(remaining + 1.0);
    while (!stop_signal_called && completed < scheduled) {
      checkTxEvents(stream, completed, 0.01);
      if (chrono::steady_clock::now() >= deadline) throw std::runtime_error("Timed out waiting for TX burst acknowledgements.");
    }
  } catch (...) {
    tx_metadata_t end;
    end.end_of_burst = true;
    stream->send(samples.data(), 0, end, 0.1);
    throw;
  }
  // Also closes a partially sent burst when interrupted.
  tx_metadata_t end;
  end.end_of_burst = true;
  stream->send(samples.data(), 0, end, 0.1);
  cout << "[TX] Pulses queued: " << scheduled << "; acknowledged: " << completed << endl;
}

/** Load the waveform once, validate it, then initialize only the transmitter. */
int UHD_SAFE_MAIN(int argc, char* argv[]) {
  if (argc < 2 || argc > 4) {
    cerr << "Usage: radar_tx CONFIG [REPOSITORY_ROOT] [--validate]" << endl;
    return EXIT_FAILURE;
  }
  const filesystem::path root = argc >= 3 ? filesystem::path(argv[2]) : filesystem::current_path();
  const auto config = YAML::LoadFile(argv[1]);
  Chirp chirp(config);
  Sdr sdr(config);
  const double rate = config["RF0"]["tx_rate"].as<double>();
  const double count = rate * chirp.tx_duration;
  if (!isfinite(count) || count < 1 || count >= static_cast<double>(numeric_limits<streamsize>::max() / sizeof(complex<float>)) ||
      abs(count - round(count)) > 1e-4) {
    throw std::invalid_argument("tx_rate * tx_duration must be a positive whole sample count.");
  }
  if (config["GENERATE"] &&
      abs(config["GENERATE"]["sample_rate"].as<double>() - rate) > rate * 1e-6) {
    throw std::invalid_argument("GENERATE.sample_rate must equal RF0.tx_rate.");
  }
  if (config["GPIO"] && (config["CHIRP"]["tr_on_lead"].as<double>(0) != 0 ||
                         config["CHIRP"]["tr_off_trail"].as<double>(0) != 0)) {
    throw std::invalid_argument("TX-only GPIO uses ATR; nonzero lead/trail delays are not supported.");
  }
  const auto files = config["FILES"];
  const auto path = root / files["output_dir"].as<string>("data") / files["chirp_loc"].as<string>();
  const size_t num_samples = static_cast<size_t>(round(count));
  ifstream input(path, ios::binary | ios::ate);
  const auto bytes = static_cast<streamsize>(num_samples * sizeof(complex<float>));
  if (!input || input.tellg() != bytes) {
    throw std::runtime_error("Chirp file missing or wrong length: " + path.string() + ". Run run_tx.py --prepare after changing the waveform.");
  }
  vector<complex<float>> samples(num_samples);
  input.seekg(0);
  if (!input.read(reinterpret_cast<char*>(samples.data()), bytes)) throw std::runtime_error("Could not read chirp samples.");
  for (const auto sample : samples) {
    if (!isfinite(sample.real()) || !isfinite(sample.imag())) throw std::runtime_error("Chirp contains non-finite samples.");
  }
  if (argc == 4) {
    if (string(argv[3]) != "--validate") throw std::invalid_argument("Unknown option.");
    cout << "[TX] Configuration and " << num_samples << " waveform samples validated; device not opened." << endl;
    return EXIT_SUCCESS;
  }
  if (chirp.num_pulses == 0) return EXIT_SUCCESS;
  signal(SIGINT, sigIntHandler);
  signal(SIGTERM, sigIntHandler);
  set_thread_priority_safe();
  sdr.setupUsrp();
  transmitWorker(sdr, chirp, samples);
  return EXIT_SUCCESS;
}
