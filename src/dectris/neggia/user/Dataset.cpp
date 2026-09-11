// SPDX-License-Identifier: MIT

#include "Dataset.h"
#include <assert.h>
#include <dectris/neggia/data/Decode.h>
#include <dectris/neggia/data/H5BTreeVersion2.h>
#include <dectris/neggia/data/H5DataspaceMsg.h>
#include <dectris/neggia/data/H5DatatypeMsg.h>
#include <dectris/neggia/data/H5FilterMsg.h>
#include <dectris/neggia/data/H5FractalHeap.h>
#include <dectris/neggia/data/H5LinkInfoMessage.h>
#include <dectris/neggia/data/H5LinkMsg.h>
#include <dectris/neggia/data/H5LocalHeap.h>
#include <dectris/neggia/data/H5Superblock.h>
#include <dectris/neggia/data/constants.h>
#include <string.h>
#include <iostream>
#include <sstream>

Dataset::Dataset()
      : _filterId(-1), _dataSize(0), _dataTypeId(-1), _isSigned(false) {}

Dataset::Dataset(const H5File& h5File, const std::string& path)
      : _h5File(h5File),
        _filterId(-1),
        _dataSize(0),
        _dataTypeId(-1),
        _isSigned(false) {
    H5Superblock root(_h5File.fileAddress());
    try {
        auto resolvedPath = root.resolve(path);
        while (resolvedPath.externalFile) {
            auto targetFile = resolvedPath.externalFile->filename;
            if (targetFile[0] != '/')
                targetFile = _h5File.fileDir() + "/" + targetFile;
            _h5File = H5File(targetFile);
            root = H5Superblock(_h5File.fileAddress());
            resolvedPath = root.resolve(resolvedPath.externalFile->h5Path);
        }
        _dataSymbolObjectHeader = resolvedPath.objectHeader;
    } catch (std::exception& exc) {
        std::cerr << "exception during path resolve: " << exc.what() << "\n";
        throw std::out_of_range(exc.what());
    }
    parseDataSymbolTable();
}

Dataset::~Dataset() {}

unsigned int Dataset::dataTypeId() const {
    return (unsigned int)_dataTypeId;
}

size_t Dataset::dataSize() const {
    return _dataSize;
}

bool Dataset::isSigned() const {
    return _isSigned;
}

std::vector<size_t> Dataset::dim() const {
    return _dim;
}

bool Dataset::isChunked() const {
    return _dataLayoutMsg.isChunked();
}

std::vector<size_t> Dataset::chunkShape() const {
    return _dataLayoutMsg.chunkShape();
}

void Dataset::readRawData(ConstDataPointer rawData,
                          void* outData,
                          size_t outDataSize) const {
    if (outDataSize != rawData.size) {
        throw std::runtime_error("cannot read " + std::to_string(outDataSize) +
                                 " bytes from a dataset of size " +
                                 std::to_string(rawData.size));
    }
    memcpy(outData, rawData.data, outDataSize);
}

void Dataset::readLz4Data(Dataset::ConstDataPointer rawData,
                          void* data,
                          size_t s) const {
    lz4Decode(rawData.data, (char*)data, s);
}

void Dataset::readBitshuffleData(ConstDataPointer rawData,
                                 void* data,
                                 size_t s) const {
    assert(_filterCdValues.size() > 4);
    assert(_filterCdValues[4] == BSHUF_H5_COMPRESS_LZ4);
    int elementSize = _filterCdValues[2];
    bshufUncompressLz4(rawData.data, (char*)data, s, elementSize);
}

size_t Dataset::chunkDataSize() const {
    size_t s = _dataSize;
    if (isChunked()) {
        // Size one chunk from the chunk shape itself. Deriving it from _dim
        // instead only works for 3-D frame stacks: for a 2-D chunked dataset
        // (an Eiger pixel mask, for instance) _dim[2] reads past the end of
        // the vector, and the resulting garbage is either harmlessly large or
        // small enough to make the decoder reject a valid chunk.
        for (auto d : chunkShape())
            s *= d;
        return s;
    }
    for (auto d : _dim)
        s *= d;
    return s;
}

void Dataset::read(void* data, const std::vector<size_t>& chunkOffset) const {
    auto rawData = _dataLayoutMsg.getRawData(_dataSize, chunkOffset);
    size_t s = chunkDataSize();
    switch (_filterId) {
        case -1:
            readRawData(rawData, data, s);
            break;
        case LZ4_FILTER:
            readLz4Data(rawData, data, s);
            break;
        case BSHUF_H5FILTER:
            readBitshuffleData(rawData, data, s);
            break;
        default:
            throw std::runtime_error("filter " + std::to_string(_filterId) +
                                     " not supported.");
    }
}

void Dataset::parseDataSymbolTable() {
    for (int i = 0; i < _dataSymbolObjectHeader.numberOfMessages(); ++i) {
        H5HeaderMessage msg(_dataSymbolObjectHeader.headerMessage(i));
        switch (msg.type) {
            case H5DataspaceMsg::TYPE_ID: {
                H5DataspaceMsg dataspaceMsg(msg.object);
                _dim.clear();
                for (size_t i = 0; i < dataspaceMsg.rank(); ++i) {
                    _dim.push_back(dataspaceMsg.dim(i));
                }
                break;
            }
            case H5DataLayoutMsg::TYPE_ID: {
                _dataLayoutMsg = H5DataLayoutMsg(msg.object);
                break;
            }
            case H5FilterMsg::TYPE_ID: {
                H5FilterMsg filterMsg(msg.object);
                // We accept at most one filter
                assert(filterMsg.nFilters() <= 1);
                if (filterMsg.nFilters() == 1)
                    _filterId = filterMsg.filterId(0);
                _filterCdValues = filterMsg.clientData(0);
                break;
            }
            case H5DatatypeMsg::TYPE_ID: {
                H5DatatypeMsg datatypeMsg(msg.object);
                _dataSize = datatypeMsg.dataSize();
                _dataTypeId = datatypeMsg.typeId();
                _isSigned = datatypeMsg.isSigned();
                break;
            }
        }
    }
    assert(_dataTypeId >= 0);
    assert(_dataSize > 0);
}
