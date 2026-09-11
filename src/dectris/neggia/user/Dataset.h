// SPDX-License-Identifier: MIT

#ifndef DATASET_H
#define DATASET_H

#include <dectris/neggia/data/H5DataLayoutMsg.h>
#include <memory>
#include <string>
#include <vector>
#include "H5File.h"

class H5LinkMsg;
class H5LinkInfoMsg;

class Dataset {
public:
    Dataset();
    Dataset(const H5File& h5File, const std::string& path);
    ~Dataset();

    unsigned int dataTypeId() const;
    size_t dataSize() const;
    bool isSigned() const;
    std::vector<size_t> dim() const;
    bool isChunked() const;
    std::vector<size_t> chunkShape() const;

    // chunkOffset is ignored for contigous or raw datasets
    void read(void* data,
              const std::vector<size_t>& chunkOffset =
                      std::vector<size_t>()) const;

private:
    typedef H5DataLayoutMsg::ConstDataPointer ConstDataPointer;

    void parseDataSymbolTable();
    void readRawData(ConstDataPointer rawData,
                     void* outData,
                     size_t outDataSize) const;
    void readLz4Data(ConstDataPointer rawData, void* data, size_t s) const;
    void readBitshuffleData(ConstDataPointer rawData,
                            void* data,
                            size_t s) const;
    size_t chunkDataSize() const;

    H5File _h5File;
    H5ObjectHeader _dataSymbolObjectHeader;
    H5DataLayoutMsg _dataLayoutMsg;
    std::vector<size_t> _dim;
    int _filterId;
    std::vector<int32_t> _filterCdValues;
    size_t _dataSize;
    int _dataTypeId;
    bool _isSigned;
};

#endif  // DATASET_H
