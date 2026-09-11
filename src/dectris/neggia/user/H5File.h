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

private:
    std::shared_ptr<char> _fileAddress;
    std::string _fileDir;
};

#endif  // H5FILE_H
