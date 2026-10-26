#include <gtest/gtest.h>
#include <limits>
#include "scheduled_start.hpp"

TEST(ScheduledStart, MapsUtcToAnIndependentDeviceClock) {
    HostTimeSample sample{1800000000.0, 1800000000.002, 7.0};
    EXPECT_NEAR(scheduledDeviceTime(1800000010.0, sample), 16.999, 1e-6);
}

TEST(ScheduledStart, RejectsMissedAndNearDeadlines) {
    HostTimeSample sample{1000.0, 1000.002, 7.0};
    EXPECT_THROW(scheduledDeviceTime(999.0, sample), std::runtime_error);
    EXPECT_THROW(scheduledDeviceTime(1001.0, sample), std::runtime_error);
}

TEST(ScheduledStart, RejectsSlowOrBackwardsClockQueries) {
    EXPECT_THROW(scheduledDeviceTime(1010.0, {1000.0, 1000.1, 7.0}), std::runtime_error);
    EXPECT_THROW(scheduledDeviceTime(1010.0, {1000.0, 999.9, 7.0}), std::runtime_error);
}

TEST(ScheduledStart, RejectsNonfiniteClockObservations) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(scheduledDeviceTime(nan, {1000.0, 1000.001, 7.0}), std::invalid_argument);
    EXPECT_THROW(scheduledDeviceTime(1010.0, {1000.0, nan, 7.0}), std::invalid_argument);
    EXPECT_THROW(scheduledDeviceTime(1010.0, {1000.0, 1000.001, nan}), std::invalid_argument);
}

TEST(ScheduledStart, VerifiesFirstPacketToWithinOneSample) {
    EXPECT_TRUE(scheduledTimestampMatches(10.0, 10.0, 56e6));
    EXPECT_TRUE(scheduledTimestampMatches(10.0 + 0.5 / 56e6, 10.0, 56e6));
    EXPECT_FALSE(scheduledTimestampMatches(10.0 + 2.0 / 56e6, 10.0, 56e6));
    EXPECT_FALSE(scheduledTimestampMatches(10.001, 10.0, 56e6));
    EXPECT_FALSE(scheduledTimestampMatches(10.0, 10.0, 0.0));
    EXPECT_FALSE(scheduledTimestampMatches(std::numeric_limits<double>::quiet_NaN(), 10.0, 56e6));
}
