#include <gtest/gtest.h>
#include <algorithm>
#include <limits>
#include <vector>
#include "scheduled_tx.hpp"

/** @brief Simulate device time and burst acknowledgements without radio hardware. */
struct FakeTxTransport {
    double clock = 0;
    bool cancelled = false;
    bool short_send = false;
    ScheduledTxEvent response = ScheduledTxEvent::Ack;
    std::vector<double> targets;
    std::vector<double> confirmed;
    long ack_count = 0;
    long max_pending = 0;

    double now() { return clock; }
    bool stopped() { return cancelled; }
    std::size_t send(long index, double target) {
        targets.push_back(target);
        max_pending = std::max(max_pending, index + 1 - ack_count);
        return short_send ? 1 : 1120;
    }
    ScheduledTxEvent event(double) {
        clock = targets.at(ack_count) + (response == ScheduledTxEvent::None ? 3 : 0.00002);
        if (response == ScheduledTxEvent::Ack) ++ack_count;
        return response;
    }
    void acknowledged(long, double target) { confirmed.push_back(target); }
};

TEST(ScheduledTx, QueuesAheadAndAcknowledgesEveryTimedBurst) {
    FakeTxTransport tx;
    EXPECT_EQ(transmitScheduledBursts(tx, 10, 0.001, 0.00002, 20, 1120), 20);
    EXPECT_EQ(tx.max_pending, 6);
    ASSERT_EQ(tx.targets.size(), 20);
    EXPECT_EQ(tx.targets, tx.confirmed);
    for (long i = 0; i < 20; ++i) EXPECT_DOUBLE_EQ(tx.targets[i], 10 + 0.001 * i);
}

TEST(ScheduledTx, RejectsLateStartBeforeSending) {
    FakeTxTransport tx;
    tx.clock = 10;
    EXPECT_THROW(transmitScheduledBursts(tx, 10, 0.001, 0.00002, 1, 1120), std::runtime_error);
    EXPECT_TRUE(tx.targets.empty());
}

TEST(ScheduledTx, RequiresACompleteSend) {
    FakeTxTransport tx;
    tx.short_send = true;
    EXPECT_THROW(transmitScheduledBursts(tx, 10, 0.001, 0.00002, 10, 1120), std::runtime_error);
    EXPECT_TRUE(tx.confirmed.empty());
}

TEST(ScheduledTx, RejectsAsynchronousTransportFaults) {
    FakeTxTransport tx;
    tx.response = ScheduledTxEvent::Error;
    EXPECT_THROW(transmitScheduledBursts(tx, 10, 0.001, 0.00002, 10, 1120), std::runtime_error);
    EXPECT_TRUE(tx.confirmed.empty());
}

TEST(ScheduledTx, RequiresAcknowledgementBeforeReportingSuccess) {
    FakeTxTransport tx;
    tx.response = ScheduledTxEvent::None;
    EXPECT_THROW(transmitScheduledBursts(tx, 10, 0.001, 0.00002, 1, 1120), std::runtime_error);
    EXPECT_TRUE(tx.confirmed.empty());
}

TEST(ScheduledTx, CancelsBeforeQueueingMoreBursts) {
    FakeTxTransport tx;
    tx.cancelled = true;
    EXPECT_THROW(transmitScheduledBursts(tx, 10, 0.001, 0.00002, 1, 1120), std::runtime_error);
    EXPECT_TRUE(tx.targets.empty());
}

TEST(ScheduledTx, RejectsInvalidOrOverlappingPulseTrains) {
    EXPECT_THROW(validateScheduledTx(10, 0.001, 0.002, 1, 1120), std::invalid_argument);
    EXPECT_THROW(validateScheduledTx(10, 0.001, 0.00002, -1, 1120), std::invalid_argument);
    EXPECT_THROW(validateScheduledTx(10, 0.001, 0.00002, 0, 1120), std::invalid_argument);
    EXPECT_THROW(validateScheduledTx(10, 0.001, 0.00002, 1, 0), std::invalid_argument);
    EXPECT_THROW(validateScheduledTx(10, std::numeric_limits<double>::infinity(), 0.00002, 1, 1120), std::invalid_argument);
}
