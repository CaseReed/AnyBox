#include "relinker/Conversion.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <map>

namespace anybox::relinker {

namespace {

using pe::PeError;
using pe::PeImage;

constexpr std::uint32_t SectionCharacteristics = 0x40000040;

void StripCertificate(PeImage& image, ConversionResult& result) {
    const pe::DataDirectory certificate = image.directories[pe::DirectorySecurity];
    if (certificate.rva == 0 && certificate.size == 0) {
        return;
    }
    const std::uint64_t end = static_cast<std::uint64_t>(certificate.rva) + certificate.size;
    if (end > image.bytes.size()) {
        throw PeError(std::format("certificate table at 0x{:x} with size {} lies past the end of the file", certificate.rva, certificate.size));
    }
    if (pe::AlignUp(static_cast<std::uint32_t>(end), 8) >= image.bytes.size()) {
        image.bytes.resize(certificate.rva);
    }
    pe::WriteU64(image.bytes, image.DirectoryOffset(pe::DirectorySecurity), 0);
    image.directories[pe::DirectorySecurity] = {};
    result.certificateRemoved = true;
}

void Unbind(PeImage& image, ConversionResult& result) {
    if (image.directories[pe::DirectoryBoundImport].rva != 0) {
        pe::WriteU64(image.bytes, image.DirectoryOffset(pe::DirectoryBoundImport), 0);
        image.directories[pe::DirectoryBoundImport] = {};
        result.boundImportsRemoved = true;
    }
    for (pe::ImportModule& module : image.imports) {
        if (module.delayLoaded || module.timeDateStamp == 0) {
            continue;
        }
        if (module.lookupRva == 0) {
            throw PeError(std::format("{} is bound but has no import lookup table to restore its address table from", module.dll));
        }
        const auto lookup = image.RvaToOffset(module.lookupRva);
        const auto iat = image.RvaToOffset(module.iatRva);
        if (!lookup || !iat) {
            throw PeError(std::format("bound import tables of {} are not backed by file data", module.dll));
        }
        for (std::size_t i = 0;; ++i) {
            const std::uint64_t thunk = pe::ReadU64(image.bytes, *lookup + i * 8);
            pe::WriteU64(image.bytes, *iat + i * 8, thunk);
            if (thunk == 0) {
                break;
            }
        }
        pe::WriteU32(image.bytes, module.timeDateStampOffset, 0);
        pe::WriteU32(image.bytes, module.forwarderChainOffset, 0);
        module.timeDateStamp = 0;
        result.boundImportsRemoved = true;
    }
}

std::uint32_t FirstRawOffset(const PeImage& image) {
    std::uint32_t first = static_cast<std::uint32_t>(image.bytes.size());
    for (const pe::Section& section : image.sections) {
        if (section.rawSize != 0) {
            first = std::min(first, section.rawOffset);
        }
    }
    return first;
}

void AddNameSection(PeImage& image, const std::map<std::string, std::vector<std::uint32_t>>& renames) {
    const std::uint32_t headerEnd = image.sectionTableOffset + static_cast<std::uint32_t>(image.sections.size() + 1) * 40;
    if (headerEnd > image.sizeOfHeaders || headerEnd > FirstRawOffset(image)) {
        throw PeError(std::format("no room for another section header: the table would end at 0x{:x}, past the 0x{:x}-byte headers", headerEnd, std::min(image.sizeOfHeaders, FirstRawOffset(image))));
    }
    std::uint32_t virtualEnd = image.sizeOfHeaders;
    for (const pe::Section& section : image.sections) {
        virtualEnd = std::max(virtualEnd, section.virtualAddress + std::max(section.virtualSize, section.rawSize));
    }
    const std::uint32_t virtualAddress = pe::AlignUp(virtualEnd, image.sectionAlignment);

    std::vector<std::uint8_t> content;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> patches;
    for (const auto& [target, fields] : renames) {
        const std::uint32_t rva = virtualAddress + static_cast<std::uint32_t>(content.size());
        content.insert(content.end(), target.begin(), target.end());
        content.push_back(0);
        if (content.size() % 2 != 0) {
            content.push_back(0);
        }
        for (std::uint32_t field : fields) {
            patches.emplace_back(field, rva);
        }
    }
    const std::uint32_t virtualSize = static_cast<std::uint32_t>(content.size());
    const std::uint32_t rawSize = pe::AlignUp(virtualSize, image.fileAlignment);
    const std::uint32_t rawOffset = pe::AlignUp(static_cast<std::uint32_t>(image.bytes.size()), image.fileAlignment);
    content.resize(rawSize, 0);
    image.bytes.resize(rawOffset, 0);
    image.bytes.insert(image.bytes.end(), content.begin(), content.end());

    const std::uint32_t header = image.sectionTableOffset + static_cast<std::uint32_t>(image.sections.size()) * 40;
    std::fill(image.bytes.begin() + header, image.bytes.begin() + header + 40, std::uint8_t{0});
    std::memcpy(image.bytes.data() + header, AddedSectionName, std::strlen(AddedSectionName));
    pe::WriteU32(image.bytes, header + 8, virtualSize);
    pe::WriteU32(image.bytes, header + 12, virtualAddress);
    pe::WriteU32(image.bytes, header + 16, rawSize);
    pe::WriteU32(image.bytes, header + 20, rawOffset);
    pe::WriteU32(image.bytes, header + 36, SectionCharacteristics);

    pe::WriteU16(image.bytes, image.NumberOfSectionsOffset(), static_cast<std::uint16_t>(image.sections.size() + 1));
    image.sizeOfImage = pe::AlignUp(virtualAddress + virtualSize, image.sectionAlignment);
    pe::WriteU32(image.bytes, image.SizeOfImageOffset(), image.sizeOfImage);
    const std::uint32_t initialized = pe::ReadU32(image.bytes, image.SizeOfInitializedDataOffset());
    pe::WriteU32(image.bytes, image.SizeOfInitializedDataOffset(), initialized + rawSize);
    image.sections.push_back({AddedSectionName, virtualSize, virtualAddress, rawSize, rawOffset, SectionCharacteristics});

    for (const auto& [field, rva] : patches) {
        pe::WriteU32(image.bytes, field, rva);
    }
}

std::uint16_t ChooseSubsystem(std::uint16_t current, TargetSubsystem target) {
    switch (target) {
    case TargetSubsystem::Gui: return pe::SubsystemWindowsGui;
    case TargetSubsystem::Console: return pe::SubsystemWindowsCui;
    case TargetSubsystem::Keep: break;
    }
    if (current == pe::SubsystemConsole) {
        return pe::SubsystemWindowsGui;
    }
    if (current == pe::SubsystemWindowsGui || current == pe::SubsystemWindowsCui) {
        return current;
    }
    throw PeError(std::format("subsystem {} is neither console nor Windows; choose one with --gui or --console", pe::SubsystemName(current)));
}

}

ConversionResult Convert(std::vector<std::uint8_t> input, const ConversionOptions& options, const LibraryMap& map) {
    PeImage image = pe::Parse(std::move(input));
    if (image.machine != pe::MachineAmd64) {
        throw PeError(std::format("machine 0x{:x} is not AMD64 (0x8664)", image.machine));
    }
    if (image.directories[pe::DirectoryClr].rva != 0) {
        throw PeError("the image is a .NET assembly; AnyBox converts native GDK executables only");
    }

    ConversionResult result;
    result.dll = image.IsDll();
    result.subsystemBefore = image.subsystem;

    std::vector<std::string> unknown;
    std::string unpinned;
    std::map<std::string, std::vector<std::uint32_t>> renames;
    for (const pe::ImportModule& module : image.imports) {
        const Resolution resolution = map.Resolve(module.dll);
        ResolvedImport resolved{module.dll, resolution.target, resolution.library, resolution.kind, module.delayLoaded, module.functions};
        if (resolution.kind == ImportKind::Unknown) {
            unknown.push_back(module.dll);
        }
        if (resolution.kind == ImportKind::Library) {
            for (const pe::ImportFunction& function : module.functions) {
                if (function.ordinal && !map.OrdinalName(resolution.library, *function.ordinal)) {
                    unpinned += std::format("{}{} #{}", unpinned.empty() ? "" : ", ", module.dll, *function.ordinal);
                }
            }
        }
        if (resolution.kind == ImportKind::Library && ToLower(resolution.target) != ToLower(module.dll)) {
            renames[resolution.target].push_back(module.nameFieldOffset);
        }
        result.imports.push_back(std::move(resolved));
    }
    if (!unknown.empty() && !options.allowUnknown) {
        std::string list;
        for (const std::string& dll : unknown) {
            list += list.empty() ? dll : ", " + dll;
        }
        throw PeError(std::format("imports from DLLs that no AnyBox library, bundled module or host DLL provides: {} (add a library under core/libs/xdll, pass the title's module folder with --module-dir, or --allow-unknown to keep the names)", list));
    }

    if (!unpinned.empty()) {
        throw PeError(std::format("imports by ordinal that no AnyBox library exports at that ordinal: {} (pin the export as '<Name> @<ordinal>' in the library's Ordinals.txt)", unpinned));
    }

    StripCertificate(image, result);
    Unbind(image, result);
    if (!renames.empty()) {
        AddNameSection(image, renames);
        result.sectionAdded = true;
    }

    result.subsystemAfter = ChooseSubsystem(image.subsystem, options.subsystem);
    pe::WriteU16(image.bytes, image.SubsystemOffset(), result.subsystemAfter);

    result.checksum = pe::ComputeChecksum(image.bytes, image.ChecksumOffset());
    pe::WriteU32(image.bytes, image.ChecksumOffset(), result.checksum);
    result.bytes = std::move(image.bytes);
    return result;
}

}
