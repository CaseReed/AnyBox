#include "pe/PeImage.hpp"

#include <algorithm>
#include <format>

namespace anybox::pe {

namespace {

constexpr std::size_t MaxImportDescriptors = 4096;
constexpr std::size_t MaxThunks = 65536;
constexpr std::size_t MaxExports = 65536;
constexpr std::uint64_t OrdinalFlag = 0x8000000000000000ull;

void Require(std::span<const std::uint8_t> bytes, std::size_t offset, std::size_t size, const char* what) {
    if (offset > bytes.size() || size > bytes.size() - offset) {
        throw PeError(std::format("{} at file offset 0x{:x} (size {}) lies outside the {}-byte file", what, offset, size, bytes.size()));
    }
}

std::string ReadCString(const PeImage& image, std::uint32_t rva, const char* what) {
    const auto offset = image.RvaToOffset(rva);
    if (!offset) {
        throw PeError(std::format("{} at RVA 0x{:x} is not backed by file data", what, rva));
    }
    const auto begin = image.bytes.begin() + *offset;
    const auto end = std::find(begin, image.bytes.end(), std::uint8_t{0});
    if (end == image.bytes.end()) {
        throw PeError(std::format("{} at RVA 0x{:x} is not NUL-terminated", what, rva));
    }
    return std::string(begin, end);
}

std::uint32_t OffsetOf(const PeImage& image, std::uint32_t rva, const char* what) {
    const auto offset = image.RvaToOffset(rva);
    if (!offset) {
        throw PeError(std::format("{} at RVA 0x{:x} is not backed by file data", what, rva));
    }
    return *offset;
}

std::vector<ImportFunction> ReadThunks(const PeImage& image, std::uint32_t tableRva, const std::string& dll) {
    std::vector<ImportFunction> functions;
    if (tableRva == 0) {
        return functions;
    }
    std::uint32_t offset = OffsetOf(image, tableRva, "import lookup table");
    for (std::size_t i = 0;; ++i) {
        if (i == MaxThunks) {
            throw PeError(std::format("import table of {} has more than {} entries", dll, MaxThunks));
        }
        Require(image.bytes, offset, 8, "import thunk");
        const std::uint64_t thunk = ReadU64(image.bytes, offset);
        offset += 8;
        if (thunk == 0) {
            break;
        }
        ImportFunction function;
        if ((thunk & OrdinalFlag) != 0) {
            function.ordinal = static_cast<std::uint16_t>(thunk & 0xffff);
        } else {
            const std::uint32_t hintRva = static_cast<std::uint32_t>(thunk & 0x7fffffff);
            const std::uint32_t hintOffset = OffsetOf(image, hintRva, "import hint/name entry");
            Require(image.bytes, hintOffset, 2, "import hint");
            function.hint = ReadU16(image.bytes, hintOffset);
            function.name = ReadCString(image, hintRva + 2, "import name");
        }
        functions.push_back(std::move(function));
    }
    return functions;
}

void ReadImports(PeImage& image) {
    const DataDirectory directory = image.directories[DirectoryImport];
    if (directory.rva == 0) {
        return;
    }
    std::uint32_t offset = OffsetOf(image, directory.rva, "import directory");
    for (std::size_t i = 0;; ++i) {
        if (i == MaxImportDescriptors) {
            throw PeError(std::format("import directory has more than {} descriptors", MaxImportDescriptors));
        }
        Require(image.bytes, offset, 20, "import descriptor");
        const std::uint32_t lookup = ReadU32(image.bytes, offset);
        const std::uint32_t stamp = ReadU32(image.bytes, offset + 4);
        const std::uint32_t nameRva = ReadU32(image.bytes, offset + 12);
        const std::uint32_t iat = ReadU32(image.bytes, offset + 16);
        if (lookup == 0 && stamp == 0 && nameRva == 0 && iat == 0) {
            break;
        }
        ImportModule module;
        module.dll = ReadCString(image, nameRva, "import DLL name");
        module.nameFieldOffset = offset + 12;
        module.timeDateStampOffset = offset + 4;
        module.forwarderChainOffset = offset + 8;
        module.timeDateStamp = stamp;
        module.lookupRva = lookup;
        module.iatRva = iat;
        module.functions = ReadThunks(image, lookup != 0 ? lookup : iat, module.dll);
        image.imports.push_back(std::move(module));
        offset += 20;
    }
}

void ReadDelayImports(PeImage& image) {
    const DataDirectory directory = image.directories[DirectoryDelayImport];
    if (directory.rva == 0) {
        return;
    }
    std::uint32_t offset = OffsetOf(image, directory.rva, "delay import directory");
    for (std::size_t i = 0;; ++i) {
        if (i == MaxImportDescriptors) {
            throw PeError(std::format("delay import directory has more than {} descriptors", MaxImportDescriptors));
        }
        Require(image.bytes, offset, 32, "delay import descriptor");
        const std::uint32_t attributes = ReadU32(image.bytes, offset);
        const std::uint32_t nameRva = ReadU32(image.bytes, offset + 4);
        const std::uint32_t iat = ReadU32(image.bytes, offset + 12);
        const std::uint32_t names = ReadU32(image.bytes, offset + 16);
        if (nameRva == 0) {
            break;
        }
        if ((attributes & 1) == 0) {
            throw PeError("delay import descriptor uses virtual addresses instead of RVAs, which PE32+ images do not support");
        }
        ImportModule module;
        module.dll = ReadCString(image, nameRva, "delay import DLL name");
        module.delayLoaded = true;
        module.nameFieldOffset = offset + 4;
        module.timeDateStampOffset = offset + 28;
        module.timeDateStamp = ReadU32(image.bytes, offset + 28);
        module.lookupRva = names;
        module.iatRva = iat;
        module.functions = ReadThunks(image, names, module.dll);
        image.imports.push_back(std::move(module));
        offset += 32;
    }
}

void ReadExports(PeImage& image) {
    const DataDirectory directory = image.directories[DirectoryExport];
    if (directory.rva == 0) {
        return;
    }
    const std::uint32_t offset = OffsetOf(image, directory.rva, "export directory");
    Require(image.bytes, offset, 40, "export directory");
    const std::uint32_t base = ReadU32(image.bytes, offset + 16);
    const std::uint32_t functionCount = ReadU32(image.bytes, offset + 20);
    const std::uint32_t nameCount = ReadU32(image.bytes, offset + 24);
    if (functionCount > MaxExports || nameCount > MaxExports) {
        throw PeError(std::format("export directory declares {} functions and {} names, more than {}", functionCount, nameCount, MaxExports));
    }
    const std::uint32_t functionsOffset = OffsetOf(image, ReadU32(image.bytes, offset + 28), "export address table");
    Require(image.bytes, functionsOffset, std::size_t{functionCount} * 4, "export address table");
    std::vector<std::string> names(functionCount);
    if (nameCount != 0) {
        const std::uint32_t namesOffset = OffsetOf(image, ReadU32(image.bytes, offset + 32), "export name table");
        const std::uint32_t ordinalsOffset = OffsetOf(image, ReadU32(image.bytes, offset + 36), "export ordinal table");
        Require(image.bytes, namesOffset, std::size_t{nameCount} * 4, "export name table");
        Require(image.bytes, ordinalsOffset, std::size_t{nameCount} * 2, "export ordinal table");
        for (std::uint32_t i = 0; i < nameCount; ++i) {
            const std::uint16_t index = ReadU16(image.bytes, ordinalsOffset + i * 2);
            if (index >= functionCount) {
                throw PeError(std::format("export name {} points at function index {}, past {}", i, index, functionCount));
            }
            names[index] = ReadCString(image, ReadU32(image.bytes, namesOffset + i * 4), "export name");
        }
    }
    for (std::uint32_t i = 0; i < functionCount; ++i) {
        const std::uint32_t rva = ReadU32(image.bytes, functionsOffset + i * 4);
        if (rva == 0) {
            continue;
        }
        ExportFunction function;
        function.name = names[i];
        function.ordinal = base + i;
        if (rva >= directory.rva && rva - directory.rva < directory.size) {
            function.forwarder = ReadCString(image, rva, "export forwarder");
        }
        image.exports.push_back(std::move(function));
    }
}

}

std::uint16_t ReadU16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    Require(bytes, offset, 2, "16-bit field");
    return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

std::uint32_t ReadU32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    Require(bytes, offset, 4, "32-bit field");
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint64_t ReadU64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint64_t>(ReadU32(bytes, offset)) | (static_cast<std::uint64_t>(ReadU32(bytes, offset + 4)) << 32);
}

void WriteU16(std::span<std::uint8_t> bytes, std::size_t offset, std::uint16_t value) {
    Require(bytes, offset, 2, "16-bit field");
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void WriteU32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
    Require(bytes, offset, 4, "32-bit field");
    for (int i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

void WriteU64(std::span<std::uint8_t> bytes, std::size_t offset, std::uint64_t value) {
    WriteU32(bytes, offset, static_cast<std::uint32_t>(value));
    WriteU32(bytes, offset + 4, static_cast<std::uint32_t>(value >> 32));
}

std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment) {
    if (alignment == 0) {
        return value;
    }
    const std::uint64_t aligned = (static_cast<std::uint64_t>(value) + alignment - 1) / alignment * alignment;
    if (aligned > 0xffffffffull) {
        throw PeError(std::format("0x{:x} aligned to 0x{:x} does not fit in 32 bits", value, alignment));
    }
    return static_cast<std::uint32_t>(aligned);
}

std::optional<std::uint32_t> PeImage::RvaToOffset(std::uint32_t rva) const {
    if (rva < sizeOfHeaders) {
        if (rva < bytes.size()) {
            return rva;
        }
        return std::nullopt;
    }
    for (const Section& section : sections) {
        const std::uint32_t span = std::max(section.virtualSize, section.rawSize);
        if (rva >= section.virtualAddress && rva - section.virtualAddress < span) {
            const std::uint32_t delta = rva - section.virtualAddress;
            if (delta >= section.rawSize) {
                return std::nullopt;
            }
            const std::uint64_t offset = static_cast<std::uint64_t>(section.rawOffset) + delta;
            if (offset >= bytes.size()) {
                return std::nullopt;
            }
            return static_cast<std::uint32_t>(offset);
        }
    }
    return std::nullopt;
}

PeImage Parse(std::vector<std::uint8_t> input) {
    PeImage image;
    image.bytes = std::move(input);
    const std::span<const std::uint8_t> bytes = image.bytes;
    Require(bytes, 0, 0x40, "DOS header");
    if (bytes[0] != 'M' || bytes[1] != 'Z') {
        throw PeError("not a PE file: the DOS header does not start with MZ");
    }
    image.peOffset = ReadU32(bytes, 0x3c);
    Require(bytes, image.peOffset, 24, "PE signature and file header");
    if (ReadU32(bytes, image.peOffset) != 0x00004550) {
        throw PeError(std::format("not a PE file: no PE signature at 0x{:x}", image.peOffset));
    }
    image.fileHeaderOffset = image.peOffset + 4;
    image.machine = ReadU16(bytes, image.fileHeaderOffset);
    const std::uint16_t sectionCount = ReadU16(bytes, image.fileHeaderOffset + 2);
    image.sizeOfOptionalHeader = ReadU16(bytes, image.fileHeaderOffset + 16);
    image.characteristics = ReadU16(bytes, image.fileHeaderOffset + 18);
    image.optionalHeaderOffset = image.fileHeaderOffset + 20;
    Require(bytes, image.optionalHeaderOffset, image.sizeOfOptionalHeader, "optional header");
    image.magic = ReadU16(bytes, image.optionalHeaderOffset);
    if (image.magic != MagicPe32Plus) {
        throw PeError(std::format("optional header magic 0x{:x} is not PE32+ (0x20b); GDK titles are 64-bit", image.magic));
    }
    if (image.sizeOfOptionalHeader < 112) {
        throw PeError(std::format("PE32+ optional header is {} bytes, shorter than the 112 bytes before the data directories", image.sizeOfOptionalHeader));
    }
    const std::uint32_t o = image.optionalHeaderOffset;
    image.imageBase = ReadU64(bytes, o + 24);
    image.sectionAlignment = ReadU32(bytes, o + 32);
    image.fileAlignment = ReadU32(bytes, o + 36);
    image.sizeOfImage = ReadU32(bytes, o + 56);
    image.sizeOfHeaders = ReadU32(bytes, o + 60);
    image.checksum = ReadU32(bytes, o + 64);
    image.subsystem = ReadU16(bytes, o + 68);
    image.dllCharacteristics = ReadU16(bytes, o + 70);
    const std::uint32_t directoryCount = ReadU32(bytes, o + 108);
    if (directoryCount > 16 || 112 + directoryCount * 8 > image.sizeOfOptionalHeader) {
        throw PeError(std::format("{} data directories do not fit in a {}-byte optional header", directoryCount, image.sizeOfOptionalHeader));
    }
    image.directories.resize(16);
    for (std::uint32_t i = 0; i < directoryCount; ++i) {
        image.directories[i] = {ReadU32(bytes, o + 112 + i * 8), ReadU32(bytes, o + 116 + i * 8)};
    }
    image.sectionTableOffset = image.optionalHeaderOffset + image.sizeOfOptionalHeader;
    Require(bytes, image.sectionTableOffset, std::size_t{sectionCount} * 40, "section table");
    for (std::uint16_t i = 0; i < sectionCount; ++i) {
        const std::uint32_t s = image.sectionTableOffset + i * 40u;
        Section section;
        const auto nameBegin = image.bytes.begin() + s;
        section.name.assign(nameBegin, std::find(nameBegin, nameBegin + 8, std::uint8_t{0}));
        section.virtualSize = ReadU32(bytes, s + 8);
        section.virtualAddress = ReadU32(bytes, s + 12);
        section.rawSize = ReadU32(bytes, s + 16);
        section.rawOffset = ReadU32(bytes, s + 20);
        section.characteristics = ReadU32(bytes, s + 36);
        image.sections.push_back(std::move(section));
    }
    ReadImports(image);
    ReadDelayImports(image);
    ReadExports(image);
    return image;
}

std::uint32_t ComputeChecksum(std::span<const std::uint8_t> bytes, std::uint32_t checksumOffset) {
    std::uint64_t sum = 0;
    const std::size_t size = bytes.size();
    for (std::size_t i = 0; i < size; i += 2) {
        if (i == checksumOffset || i == static_cast<std::size_t>(checksumOffset) + 2) {
            continue;
        }
        const std::uint32_t word = bytes[i] | (i + 1 < size ? static_cast<std::uint32_t>(bytes[i + 1]) << 8 : 0u);
        sum += word;
        sum = (sum & 0xffff) + (sum >> 16);
    }
    sum = (sum & 0xffff) + (sum >> 16);
    return static_cast<std::uint32_t>(sum + size);
}

std::string SubsystemName(std::uint16_t subsystem) {
    switch (subsystem) {
    case 1: return "native";
    case SubsystemWindowsGui: return "windows-gui";
    case SubsystemWindowsCui: return "windows-cui";
    case SubsystemConsole: return "console";
    default: return std::format("subsystem-{}", subsystem);
    }
}

}
