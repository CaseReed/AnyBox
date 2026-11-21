#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace anybox::pe {

class PeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline constexpr std::uint16_t MachineAmd64 = 0x8664;
inline constexpr std::uint16_t MagicPe32Plus = 0x20b;
inline constexpr std::uint16_t SubsystemWindowsGui = 2;
inline constexpr std::uint16_t SubsystemWindowsCui = 3;
inline constexpr std::uint16_t SubsystemConsole = 14;
inline constexpr std::uint16_t CharacteristicDll = 0x2000;

enum Directory : std::size_t {
    DirectoryExport = 0,
    DirectoryImport = 1,
    DirectoryResource = 2,
    DirectoryException = 3,
    DirectorySecurity = 4,
    DirectoryBaseReloc = 5,
    DirectoryDebug = 6,
    DirectoryTls = 9,
    DirectoryLoadConfig = 10,
    DirectoryBoundImport = 11,
    DirectoryIat = 12,
    DirectoryDelayImport = 13,
    DirectoryClr = 14,
};

struct DataDirectory {
    std::uint32_t rva = 0;
    std::uint32_t size = 0;
};

struct Section {
    std::string name;
    std::uint32_t virtualSize = 0;
    std::uint32_t virtualAddress = 0;
    std::uint32_t rawSize = 0;
    std::uint32_t rawOffset = 0;
    std::uint32_t characteristics = 0;
};

struct ImportFunction {
    std::string name;
    std::optional<std::uint16_t> ordinal;
    std::uint16_t hint = 0;
};

struct ImportModule {
    std::string dll;
    bool delayLoaded = false;
    std::uint32_t nameFieldOffset = 0;
    std::uint32_t timeDateStampOffset = 0;
    std::uint32_t forwarderChainOffset = 0;
    std::uint32_t timeDateStamp = 0;
    std::uint32_t lookupRva = 0;
    std::uint32_t iatRva = 0;
    std::vector<ImportFunction> functions;
};

struct ExportFunction {
    std::string name;
    std::uint32_t ordinal = 0;
    std::optional<std::string> forwarder;
};

struct PeImage {
    std::vector<std::uint8_t> bytes;
    std::uint32_t peOffset = 0;
    std::uint32_t fileHeaderOffset = 0;
    std::uint32_t optionalHeaderOffset = 0;
    std::uint32_t sectionTableOffset = 0;
    std::uint16_t machine = 0;
    std::uint16_t characteristics = 0;
    std::uint16_t sizeOfOptionalHeader = 0;
    std::uint16_t magic = 0;
    std::uint16_t subsystem = 0;
    std::uint16_t dllCharacteristics = 0;
    std::uint64_t imageBase = 0;
    std::uint32_t sectionAlignment = 0;
    std::uint32_t fileAlignment = 0;
    std::uint32_t sizeOfImage = 0;
    std::uint32_t sizeOfHeaders = 0;
    std::uint32_t checksum = 0;
    std::vector<DataDirectory> directories;
    std::vector<Section> sections;
    std::vector<ImportModule> imports;
    std::vector<ExportFunction> exports;

    bool IsDll() const { return (characteristics & CharacteristicDll) != 0; }
    std::optional<std::uint32_t> RvaToOffset(std::uint32_t rva) const;
    std::uint32_t ChecksumOffset() const { return optionalHeaderOffset + 64; }
    std::uint32_t SubsystemOffset() const { return optionalHeaderOffset + 68; }
    std::uint32_t SizeOfImageOffset() const { return optionalHeaderOffset + 56; }
    std::uint32_t SizeOfInitializedDataOffset() const { return optionalHeaderOffset + 8; }
    std::uint32_t DirectoryOffset(std::size_t index) const { return optionalHeaderOffset + 112 + static_cast<std::uint32_t>(index) * 8; }
    std::uint32_t NumberOfSectionsOffset() const { return fileHeaderOffset + 2; }
};

PeImage Parse(std::vector<std::uint8_t> bytes);

std::uint32_t ComputeChecksum(std::span<const std::uint8_t> bytes, std::uint32_t checksumOffset);

std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment);

std::uint16_t ReadU16(std::span<const std::uint8_t> bytes, std::size_t offset);
std::uint32_t ReadU32(std::span<const std::uint8_t> bytes, std::size_t offset);
std::uint64_t ReadU64(std::span<const std::uint8_t> bytes, std::size_t offset);
void WriteU16(std::span<std::uint8_t> bytes, std::size_t offset, std::uint16_t value);
void WriteU32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value);
void WriteU64(std::span<std::uint8_t> bytes, std::size_t offset, std::uint64_t value);

std::string SubsystemName(std::uint16_t subsystem);

}
