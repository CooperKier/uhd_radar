/** @file Hardware-free RF packet, synchronization rejection and pulse-stream tests. */
#include "train.hpp"
#include "rx_timeline.hpp"
#include <iostream>
#include <limits>
#include <random>
#include <string>

using namespace bistatic;
static size_t checks = 0;

void expect(bool condition, const std::string& message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}

void testPackets() {
  Packet announcement{0x123456789abcdef0ull, 2, 7, 0xfedcba9876543210ull, 1};
  auto bytes = encodePacket(announcement);
  auto decoded = decodePacket(bytes);
  expect(sameAttempt(decoded, announcement) && decoded.repetition == announcement.repetition, "Packet roundtrip");
  for (size_t bit = 0; bit < bytes.size() * 8; ++bit) {
    auto corrupt = bytes;
    corrupt[bit / 8] ^= uint8_t(1 << (bit % 8));
    bool rejected = false;
    try { decodePacket(corrupt); } catch (const std::invalid_argument&) { rejected = true; }
    expect(rejected, "Corrupt control packet must fail CRC/version validation");
  }
  expect(crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xcbf43926u, "Standard CRC vector");
  for (auto change : {std::pair<size_t, uint8_t>{4, 1}, {5, 2}, {5, 3}, {6, 3}, {7, 1}}) {
    auto unsupported = bytes;
    unsupported[change.first] = change.second;
    unsupported.resize(40);
    appendInt(unsupported, crc32(unsupported.data(), unsupported.size()), 4);
    bool rejected = false;
    try { decodePacket(unsupported); } catch (const std::invalid_argument&) { rejected = true; }
    expect(rejected, "Reject legacy protocol, removed control types and invalid repetitions even with valid CRC");
  }
}

void testOneWaySync() {
  Packet announcement{100, 8, 25, 300};
  for (size_t repetition = 0; repetition < kSyncPhases.size(); ++repetition) {
    announcement.repetition = static_cast<uint8_t>(repetition);
    expect(validAnnouncement(announcement, 300, kSyncPhases[repetition], 0.001), "Each repeated announcement independently establishes sync");
    expect(!validAnnouncement(announcement, 300, kSyncPhases[repetition] + 1, 0.001), "Reject a different PPS second");
    expect(!validAnnouncement(announcement, 300, kSyncPhases[repetition] + 0.01, 0.001), "Reject wrong PPS phase");
    expect(!validAnnouncement(announcement, 301, kSyncPhases[repetition], 0.001), "Reject mismatched experiment fingerprint");
    expect(!validAnnouncement(announcement, 300, kSyncPhases[repetition], 0.001, announcement), "Reject duplicate copies after accepting a cycle");
  }
  auto valid = [&](const Packet& packet) { return validAnnouncement(packet, 300, kSyncPhases[packet.repetition], 0.001, announcement); };
  auto next = announcement;
  ++next.cycle;
  ++next.attempt;
  expect(valid(next), "Join next cycle without a return channel");
  next.cycle += 3;
  next.attempt += 3;
  expect(valid(next), "Recover after missing several complete cycles");
  next.cycle = announcement.cycle;
  expect(!valid(next), "A new attempt cannot replay a completed cycle");
  next.cycle = announcement.cycle + 1;
  next.attempt = announcement.attempt;
  expect(!valid(next), "A stale attempt cannot start a new cycle");
  next.session = 101;
  next.cycle = next.attempt = 1;
  expect(valid(next), "Accept a newly started transmitter session");
  // First two broadcasts were lost; the third alone must still identify the cycle.
  auto found = detect(modulate(announcement));
  expect(found && found->packet.repetition == 2 && valid(found->packet) == false, "Third copy survives RF encoding and duplicate rejection");
  expect(validAnnouncement(found->packet, 300, kSyncPhases[2], 0.001), "Third copy works without first copies or any ACK");
  expect(!detect(std::vector<Sample>(packetSamples() * 2, Sample(0, 0))), "Missing preamble cannot establish sync");
}

void testModem() {
  Packet packet{123, 456, 789, 987654321};
  std::mt19937 generator(42);
  std::normal_distribution<float> noise(0, 0.025f);
  for (double carrier : {-25000., -9000., 0., 12000., 25000.}) {
    for (size_t padding : {size_t(0), size_t(37), size_t(419)}) {
      auto signal = modulate(packet);
      std::vector<Sample> channel(padding + signal.size() + 100, Sample(0, 0));
      for (size_t i = 0; i < channel.size(); ++i) {
        if (i >= padding && i < padding + signal.size()) channel[i] = signal[i - padding] * std::polar(1.f, float(1.3 + 2 * kPi * carrier * i / kControlRate));
        channel[i] += Sample(noise(generator), noise(generator));
      }
      DetectionStats stats;
      auto found = detect(channel, 0.85, &stats);
      expect(found && sameAttempt(found->packet, packet), "Decode RF with unknown phase, noise, carrier offset and arbitrary sample alignment");
      expect(stats.decoded_packets == 1 && stats.preamble_hits > 0 && stats.best_score >= 0.85,
             "Diagnostic decoder path preserves decoding across noise, carrier offset and sample alignment");
      expect(std::abs(double(found->sample) - double(padding)) <= 8, "Preamble timestamp within half a control symbol");
      expect(std::abs(found->carrier_hz - carrier) < 500, "Carrier-offset estimate");
    }
  }
  auto damaged = modulate(packet);
  size_t corrupt_symbol = kPreambleSymbols + 100;
  for (size_t i = corrupt_symbol * kSamplesPerSymbol; i < (corrupt_symbol + 1) * kSamplesPerSymbol; ++i) damaged[i] = -damaged[i];
  DetectionStats damaged_stats;
  expect(!detect(damaged, 0.85, &damaged_stats), "Corrupted RF payload must not pass CRC");
  expect(damaged_stats.invalid_headers > 0 && damaged_stats.decoded_packets == 0,
         "Diagnostics distinguish a received preamble with an invalid payload from a decoded packet");
  std::vector<Sample> random_noise(packetSamples() * 3);
  for (auto& sample : random_noise) sample = Sample(noise(generator), noise(generator));
  expect(!detect(random_noise), "Noise must not establish synchronization");
  auto truncated = modulate(packet);
  truncated.resize(truncated.size() - 1);
  expect(!detect(truncated), "Truncated RF header cannot establish synchronization");
}

void testTrainChunks() {
  const double rate = 56000000, pri = 0.0002;
  std::vector<Sample> reference(1120);
  for (size_t i = 0; i < reference.size(); ++i) reference[i] = std::polar(0.8f, float(0.001 * i * i));
  for (bool dither : {false, true}) for (double ratio : {0.99995, 1., 1.00005}) {
    TrainGenerator train(reference, 31, pri, rate, ratio, dither);
    std::vector<Sample> signal(train.samples()), chunk(1237);
    for (size_t first = 0; first < signal.size();) {
      size_t count = std::min(chunk.size(), signal.size() - first);
      train.fill(chunk, first, count);
      std::copy(chunk.begin(), chunk.begin() + count, signal.begin() + first);
      first += count;
    }
    expect(train.generated() == 31, "Generate all chirps across TX buffer boundaries");
    WindowExtractor extractor(31, reference.size(), pri, rate, ratio, dither);
    for (size_t first = 0; first < signal.size();) {
      size_t count = std::min(size_t(971), signal.size() - first);
      extractor.consume(signal.data() + first, count, [&](const std::vector<Sample>& record) {
        expect(record.size() == reference.size(), "Preserve RX record length");
        for (size_t i = 0; i < record.size(); ++i)
          expect(std::abs(record[i] - reference[i]) < 1e-5, "Recover unmodified complex IQ and undo optional phase sequence");
      });
      first += count;
    }
    expect(extractor.extracted() == 31, "Extract all windows across different RX buffer boundaries");
    for (size_t pulse = 1; pulse < 31; ++pulse) {
      uint64_t start = pulseOffset(pulse, pri, rate, ratio);
      expect(signal[start - 1] == Sample(0, 0), "Zero-valued interval before each chirp");
    }
    // New sync cycle restarts the same phase sequence on both stations.
    TrainGenerator second(reference, 31, pri, rate, ratio, dither);
    second.fill(chunk, 0, chunk.size());
    expect(std::equal(chunk.begin(), chunk.end(), signal.begin()), "Phase sequence restarts at each cycle");
  }
}

void testIndependentEpochs() {
  // Both PPS streams refer to the same physical edge, but RX started 103 seconds earlier.
  double tx_ratio = 1.00002, rx_ratio = 0.99997;
  double tx_anchor = 5 * tx_ratio, rx_anchor = 108 * rx_ratio;
  for (double phase : kSyncPhases) {
    double received_sync = rx_anchor + phase * rx_ratio;
    expect(std::abs((received_sync - rx_anchor) / rx_ratio - phase) < 1e-10, "Every preamble copy maps to the same RX local PPS edge");
  }
  double tx_edge = tx_anchor + kLeadEdges * tx_ratio;
  double rx_edge = rx_anchor + kLeadEdges * rx_ratio;
  for (uint64_t pulse : {uint64_t(0), uint64_t(5000), uint64_t(9999)}) {
    double tx_time = tx_edge + tx_ratio + double(pulseOffset(pulse, 0.0002, 56e6, tx_ratio)) / 56e6;
    double rx_time = rx_edge + rx_ratio + double(pulseOffset(pulse, 0.0002, 56e6, rx_ratio)) / 56e6;
    double tx_physical = tx_time / tx_ratio - 5;
    double rx_physical = rx_time / rx_ratio - 108;
    expect(std::abs(tx_physical - rx_physical) < 2 / 56e6, "Clock-rate compensation prevents train timing from accumulating drift");
  }
}

/** Exercise complex averaging through split pulse windows and optional phase removal. */
void testPresumming() {
  const double rate = 1e6, pri = 0.0002;
  const std::vector<Sample> waveform{{1, 2}, {-3, 4}, {5, -6}};
  for (uint64_t factor : {1, 2, 5}) for (bool dither : {false, true}) {
    TrainGenerator train(waveform, 10, pri, rate, 1, dither);
    std::vector<Sample> signal(train.samples());
    train.fill(signal, 0, signal.size());
    // Scale each pulse so output verifies the group membership and the normalization.
    for (uint64_t pulse = 0; pulse < 10; ++pulse)
      for (size_t i = 0; i < waveform.size(); ++i)
        signal[pulseOffset(pulse, pri, rate, 1) + i] *= float(pulse + 1);
    WindowExtractor windows(10, waveform.size(), pri, rate, 1, dither);
    PulseAverager averages(waveform.size(), factor);
    for (size_t first = 0; first < signal.size();) {
      size_t count = std::min(size_t(101), signal.size() - first);
      windows.consume(signal.data() + first, count, [&](const std::vector<Sample>& record) {
        averages.consume(record, [&](const std::vector<Sample>& average) {
          float scale = float(averages.saved() * factor) + float(factor + 1) / 2;
          for (size_t i = 0; i < waveform.size(); ++i)
            expect(std::abs(average[i] - waveform[i] * scale) < 1e-4, "Presum complete consecutive windows after phase removal");
        });
      });
      first += count;
    }
    expect(windows.extracted() == 10 && averages.saved() == 10 / factor && !averages.buffered(),
           "Presums reduce output records without changing acquired pulses");
  }
  PulseAverager incomplete(1, 3);
  size_t emitted = 0;
  auto save = [&](const std::vector<Sample>&) { ++emitted; };
  incomplete.consume({Sample(1, 2)}, save);
  incomplete.consume({Sample(3, 4)}, save);
  expect(emitted == 0 && incomplete.buffered() == 2, "Never write a partial presum group");
  bool rejected = false;
  try { PulseAverager invalid(1, 0); } catch (const std::invalid_argument&) { rejected = true; }
  expect(rejected, "Reject zero presumming factor");
  rejected = false;
  try { incomplete.consume({Sample(1, 0), Sample(2, 0)}, save); }
  catch (const std::invalid_argument&) { rejected = true; }
  expect(rejected && incomplete.buffered() == 2, "Reject wrong window size without advancing the group");
  PulseAverager rotating(1, 2);
  rotating.consume({Sample(1, 0)}, save);
  rotating.consume({Sample(-1, 0)}, [&](const std::vector<Sample>& average) {
    expect(average[0] == Sample(0, 0), "Complex presumming preserves phase cancellation rather than averaging magnitudes");
  });
}

/** Preserve pulse locations when the actual stream start consumes part of the lead-in. */
void testGuardedRxWindows() {
  const double rate = 56e6, desired = 11.00000020982143, pri = 0.0002;
  const std::vector<Sample> waveform{{1, 2}, {-3, 4}, {5, -6}, {-7, -8}};
  for (double startup : {-1., 0., 7.25, 55999.75}) for (size_t chunk : {size_t(1), size_t(101), size_t(262144)}) {
    WindowExtractor windows(3, waveform.size(), pri, rate, 1, false);
    RxTimeline timeline(desired, rate, windows.samples());
    double first_time = timeline.commandTime() + startup / rate;
    auto leading = uint64_t(std::llround(56000 - startup));
    std::vector<Sample> stream(timeline.requestedSamples(), Sample(0, 0));
    for (uint64_t pulse = 0; pulse < 3; ++pulse)
      std::copy(waveform.begin(), waveform.end(), stream.begin() + leading + pulseOffset(pulse, pri, rate, 1));
    size_t emitted = 0;
    for (size_t first = 0; !timeline.complete();) {
      size_t count = size_t(std::min<uint64_t>(chunk, timeline.remaining()));
      auto slice = timeline.accept(true, first_time + double(first) / rate, count);
      windows.consume(stream.data() + first + slice.offset, slice.count, [&](const std::vector<Sample>& record) {
        expect(record == waveform, "Guard trimming preserves every radar sample across RX chunk boundaries");
        ++emitted;
      });
      first += count;
    }
    expect(emitted == 3 && windows.extracted() == 3, "Save every requested window and omit leading/trailing guards");
    expect(timeline.discardedSamples() == leading, "Measure lead-in using the actual hardware timestamp");
    expect(std::abs((timeline.firstWindowTime() - desired) * rate) <= 0.50001, "Align radar windows to the nearest sample");
  }
}

/** Reproduce the 7.25-sample offset from the hardware log without moving the radar window. */
void testObservedRxStartup() {
  for (double rate : {1e6, 14e6, 56e6}) {
    RxTimeline timeline(11.00000020982143, rate, 1232);
    double first = rate == 56e6 ? 11.000000339285714 - 0.001 : timeline.commandTime() + 7.25 / rate;
    auto slice = timeline.accept(true, first, 100);
    expect(slice.count == 0, "A short first chunk can contain only discarded lead-in");
    expect(std::abs((first - timeline.commandTime()) * rate - 7.25) < 1e-5, "Reproduce measured startup offset");
    expect(std::abs((timeline.firstWindowTime() - 11.00000020982143) * rate - 0.25) < 1e-5,
           "Correct the initial offset while retaining only quarter-sample rounding error");
    expect(std::abs(timeline.expectedTime() - first - 100 / rate) * rate < 1e-5,
           "Check subsequent timestamps against the actual first sample");
  }
}

/** Reject unavailable start samples, missing timestamps and subsequent stream gaps. */
void testRxTimelineRejection() {
  const double rate = 56e6;
  for (double time : {10.998, 11.000001, std::numeric_limits<double>::quiet_NaN()}) {
    RxTimeline timeline(11, rate, 1232);
    bool rejected = false;
    try { timeline.accept(true, time, 100); } catch (const std::runtime_error&) { rejected = true; }
    expect(rejected && timeline.receivedSamples() == 0, "Reject stale, too-late or invalid first timestamps");
  }
  for (bool has_time : {false, true}) for (double gap : {-3., 3., 3661.}) {
    RxTimeline timeline(11, rate, 1232);
    double first = timeline.commandTime() + 7.25 / rate;
    timeline.accept(true, first, 100);
    bool rejected = false;
    try { timeline.accept(has_time, first + (100 + gap) / rate, 100); }
    catch (const std::runtime_error&) { rejected = true; }
    expect(rejected && timeline.receivedSamples() == 100, "Reject later missing timestamps, gaps and out-of-order data even during lead-in");
  }
}

/** Diagnostic only: per-sample complex AWGN SNR, not a hardware sensitivity measurement. */
void noiseSweep() {
  constexpr size_t trials = 200;
  constexpr float amplitude = 0.35f;
  Packet packet{123, 456, 789, 987654321};
  auto waveform = modulate(packet, amplitude);
  std::mt19937 generator(2026);
  std::normal_distribution<float> unit_noise(0, 1);
  std::uniform_real_distribution<double> phase_distribution(-kPi, kPi);
  std::uniform_real_distribution<double> carrier_distribution(-12000, 12000);
  std::uniform_int_distribution<size_t> padding_distribution(0, 63);
  std::cout << "# Software simulation: 200 packets/SNR, complex white Gaussian noise, random phase, carrier +/-12 kHz.\n"
            << "# SNR is signal power / total complex noise power per input sample. No multipath, fading or clipping.\n"
            << "snr_db,decoded,trials,success_percent,noise_only_valid_headers\n";
  for (double snr_db : {-20., -15., -10., -5., 0., 5., 10., 15., 20.}) {
    float sigma = amplitude / std::sqrt(float(2 * std::pow(10., snr_db / 10.)));
    size_t decoded_count = 0, false_headers = 0;
    for (size_t trial = 0; trial < trials; ++trial) {
      size_t padding = padding_distribution(generator);
      double phase = phase_distribution(generator), carrier = carrier_distribution(generator);
      std::vector<Sample> channel(padding + waveform.size() + 64), noise_only(channel.size());
      for (size_t i = 0; i < channel.size(); ++i) {
        auto noise = Sample(sigma * unit_noise(generator), sigma * unit_noise(generator));
        noise_only[i] = channel[i] = noise;
        if (i >= padding && i < padding + waveform.size())
          channel[i] += waveform[i - padding] * std::polar(1.f, float(phase + 2 * kPi * carrier * i / kControlRate));
      }
      auto found = detect(channel);
      if (found && sameAttempt(found->packet, packet)) ++decoded_count;
      if (detect(noise_only)) ++false_headers;
    }
    std::cout << snr_db << ',' << decoded_count << ',' << trials << ','
              << 100. * decoded_count / trials << ',' << false_headers << '\n';
  }
}

int main(int argc, char* argv[]) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--noise-sweep") {
      noiseSweep();
      return 0;
    }
    if (argc != 1) throw std::invalid_argument("Usage: test_bistatic_protocol [--noise-sweep]");
    testPackets();
    testOneWaySync();
    testModem();
    testTrainChunks();
    testIndependentEpochs();
    testPresumming();
    testGuardedRxWindows();
    testObservedRxStartup();
    testRxTimelineRejection();
    std::cout << "Passed " << checks << " checks (one-way sync, lost copies, CRC, PPS mapping, clock drift, IQ windows).\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Failed after " << checks << " checks: " << error.what() << '\n';
    return 1;
  }
}
