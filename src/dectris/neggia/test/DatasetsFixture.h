// SPDX-License-Identifier: MIT

#ifndef DATASETS_FIXTURE_H
#define DATASETS_FIXTURE_H

#include <gtest/gtest.h>
#include <cstdint>
#include <sstream>
#include <string>

class TestDataset : public ::testing::Test {
public:
    void SetUp();
    std::string getTargetDataset(size_t i) const;
    size_t getNumberOfImages() const;
    size_t getNumberOfTriggers() const;

    constexpr static size_t WIDTH = 11;
    constexpr static size_t HEIGHT = 13;
    constexpr static size_t N_FRAMES_PER_DATASET = 5;
    constexpr static double X_PIXEL_SIZE = 0.25;
    constexpr static double Y_PIXEL_SIZE = 0.5;

    unsigned int pixelMaskData[WIDTH * HEIGHT];
    using DATA_TYPE = uint16_t;
    DATA_TYPE dataArray[HEIGHT * WIDTH];

protected:
    virtual size_t getNumberOfDatasets() const = 0;
    virtual std::string getPathToSourceFile() const = 0;
};

class TestDatasetArtificialSmall001 : public TestDataset {
protected:
    size_t getNumberOfDatasets() const override { return 1; }
    std::string getPathToSourceFile() const override {
        return "h5-testfiles/dataset_artificial_small_001/test_master.h5";
    }
};

class TestDatasetArtificialLarge001 : public TestDataset {
protected:
    size_t getNumberOfDatasets() const override { return 1000; }
    std::string getPathToSourceFile() const override {
        return "h5-testfiles/dataset_artificial_large_001/test_master.h5";
    }
};

#endif  // DATASETS_FIXTURE_H
