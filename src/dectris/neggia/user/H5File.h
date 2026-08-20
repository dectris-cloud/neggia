// SPDX-License-Identifier: MIT

#ifndef H5FILE_H
#define H5FILE_H
#include <memory>
#include <string>

class H5File {
public:
    H5File() = default;
    H5File(const std::string& path);
    ~H5File();
    const char* fileAddress() const;
    std::string fileDir() const;
    // Bytes mapped when this handle was created, i.e. the file's size at that
    // moment. Data appended afterwards is NOT reachable through it.
    size_t mapSize() const;

private:
    std::shared_ptr<char> _fileAddress;
    std::string _fileDir;
};

#endif  // H5FILE_H
