// SPDX-License-Identifier: MIT
//
// Concurrent plugin_get_data must return byte-identical data to the
// single-threaded reference, across NUM_THREADS workers each making
// CALLS_PER_THREAD calls in randomised frame order. Expected to be
// race-free under TSan/Helgrind: each worker's H5DataCache is
// thread-confined (per-worker ownership; dispatch by
// frame_number % NUM_WORKERS in plugin_get_data).
//
// The shared stress body runStressOn() also runs against real bslz4
// external-link multi-datafile masters (eiger1/2) and against the large
// synthetic master capped at frames 1..80 for full worker-slot coverage
// (80 = 16 workers x 5 frames/dataset).

#include <dectris/neggia/user/H5File.h>
#include <dlfcn.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <random>
#include <thread>
#include <string>
#include <vector>
#include "DatasetsFixture.h"

typedef void (*plugin_open_file)(const char*,
                                 int info_array[1024],
                                 int* error_flag);
typedef void (*plugin_get_header)(int* nx,
                                  int* ny,
                                  int* nbytes,
                                  float* qx,
                                  float* qy,
                                  int* number_of_frames,
                                  int info_array[1024],
                                  int* error_flag);
typedef void (*plugin_get_data)(int* frame_number,
                                int* nx,
                                int* ny,
                                int data_array[],
                                int info_array[1024],
                                int* error_flag);
typedef void (*plugin_close_file)(int* error_flag);

namespace {
constexpr int NUM_THREADS = 16;
constexpr int CALLS_PER_THREAD = 100;
}  // namespace

class TestXdsPluginConcurrent : public TestDatasetArtificialSmall001 {
public:
    void SetUp() override {
        TestDataset::SetUp();
        pluginHandle = dlopen(PATH_TO_XDS_PLUGIN, RTLD_NOW);
        ASSERT_NE(pluginHandle, nullptr);
        open_file = (plugin_open_file)dlsym(pluginHandle, "plugin_open");
        get_header =
                (plugin_get_header)dlsym(pluginHandle, "plugin_get_header");
        get_data = (plugin_get_data)dlsym(pluginHandle, "plugin_get_data");
        close_file = (plugin_close_file)dlsym(pluginHandle, "plugin_close");
        error_flag = 1;
        std::memset(info_array, 0, sizeof(info_array));
    }
    void TearDown() override { dlclose(pluginHandle); }

    void* pluginHandle;
    plugin_open_file open_file;
    plugin_get_header get_header;
    plugin_get_data get_data;
    plugin_close_file close_file;
    int error_flag;
    int info_array[1024];

    // Shared stress body. capFrames > 0 limits the tested range to frames
    // 1..capFrames (contiguous -- deterministic worker-slot coverage);
    // expectedTotal > 0 pins the master's plugin-level frame count.
    void runStressOn(const std::string& masterPath,
                     int capFrames,
                     int expectedTotal) {
        // 1. Open and read header (single-threaded — happens-before any concurrent
        //    get_data calls per std::thread's spawn-creates-happens-before rule).
        open_file(masterPath.c_str(), info_array, &error_flag);
        ASSERT_EQ(error_flag, 0);

        int nx, ny, nbytes, total_frames;
        float qx, qy;
        get_header(&nx, &ny, &nbytes, &qx, &qy, &total_frames, info_array,
                   &error_flag);
        ASSERT_EQ(error_flag, 0);
        ASSERT_GT(total_frames, 0);
        if (expectedTotal > 0) {
            ASSERT_EQ(total_frames, expectedTotal);
        }
        int n_test = total_frames;
        if (capFrames > 0) {
            ASSERT_GE(total_frames, capFrames);
            n_test = capFrames;
        }
        const size_t frame_pixels = (size_t)nx * (size_t)ny;

        // 2. Build single-threaded reference: read every available frame once,
        //    serialise the int32 buffer into `reference[frame_index]`.
        std::vector<std::vector<int32_t>> reference(n_test);
        for (int f = 0; f < n_test; ++f) {
            reference[f].resize(frame_pixels);
            int frame_number = f + 1;  // plugin uses 1-indexed frame numbers
            get_data(&frame_number, &nx, &ny, reference[f].data(), info_array,
                     &error_flag);
            ASSERT_EQ(error_flag, 0) << "reference read failed at frame " << frame_number;
        }

        // 3. Spawn NUM_THREADS workers; each calls plugin_get_data
        //    CALLS_PER_THREAD times with frame numbers picked from a per-thread
        //    deterministic random sequence (different seed per thread so frame
        //    orderings differ across threads).
        std::atomic<int> mismatch_count{0};
        std::atomic<int> error_count{0};
        std::vector<std::thread> workers;
        workers.reserve(NUM_THREADS);
        for (int t = 0; t < NUM_THREADS; ++t) {
            workers.emplace_back([&, t]() {
                std::mt19937 rng(0xC0FFEE ^ t);  // per-thread deterministic seed
                std::uniform_int_distribution<int> frame_dist(1, n_test);
                std::vector<int32_t> buffer(frame_pixels);
                int thread_info[1024];
                std::memset(thread_info, 0, sizeof(thread_info));
                int thread_error = 0;
                for (int call = 0; call < CALLS_PER_THREAD; ++call) {
                    int frame_number = frame_dist(rng);
                    get_data(&frame_number, &nx, &ny, buffer.data(), thread_info,
                             &thread_error);
                    if (thread_error != 0) {
                        ++error_count;
                        return;
                    }
                    const auto& ref = reference[frame_number - 1];
                    if (!std::equal(buffer.begin(), buffer.end(), ref.begin())) {
                        ++mismatch_count;
                        return;
                    }
                }
            });
        }
        for (auto& w : workers) {
            w.join();
        }

        EXPECT_EQ(error_count.load(), 0)
                << "one or more workers reported a plugin error";
        EXPECT_EQ(mismatch_count.load(), 0)
                << "one or more workers read frame data that differed from "
                   "single-threaded reference";

        // 4. Close (single-threaded — happens-after all worker joins).
        close_file(&error_flag);
        ASSERT_EQ(error_flag, 0);
    }
};

// Original case: 5-frame synthetic master (slots 1-5 only).
TEST_F(TestXdsPluginConcurrent,
       ConcurrentGetDataMatchesSingleThreadedReference) {
    runStressOn(getPathToSourceFile(), 0, 0);
}

// Real bslz4 compression + external-link multi-datafile layout under
// 16-thread stress.
TEST_F(TestXdsPluginConcurrent, Eiger1Bslz4MultiDatafileConcurrent) {
    runStressOn(
            "h5-testfiles/datasets_eiger1/"
            "eiger1_testmode10_2datafiles_4images_bslz4_master.h5",
            0, 4);
}

TEST_F(TestXdsPluginConcurrent, Eiger2Bslz4Uint32MultiDatafileConcurrent) {
    runStressOn(
            "h5-testfiles/datasets_eiger2/"
            "eiger2_simread7_2datafiles_4images_bslz4_uint32_master.h5",
            0, 4);
}

// Full worker-slot coverage on the large synthetic master (1000 datasets
// x 5 frames): frames 1..80 contiguous cover all 16 slots under the
// current frame-modulo dispatch and under a dataset-index dispatch.
TEST_F(TestXdsPluginConcurrent, Large001AllWorkerSlotsCovered) {
    runStressOn("h5-testfiles/dataset_artificial_large_001/test_master.h5",
                16 * 5, 5000);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::GTEST_FLAG(catch_exceptions) = false;
    return RUN_ALL_TESTS();
}
