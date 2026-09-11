// SPDX-License-Identifier: MIT

#include "H5ToXds.h"
#include <dectris/neggia/user/Dataset.h>
#include <dectris/neggia/user/H5File.h>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
#include "H5Error.h"

namespace {

struct H5DataCache {
    std::string filename;
    H5File h5File;
    int dimx;
    int dimy;
    int datasize;
    int nframesPerDataset;
    // Shared, not per-worker: the mask is read-only once the header phase is
    // done, and a private copy per worker costs dimx*dimy*4 bytes each (69 MB
    // per worker on a 16M detector).
    std::shared_ptr<int32_t> mask;
    float xpixelSize;
    float ypixelSize;
    bool masterFileOnly;
};

// Worker pool: each worker owns its own H5DataCache + H5File (own mmap =
// own kernel readahead state). plugin_get_data dispatches by
// frame_number % NUM_WORKERS — lock-free hot path (each worker's state is
// thread-confined). Single-open external contract preserved (refuse a
// second plugin_open while the pool is non-empty).
constexpr int NUM_WORKERS = 16;
std::vector<std::unique_ptr<H5DataCache>> GLOBAL_POOL;

void printVersionInfo() {
    std::cout << "This is neggia " << VERSION << " (Copyright Dectris 2020)"
              << std::endl;
}

template <class T>
int32_t applyOverflow(T value);

template <>
int32_t applyOverflow<uint32_t>(uint32_t value) {
    // XDS uses int32_t pixel values for processing therefore
    // cannot use any pixels from uint32_t >= 2**31
    // these values must be set to -1.
    return (value > INT32_MAX) ? -1 : value;
}

template <>
int32_t applyOverflow<uint16_t>(uint16_t value) {
    // For conversion from uint16_t we only need to set the
    // 'overflow' value of 0xFFFF to -1. All other values
    // from uint16_t are allowed.
    return (value == 0xFFFF) ? -1 : value;
}

template <>
int32_t applyOverflow<uint8_t>(uint8_t value) {
    // For conversion from uint8_t we only need to set the
    // 'overflow' value of 0xFF to -1. All other values
    // from uint8_t are allowed.
    return (value == 0xFF) ? -1 : value;
}

template <class T>
void applyMaskAndTransformToInt32(const T* indata,
                                  int outdata[],
                                  const int32_t* mask,
                                  size_t size) {
    for (size_t j = 0; j < size; ++j) {
        outdata[j] = mask[j] ? mask[j] : applyOverflow(indata[j]);
    }
}

template <class Type>
Type readFromDataset(const Dataset& d) {
    Type val;
    d.read(&val);
    return val;
}

uint64_t readNonZeroUint(const Dataset& d) {
    assert(d.dataTypeId() == 0);
    if (d.isSigned()) {
        int64_t value;
        switch (d.dataSize()) {
            case sizeof(int8_t):
                value = readFromDataset<int8_t>(d);
                break;
            case sizeof(int16_t):
                value = readFromDataset<int16_t>(d);
                break;
            case sizeof(int32_t):
                value = readFromDataset<int32_t>(d);
                break;
            case sizeof(int64_t):
                value = readFromDataset<int64_t>(d);
                break;
            default:
                throw H5Error(-4, "NEGGIA ERROR: UNSUPPORTED DATATYPE");
        }
        if (value <= 0) {
            throw H5Error(-4, "NEGGIA ERROR: VALUE ZERO OR NEGATIVE");
        }
        return value;
    }
    uint64_t value;
    switch (d.dataSize()) {
        case sizeof(uint8_t):
            value = readFromDataset<uint8_t>(d);
            break;
        case sizeof(uint16_t):
            value = readFromDataset<uint16_t>(d);
            break;
        case sizeof(uint32_t):
            value = readFromDataset<uint32_t>(d);
            break;
        case sizeof(uint64_t):
            value = readFromDataset<uint64_t>(d);
            break;
        default:
            throw H5Error(-4, "NEGGIA ERROR: UNSUPPORTED DATATYPE");
    }
    if (value == 0) {
        throw H5Error(-4, "NEGGIA ERROR: VALUE MUST BE NON-ZERO");
    }
    return value;
}

double readFloatFromDataset(const Dataset& d) {
    assert(d.dataTypeId() == 1);
    switch (d.dataSize()) {
        case sizeof(float):
            return readFromDataset<float>(d);
        case sizeof(double):
            return readFromDataset<double>(d);
        default:
            throw H5Error(-4, "NEGGIA ERROR: UNSUPPORTED DATATYPE");
    }
}

H5DataCache* getPreopenedDataCache(size_t frame_index = 0) {
    if (GLOBAL_POOL.empty()) {
        throw H5Error(-2, "NEGGIA ERROR: NO FILE HAS BEEN OPENED YET");
    }
    return GLOBAL_POOL[frame_index % GLOBAL_POOL.size()].get();
}

size_t correctFrameNumberOffset(int frameNumberStartingFromOne) {
    if (frameNumberStartingFromOne < 1) {
        throw H5Error(-2, "NEGGIA ERROR: Framenumbers start from 1");
    }
    return (size_t)frameNumberStartingFromOne - 1;
}

size_t getFrameNumberWithinDataset(size_t globalFrameNumber,
                                   const H5DataCache* dataCache) {
    return globalFrameNumber % (size_t)dataCache->nframesPerDataset;
}

std::string getPathToDataset(size_t globalFrameNumber,
                             const H5DataCache* dataCache) {
    size_t datasetNumber = globalFrameNumber / dataCache->nframesPerDataset + 1;
    if (dataCache->masterFileOnly) {
        if (datasetNumber > 1) {
            throw H5Error(-2,
                          "NEGGIA ERROR: Not all frames in master but "
                          "data_000001 not available");
        }
        return "/entry/data/data";
    }
    std::stringstream ss;
    ss << "/entry/data/data_" << std::setw(6) << std::setfill('0')
       << datasetNumber;
    return ss.str();
}

void setXPixelSize(H5DataCache* dataCache) {
    try {
        Dataset d(dataCache->h5File, "/entry/instrument/detector/x_pixel_size");
        dataCache->xpixelSize = (float)readFloatFromDataset(d);
    } catch (const std::out_of_range&) {
        dataCache->xpixelSize = 0;
    }
}

void setYPixelSize(H5DataCache* dataCache) {
    try {
        Dataset d(dataCache->h5File, "/entry/instrument/detector/y_pixel_size");
        dataCache->ypixelSize = (float)readFloatFromDataset(d);
    } catch (const std::out_of_range&) {
        dataCache->ypixelSize = 0;
    }
}

template <typename ValueType>
std::unique_ptr<ValueType[]> read2D(const Dataset& ds) {
    assert(ds.dataSize() == sizeof(ValueType));
    auto dim(ds.dim());
    assert(dim.size() == 2);
    size_t s = dim[0] * dim[1];
    auto output = std::unique_ptr<ValueType[]>(new ValueType[s]);
    ds.read(output.get());
    return output;
}

template <typename ValueType>
void preprocessPixelMask(int32_t* dest, ValueType* src, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (src[i] < 0 || src[i] > std::numeric_limits<uint32_t>::max())
            throw std::out_of_range(
                    "pixel mask value not in range [0, 0xffffffff]");
        if (src[i] & 0x1) {
            dest[i] = -1;
        } else if (src[i] & 0x1e) {
            dest[i] = -2;
        } else {
            dest[i] = 0;
        }
    }
}

void setPixelMask(H5DataCache* dataCache) {
    try {
        Dataset pixelMask(
                dataCache->h5File,
                "/entry/instrument/detector/detectorSpecific/pixel_mask");
        assert(pixelMask.dataTypeId() == 0);
        auto dim(pixelMask.dim());
        assert(dim.size() == 2);
        dataCache->dimx = (int)dim[1];
        dataCache->dimy = (int)dim[0];
        size_t s = (size_t)(dataCache->dimx * dataCache->dimy);
        dataCache->mask.reset(new int32_t[s], std::default_delete<int32_t[]>());
        if (pixelMask.isSigned()) {
            switch (pixelMask.dataSize()) {
                case 1: {
                    auto pm = read2D<int8_t>(pixelMask);
                    preprocessPixelMask(dataCache->mask.get(), pm.get(), s);
                    break;
                }
                case 2: {
                    auto pm = read2D<int16_t>(pixelMask);
                    preprocessPixelMask(dataCache->mask.get(), pm.get(), s);
                    break;
                }
                case 4: {
                    auto pm = read2D<int32_t>(pixelMask);
                    preprocessPixelMask(dataCache->mask.get(), pm.get(), s);
                    break;
                }
                case 8: {
                    auto pm = read2D<int64_t>(pixelMask);
                    preprocessPixelMask(dataCache->mask.get(), pm.get(), s);
                    break;
                }
                default:
                    throw H5Error(-4,
                                  "NEGGIA ERROR: UNSUPPORTED DATASIZE FOR "
                                  "PIXEL MASK");
            }
        } else {
            switch (pixelMask.dataSize()) {
                case 1: {
                    auto pm = read2D<uint8_t>(pixelMask);
                    preprocessPixelMask(dataCache->mask.get(), pm.get(), s);
                    break;
                }
                case 2: {
                    auto pm = read2D<uint16_t>(pixelMask);
                    preprocessPixelMask(dataCache->mask.get(), pm.get(), s);
                    break;
                }
                case 4: {
                    auto pm = read2D<uint32_t>(pixelMask);
                    preprocessPixelMask(dataCache->mask.get(), pm.get(), s);
                    break;
                }
                case 8: {
                    auto pm = read2D<uint64_t>(pixelMask);
                    preprocessPixelMask(dataCache->mask.get(), pm.get(), s);
                    break;
                }
                default:
                    throw H5Error(-4,
                                  "NEGGIA ERROR: UNSUPPORTED DATASIZE FOR "
                                  "PIXEL MASK");
            }
        }
    } catch (const std::out_of_range&) {
        throw H5Error(-4, "NEGGIA ERROR: CANNOT READ PIXEL MASK FROM ",
                      dataCache->filename);
    }
}

size_t getNumberOfImages(const H5DataCache* dataCache) {
    try {
        Dataset d(dataCache->h5File,
                  "/entry/instrument/detector/detectorSpecific/nimages");
        return readNonZeroUint(d);
    } catch (const std::out_of_range&) {
        throw H5Error(-4, "NEGGIA ERROR: CANNOT READ N_IMAGES FROM ",
                      dataCache->filename);
    } catch (const H5Error&) {
        throw H5Error(-4, "NEGGIA ERROR: UNSUPPORTED DATATYPE FOR N_IMAGES");
    }
}

size_t getNumberOfTriggers(const H5DataCache* dataCache) {
    try {
        Dataset d(dataCache->h5File,
                  "/entry/instrument/detector/detectorSpecific/ntrigger");
        return readNonZeroUint(d);
    } catch (const std::out_of_range&) {
        std::cerr << "NEGGIA WARNING: "
                     "/entry/instrument/detector/detectorSpecific/ntrigger not "
                     "found, using ntrigger = 1\n";
        return 1;
    } catch (const H5Error&) {
        throw H5Error(-4, "NEGGIA ERROR: UNSUPPORTED DATATYPE FOR N_TRIGGER");
    }
}

void setNFramesPerDatasetFromPath(H5DataCache* dataCache,
                                  const std::string& path) {
    try {
        Dataset dataset(dataCache->h5File, path);
        auto dim = dataset.dim();
        assert(dim.size() == 3);
        dataCache->nframesPerDataset = dim[0];
        assert(dataCache->dimy == dim[1]);
        assert(dataCache->dimx == dim[2]);
        dataCache->datasize = dataset.dataSize();
        assert(dataset.dataTypeId() == 0);
        assert(dataset.isChunked());
        assert(dataset.chunkShape() ==
               std::vector<size_t>({1, (unsigned int)dataCache->dimy,
                                    (unsigned int)dataCache->dimx}));
    } catch (const std::out_of_range&) {
        throw H5Error(-4, "NEGGIA ERROR: CANNOT OPEN " + path + " FROM ",
                      dataCache->filename);
    }
}

void setNFramesPerDataset(H5DataCache* dataCache) {
    try {
        setNFramesPerDatasetFromPath(dataCache, "/entry/data/data_000001");
        dataCache->masterFileOnly = false;
    } catch (const H5Error&) {
        setNFramesPerDatasetFromPath(dataCache, "/entry/data/data");
        dataCache->masterFileOnly = true;
    }
}

void applyMaskAndTransformToInt32(const H5DataCache* dataCache,
                                  const void* indata,
                                  int outdata[]) {
    switch (dataCache->datasize) {
        case 1:
            applyMaskAndTransformToInt32((const uint8_t*)indata, outdata,
                                         dataCache->mask.get(),
                                         dataCache->dimx * dataCache->dimy);
            break;
        case 2:
            applyMaskAndTransformToInt32((const uint16_t*)indata, outdata,
                                         dataCache->mask.get(),
                                         dataCache->dimx * dataCache->dimy);
            break;
        case 4:
            applyMaskAndTransformToInt32((const uint32_t*)indata, outdata,
                                         dataCache->mask.get(),
                                         dataCache->dimx * dataCache->dimy);
            break;
        default: {
            throw H5Error(-3, "NEGGIA ERROR: DATATYPE NOT SUPPORTED");
        }
    }
}

void readDataset(int* frame_number,
                 int data_array[],
                 const H5DataCache* dataCache) {
    size_t globalFrameNumber = correctFrameNumberOffset(*frame_number);
    std::string pathToDataset = getPathToDataset(globalFrameNumber, dataCache);
    try {
        Dataset dataset(dataCache->h5File, pathToDataset);
        size_t totNumberOfDatasets = dataset.dim()[0];
        size_t datasetFrameNumber =
                getFrameNumberWithinDataset(globalFrameNumber, dataCache);
        if (datasetFrameNumber >= totNumberOfDatasets)
            throw std::out_of_range("frame_number out of range");
        std::unique_ptr<char[]> buffer(
                new char[dataCache->dimx * dataCache->dimy *
                         dataCache->datasize]);
        dataset.read(buffer.get(),
                     std::vector<size_t>({datasetFrameNumber, 0, 0}));
        applyMaskAndTransformToInt32(dataCache, buffer.get(), data_array);
    } catch (const std::out_of_range&) {
        throw H5Error(-2, "NEGGIA ERROR: CANNOT OPEN FRAME ", *frame_number);
    }
}

void setInfoArray(int info[1024]) {
    info[0] = DECTRIS_H5TOXDS_CUSTOMER_ID;        // Customer ID [1:Dectris]
    info[1] = DECTRIS_H5TOXDS_VERSION_MAJOR;      // Version  [Major]
    info[2] = DECTRIS_H5TOXDS_VERSION_MINOR;      // Version  [Minor]
    info[3] = DECTRIS_H5TOXDS_VERSION_PATCH;      // Version  [Patch]
    info[4] = DECTRIS_H5TOXDS_VERSION_TIMESTAMP;  // Version  [timestamp]
}

}  // namespace

std::unique_ptr<H5DataCache> retVal(new H5DataCache);

extern "C" {

void plugin_open(const char* filename, int info_array[1024], int* error_flag) {
    setInfoArray(info_array);
    *error_flag = 0;
    printVersionInfo();
    if (!GLOBAL_POOL.empty()) {
        std::cerr << "NEGGIA ERROR: CAN ONLY OPEN ONE FILE AT A TIME "
                  << std::endl;
        *error_flag = -4;
        return;
    }
    try {
        GLOBAL_POOL.reserve(NUM_WORKERS);
        // Map the master once. H5File holds a shared_ptr to the mapping, so a
        // copy is a refcount bump: one open() and one mmap for the whole pool
        // instead of one per worker.
        H5File master(filename);
        for (int i = 0; i < NUM_WORKERS; ++i) {
            std::unique_ptr<H5DataCache> dataCache(new H5DataCache);
            dataCache->filename = filename;
            dataCache->h5File = master;
            GLOBAL_POOL.push_back(std::move(dataCache));
        }
    } catch (const std::out_of_range&) {
        std::cerr << "NEGGIA ERROR: CANNOT OPEN " << filename << std::endl;
        GLOBAL_POOL.clear();
        *error_flag = -4;
    }
}

void plugin_get_header(int* nx,
                       int* ny,
                       int* nbytes,
                       float* qx,
                       float* qy,
                       int* number_of_frames,
                       int info[1024],
                       int* error_flag) {
    setInfoArray(info);
    try {
        H5DataCache* dataCache = getPreopenedDataCache();
        setXPixelSize(dataCache);
        setYPixelSize(dataCache);
        setPixelMask(dataCache);
        size_t nimages = getNumberOfImages(dataCache);
        size_t ntrigger = getNumberOfTriggers(dataCache);
        setNFramesPerDataset(dataCache);

        // Propagate parsed header fields from worker[0] to worker[1..K-1]
        // so each worker's H5DataCache is fully populated before any
        // concurrent plugin_get_data call. H5DataCache is
        // write-once-then-immutable after this point, so per-worker copies
        // are thread-confined and need no synchronization on the hot path.
        for (size_t i = 1; i < GLOBAL_POOL.size(); ++i) {
            H5DataCache* w = GLOBAL_POOL[i].get();
            w->dimx = dataCache->dimx;
            w->dimy = dataCache->dimy;
            w->datasize = dataCache->datasize;
            w->nframesPerDataset = dataCache->nframesPerDataset;
            w->xpixelSize = dataCache->xpixelSize;
            w->ypixelSize = dataCache->ypixelSize;
            w->masterFileOnly = dataCache->masterFileOnly;
            w->mask = dataCache->mask;
        }

        *nx = dataCache->dimx;
        *ny = dataCache->dimy;
        *nbytes = dataCache->datasize;
        *qx = dataCache->xpixelSize;
        *qy = dataCache->ypixelSize;
        *number_of_frames = (int)(nimages * ntrigger);

    } catch (const H5Error& error) {
        std::cerr << error.what() << std::endl;
        *error_flag = error.getErrorCode();
        return;
    }
    *error_flag = 0;
    return;
}

void plugin_get_data(int* frame_number,
                     int* nx,
                     int* ny,
                     int data_array[],
                     int info_array[1024],
                     int* error_flag) {
    setInfoArray(info_array);
    try {
        H5DataCache* dataCache = getPreopenedDataCache((size_t)*frame_number);
        readDataset(frame_number, data_array, dataCache);
    } catch (const H5Error& error) {
        std::cerr << error.what() << std::endl;
        *error_flag = error.getErrorCode();
        return;
    }
    *error_flag = 0;
}

void plugin_close(int* error_flag) {
    GLOBAL_POOL.clear();
}

}  // extern "C"
