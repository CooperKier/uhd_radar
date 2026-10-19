#include "../../sdr/sdr.hpp"
#include "../../sdr/rf_settings.hpp"
#include <gtest/gtest.h>
#include <fstream>

using namespace std;

class SdrHwTest{
    public:
    void revealCheck10MhzLock(Sdr& sdr){sdr.check10MhzLock();}
    void revealGpsLock(Sdr& sdr){sdr.gpsLock();}
    void revealCheckAndSetTime(Sdr& sdr){sdr.checkAndSetTime();}
    void revealDetectChannels(Sdr& sdr){sdr.detectChannels();}
    void revealSetRFParams(Sdr& sdr){sdr.setRFParams();}
    void revealRefLoLockDetect(Sdr& sdr){sdr.refLoLockDetect();}
    void revealSetupGpio(Sdr& sdr){sdr.setupGpio();}
    void revealSetupTx(Sdr& sdr){sdr.setupTx();}
    void revealSetupRx(Sdr& sdr){sdr.setupRx();}
};
/**
 * @brief tests that the construction of sdr class reads from yaml correctly
 * 
 * Has unit tests for all variables assigned in the loadConfigFromYaml 
 * fucntion in the sdr class to make sure the function pulls from Yaml
 * correctly
 */
TEST(loadConfigFromYaml, LoadsDefault){
    const string kYamlFile = string(CONFIG_DIR) + "/default.yaml";
    Sdr sdr(kYamlFile);
    //DEVICE
    EXPECT_EQ(sdr.getDeviceArgs(), "num_recv_frames=700,num_send_frames=700,recv_frame_size=11000,send_frame_size=11000");
    EXPECT_EQ(sdr.getSubdev(), "A:A");
    EXPECT_EQ(sdr.getClkRef(), "internal");
    EXPECT_EQ(sdr.getClkRate(), 56e6);
    EXPECT_EQ(sdr.getTxChannels(), "0");
    EXPECT_EQ(sdr.getRxChannels(), "0");
    EXPECT_EQ(sdr.getCpuFormat(), "fc32");
    EXPECT_EQ(sdr.getOtwFormat(), "sc12");

    //GPIO
    EXPECT_EQ(sdr.getPwrAmpPin(), -1);
    EXPECT_EQ(sdr.getGpioBank(), "FP0");
    EXPECT_EQ(sdr.getRefOutInt(), -1);

    //RF
    EXPECT_EQ(sdr.getRxRate(), 56e6);
    EXPECT_EQ(sdr.getTxRate(), 56e6);
    EXPECT_EQ(sdr.getFreq(), 450e6);
    EXPECT_EQ(sdr.getRxGain(), 36);
    EXPECT_EQ(sdr.getTxGain(), 65);
    EXPECT_EQ(sdr.getBw(), 56e6);
    EXPECT_EQ(sdr.getTxAnt(), "TX/RX");
    EXPECT_EQ(sdr.getRxAnt(), "RX2");
    EXPECT_EQ(sdr.getTransmit(), true);
}

/**
 * @brief tests GPIO mask getter functions
 *
 * these 4 getter methods are not tested with the other getter
 * methods in Loads Default because they are not directly read from YAML
 */
TEST(GetterMethods, UntestedInLoadsDefault){
    const string kYamlFile = string(CONFIG_DIR) + "/default.yaml";
    Sdr sdr(kYamlFile);

    EXPECT_EQ(sdr.getAmpGpioMask(), 0);
    EXPECT_EQ(sdr.getAtrMasks(), 0);
    EXPECT_EQ(sdr.getAtrControl(), 0);
    EXPECT_EQ(sdr.getGpioDdr(), 0);
}

/** @brief Exercises configuration loading without a physical USRP. */
class SdrConfigTest : public ::testing::Test {
protected:
    YAML::Node config;
    boost::filesystem::path filename;

    void SetUp() override {
        config = YAML::LoadFile(string(CONFIG_DIR) + "/default.yaml");
        // Break YAML aliases so changes to RF0 cannot also update RF1.
        config["RF1"] = YAML::Clone(config["RF1"]);
        filename = boost::filesystem::temp_directory_path() /
                   boost::filesystem::unique_path("orca-config-%%%%-%%%%.yaml");
    }

    void TearDown() override { boost::filesystem::remove(filename); }

    void writeConfig() {
        ofstream file(filename.string());
        file << config;
    }
};

TEST_F(SdrConfigTest, MapsEnabledGpioPin) {
    config["GPIO"]["pwr_amp_pin"] = 8;
    writeConfig();
    Sdr sdr(filename.string());
    EXPECT_EQ(sdr.getPwrAmpPin(), 6);
    EXPECT_EQ(sdr.getAmpGpioMask(), uint32_t(1) << 6);
    EXPECT_EQ(sdr.getAtrMasks(), sdr.getAmpGpioMask());
    EXPECT_EQ(sdr.getAtrControl(), sdr.getAmpGpioMask());
    EXPECT_EQ(sdr.getGpioDdr(), sdr.getAmpGpioMask());
}

TEST_F(SdrConfigTest, RejectsInvalidGpioPins) {
    for (int pin : {-2, 0, 1, 34}) {
        config["GPIO"]["pwr_amp_pin"] = pin;
        writeConfig();
        EXPECT_THROW(Sdr{filename.string()}, invalid_argument);
    }
}

TEST_F(SdrConfigTest, ReadsActiveFrontendInSingleDirectionModes) {
    for (bool transmit : {false, true}) {
        config["RF0"]["transmit"] = transmit;
        config["RF0"]["receive"] = !transmit;
        config["RF0"]["rx_rate"] = 10e6;
        config["RF0"]["tx_rate"] = 10e6;
        config["RF0"]["freq"] = 300e6;
        config["RF0"]["rx_gain"] = 20;
        config["RF0"]["tx_gain"] = 30;
        config["RF0"]["bw"] = 30e6;
        config["GENERATE"]["sample_rate"] = 10e6;
        writeConfig();
        Sdr sdr(filename.string());
        EXPECT_EQ(sdr.getTransmit(), transmit);
        EXPECT_EQ(sdr.getReceive(), !transmit);
        EXPECT_EQ(sdr.getRxRate(), 10e6);
        EXPECT_EQ(sdr.getTxRate(), 10e6);
        EXPECT_EQ(sdr.getFreq(), 300e6);
        EXPECT_EQ(sdr.getRxGain(), 20);
        EXPECT_EQ(sdr.getTxGain(), 30);
        EXPECT_EQ(sdr.getBw(), 30e6);
    }
}

TEST_F(SdrConfigTest, RejectsMissingEnabledChannelsBeforeAccessingDevice) {
    YAML::Node rf = config["RF0"];
    for (bool transmit : {false, true}) {
        rf["transmit"] = transmit;
        rf["receive"] = !transmit;
        EXPECT_THROW(set_rf_params_single(nullptr, rf, {}, {}), std::runtime_error);
        EXPECT_THROW(set_rf_params_single(nullptr, rf, {0, 1}, {0, 1}), std::runtime_error);
    }
}

TEST_F(SdrConfigTest, RejectsBothDirectionsDisabled) {
    config["RF0"]["transmit"] = false;
    config["RF0"]["receive"] = false;
    writeConfig();
    EXPECT_THROW(Sdr{filename.string()}, std::runtime_error);
    EXPECT_THROW(set_rf_params_single(nullptr, config["RF0"], {}, {}), std::runtime_error);
}

//hardware testing for usrp, all run at the very beginning

/**
 * @brief tests the check10MhzLock function when hardware and gps are connected
 * 
 * unit tests for this function. this function is accessed by a friend class 
 * at the top of this file and so are all the testing suites below
 * 
 * this checks to make sure the gps_locked variable is true
 */
TEST(check10MhzLock, checkParams){
    const string kYamlFile = string(CONFIG_DIR) + "/default.yaml";
    Sdr sdr(kYamlFile);
    SdrHwTest test;

    sdr.createUsrp();

    if(sdr.getClkRef() == "gpsdo"){
    test.revealCheck10MhzLock(sdr);
    EXPECT_TRUE(sdr.getUsrp()->get_mboard_sensor("gps_locked", 0).to_bool()); //should pass test if locked to GPSDO 10 Mhz reference
    }
}


/** 
 * @brief tests gpsLock function when gps and hardware are connected
 * 
 * this makes sure the gps_locked variable is true
 */
TEST(gpsLock, checkParams){
    const string kYamlFile = string(CONFIG_DIR) + "/default.yaml";
    Sdr sdr(kYamlFile);
    SdrHwTest test;

    sdr.createUsrp();

    if(sdr.getClkRef() == "gpsdo"){
    test.revealCheck10MhzLock(sdr);
    test.revealGpsLock(sdr);
    EXPECT_TRUE(sdr.getUsrp()->get_mboard_sensor("gps_locked", 0).to_bool());
}}


/** 
 * @brief tests checkAndSetTime function when hardware and gps are connected
 * 
 * this makes sure that the expected time and the actual time are the same
 */
TEST(checkAndSetTime, checkParams){
    const string kYamlFile = string(CONFIG_DIR) + "/default.yaml";
    Sdr sdr(kYamlFile);
    SdrHwTest test;

    sdr.createUsrp();
    if(sdr.getClkRef() == "gpsdo"){
        test.revealCheck10MhzLock(sdr);
        test.revealGpsLock(sdr);
        test.revealCheckAndSetTime(sdr);
        auto gps_time =sdr.getUsrp()->get_mboard_sensor("gps_time", 0).to_int();
        time_spec_t expected_time(gps_time + 1.0);
        auto actual_time = sdr.getUsrp()->get_time_last_pps(0);
        EXPECT_EQ(expected_time, actual_time); //should pass if USRP time is synchronized to GPS time
    //does this need to exist
}}


/**
 * @brief tests detectChannels function when hardware is connected
 * 
 * makes sure the number of tx_channels is greater than the channel we are using
 * makes sure no errors are thrown
 */
TEST(detectChannels, checkParams){
    const string kYamlFile = string(CONFIG_DIR) + "/default.yaml";
    Sdr sdr(kYamlFile);
    SdrHwTest test;

    sdr.createUsrp();

    sdr.getUsrp()->set_time_next_pps(time_spec_t(0.0));
    this_thread::sleep_for((chrono::milliseconds(1000)));
    if (sdr.getTransmit()) {
        sdr.getUsrp()->set_tx_subdev_spec(sdr.getSubdev());
     }
    sdr.getUsrp()->set_rx_subdev_spec(sdr.getSubdev());
    sdr.getUsrp()->set_master_clock_rate(sdr.getClkRate());

    test.revealDetectChannels(sdr);
    size_t i = sdr.getTxChannelStrings().size() - 1;
    auto tx_chan = stoi(sdr.getTxChannelStrings()[i]);
    EXPECT_GT(sdr.getUsrp()->get_tx_num_channels(), tx_chan);
    EXPECT_NO_THROW(test.revealDetectChannels(sdr)); //makes sure no errors are thrown when working how the function should

    size_t j = sdr.getRxChannelStrings().size()-1;
    auto rx_chan = stoi(sdr.getRxChannelStrings()[j]);
    EXPECT_GT(sdr.getUsrp()->get_rx_num_channels(), rx_chan);
    EXPECT_NO_THROW(test.revealDetectChannels(sdr)); //makes sure no errors are thrown when working how the function should;
    }


 /**
  * @brief tests setRFParams function when hardware is connected
  * 
  * makes sure no errors are thrown in the setup
  */
TEST(setRFParams, checkParams){
    const string kYamlFile = string(CONFIG_DIR) + "/default.yaml";
    Sdr sdr(kYamlFile);
    SdrHwTest test;

    sdr.createUsrp();

    sdr.getUsrp()->set_time_next_pps(time_spec_t(0.0));
    this_thread::sleep_for((chrono::milliseconds(1000)));
    if (sdr.getTransmit()) {
        sdr.getUsrp()->set_tx_subdev_spec(sdr.getSubdev());
     }
    sdr.getUsrp()->set_rx_subdev_spec(sdr.getSubdev());
    sdr.getUsrp()->set_master_clock_rate(sdr.getClkRate());

    test.revealDetectChannels(sdr);
    test.revealSetRFParams(sdr);
    EXPECT_NO_THROW(test.revealSetRFParams(sdr));
}


/**
 * @brief tests refLOLockDetect function when hardware is connected
 * 
 * makes sure no errors are thrown in the setup
 */
TEST(refLoLockDetect, checkParams){
    const string kYamlFile = string(CONFIG_DIR) + "/default.yaml";
    Sdr sdr(kYamlFile);
    SdrHwTest test;

    sdr.createUsrp();

    sdr.getUsrp()->set_time_next_pps(time_spec_t(0.0));
    this_thread::sleep_for((chrono::milliseconds(1000)));
    if (sdr.getTransmit()) {
        sdr.getUsrp()->set_tx_subdev_spec(sdr.getSubdev());
     }
    sdr.getUsrp()->set_rx_subdev_spec(sdr.getSubdev());
    sdr.getUsrp()->set_master_clock_rate(sdr.getClkRate());

    test.revealDetectChannels(sdr);
    test.revealSetRFParams(sdr);
    test.revealRefLoLockDetect(sdr);

    EXPECT_NO_THROW(test.revealRefLoLockDetect(sdr));
    }


/**
 * @brief tests setupGpio fucntion when hardware is connected
 * 
 * makes sure the power amp pin is not equal to -1
 */
TEST(setupGpio, checkParams){
    const string kYamlFile = string(CONFIG_DIR) + "/default.yaml";
    Sdr sdr(kYamlFile);
    SdrHwTest test;

    sdr.createUsrp();

    sdr.getUsrp()->set_time_next_pps(time_spec_t(0.0));
    this_thread::sleep_for((chrono::milliseconds(1000)));
    if (sdr.getTransmit()) {
        sdr.getUsrp()->set_tx_subdev_spec(sdr.getSubdev());
     }
    sdr.getUsrp()->set_rx_subdev_spec(sdr.getSubdev());
    sdr.getUsrp()->set_master_clock_rate(sdr.getClkRate());

    test.revealDetectChannels(sdr);
    test.revealSetRFParams(sdr);
    test.revealRefLoLockDetect(sdr);
    test.revealSetupGpio(sdr);
    if (sdr.getPwrAmpPin() == -1) {
        EXPECT_EQ(sdr.getAmpGpioMask(), 0);
    } else {
        EXPECT_NE(sdr.getAmpGpioMask(), 0);
    }
}
