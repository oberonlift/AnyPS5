#include <zarchive/zarchivereader.h>

#include <cstdlib>

ZArchiveReader* ZArchiveReader::OpenFromFile(const std::filesystem::path&) { return nullptr; }
ZArchiveReader::~ZArchiveReader() { std::abort(); }
ZArchiveNodeHandle ZArchiveReader::LookUp(std::string_view, bool, bool) { std::abort(); }
bool ZArchiveReader::IsDirectory(ZArchiveNodeHandle) const { std::abort(); }
bool ZArchiveReader::IsFile(ZArchiveNodeHandle) const { std::abort(); }
uint32_t ZArchiveReader::GetDirEntryCount(ZArchiveNodeHandle) const { std::abort(); }
bool ZArchiveReader::GetDirEntry(ZArchiveNodeHandle, uint32_t, DirEntry&) const { std::abort(); }
uint64_t ZArchiveReader::GetFileSize(ZArchiveNodeHandle) { std::abort(); }
uint64_t ZArchiveReader::ReadFromFile(ZArchiveNodeHandle, uint64_t, uint64_t, void*) { std::abort(); }
