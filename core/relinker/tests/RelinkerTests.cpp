#include "SyntheticPe.hpp"

#include "relinker/Conversion.hpp"
#include "relinker/Registry.hpp"

#include <functional>
#include <iostream>
#include <map>
#include <string>

using namespace anybox;

namespace {

struct Failure {
    std::string message;
};

void Check(bool condition, const std::string& message) {
    if (!condition) {
        throw Failure{message};
    }
}

relinker::LibraryMap TestMap() {
    return relinker::LibraryMap(
        {{"xgameruntime.dll", "ab_xgameruntime.dll", "xgameruntime"}, {"d3d12_x*.dll", "ab_d3d12_x.dll", "d3d12_x"}},
        {"kernel32.dll", "api-ms-win-*"},
        {{"xgameruntime", 4, "XGameRuntimeInitialize"}});
}

test::SyntheticPe Title() {
    test::SyntheticPe spec;
    spec.imports.push_back({"XGameRuntime.dll", {"XGameRuntimeInitialize", "XTaskQueueCreate"}, {}, false, false});
    spec.imports.push_back({"kernel32.dll", {"Sleep"}, {}, false, false});
    return spec;
}

relinker::ConversionResult ConvertSpec(const test::SyntheticPe& spec, relinker::ConversionOptions options = {}, relinker::LibraryMap map = TestMap()) {
    return relinker::Convert(test::Build(spec), options, map);
}

std::string ExpectError(const std::function<void()>& action) {
    try {
        action();
    } catch (const pe::PeError& error) {
        return error.what();
    }
    throw Failure{"expected a PeError"};
}

void TestRename() {
    const auto result = ConvertSpec(Title());
    const pe::PeImage image = pe::Parse(result.bytes);
    Check(image.imports.size() == 2, "two import descriptors survive");
    Check(image.imports[0].dll == "ab_xgameruntime.dll", "xgameruntime is routed to ab_xgameruntime.dll, got " + image.imports[0].dll);
    Check(image.imports[0].functions.size() == 2 && image.imports[0].functions[1].name == "XTaskQueueCreate", "function names are unchanged");
    Check(image.imports[1].dll == "kernel32.dll", "host DLLs keep their name");
    Check(image.sections.size() == 2 && image.sections[1].name == relinker::AddedSectionName, "the name section is appended");
    Check(image.sizeOfImage == pe::AlignUp(image.sections[1].virtualAddress + image.sections[1].virtualSize, image.sectionAlignment), "SizeOfImage covers the new section");
    Check(result.imports[0].kind == relinker::ImportKind::Library && result.imports[0].library == "xgameruntime", "result reports the library");
}

void TestSubsystem() {
    const auto result = ConvertSpec(Title());
    Check(result.subsystemBefore == pe::SubsystemConsole, "input is a console image");
    Check(pe::Parse(result.bytes).subsystem == pe::SubsystemWindowsGui, "console subsystem becomes Windows GUI");
    auto spec = Title();
    spec.subsystem = 1;
    Check(ExpectError([&] { ConvertSpec(spec); }).find("neither console nor Windows") != std::string::npos, "native subsystem needs an explicit choice");
}

void TestConsole() {
    relinker::ConversionOptions options;
    options.subsystem = relinker::TargetSubsystem::Console;
    Check(pe::Parse(ConvertSpec(Title(), options).bytes).subsystem == pe::SubsystemWindowsCui, "--console selects the console subsystem");
}

void TestCertificate() {
    auto spec = Title();
    spec.certificateBytes = 0x100;
    const auto input = test::Build(spec);
    const auto result = relinker::Convert(input, {}, TestMap());
    const pe::PeImage image = pe::Parse(result.bytes);
    Check(result.certificateRemoved, "certificate removal is reported");
    Check(image.directories[pe::DirectorySecurity].rva == 0 && image.directories[pe::DirectorySecurity].size == 0, "security directory is cleared");
    Check(image.sections[1].rawOffset + image.sections[1].rawSize == result.bytes.size(), "the new section replaces the certificate at the end of the file");
}

void TestBound() {
    auto spec = Title();
    spec.imports[0].bound = true;
    const auto result = ConvertSpec(spec);
    const pe::PeImage image = pe::Parse(result.bytes);
    Check(result.boundImportsRemoved, "unbinding is reported");
    Check(image.directories[pe::DirectoryBoundImport].rva == 0, "bound import directory is cleared");
    Check(image.imports[0].timeDateStamp == 0, "descriptor time stamp is cleared");
    const auto lookup = *image.RvaToOffset(image.imports[0].lookupRva);
    const auto iat = *image.RvaToOffset(image.imports[0].iatRva);
    for (std::size_t i = 0; i < 3; ++i) {
        Check(pe::ReadU64(image.bytes, lookup + i * 8) == pe::ReadU64(image.bytes, iat + i * 8), "address table is restored from the lookup table");
    }
}

void TestDelay() {
    auto spec = Title();
    spec.imports.push_back({"d3d12_xs.dll", {"D3D12XboxCreateDevice"}, {}, true, false});
    const pe::PeImage image = pe::Parse(ConvertSpec(spec).bytes);
    Check(image.imports.size() == 3, "delay descriptor is read");
    Check(image.imports[2].delayLoaded && image.imports[2].dll == "ab_d3d12_x.dll", "delay-loaded DLLs are routed by pattern, got " + image.imports[2].dll);
}

void TestUnknown() {
    auto spec = Title();
    spec.imports.push_back({"xmystery.dll", {"Thing"}, {7}, false, false});
    const std::string message = ExpectError([&] { ConvertSpec(spec); });
    Check(message.find("xmystery.dll") != std::string::npos, "the error names the DLL: " + message);
    relinker::ConversionOptions options;
    options.allowUnknown = true;
    const auto result = ConvertSpec(spec, options);
    const pe::PeImage image = pe::Parse(result.bytes);
    Check(image.imports[2].dll == "xmystery.dll" && result.imports[2].kind == relinker::ImportKind::Unknown, "--allow-unknown keeps the name");
    Check(image.imports[2].functions[1].ordinal == 7, "ordinal imports are read");
}

void TestHost() {
    test::SyntheticPe spec;
    spec.imports.push_back({"api-ms-win-core-synch-l1-2-0.dll", {"WaitOnAddress"}, {}, false, false});
    const auto result = ConvertSpec(spec);
    Check(result.imports[0].kind == relinker::ImportKind::Host, "API sets resolve to the host");
    Check(!result.sectionAdded && pe::Parse(result.bytes).sections.size() == 1, "nothing to rename adds no section");
}

void TestBundled() {
    auto map = TestMap();
    map.SetBundled({"XGAMERUNTIME.DLL"});
    const auto result = ConvertSpec(Title(), {}, map);
    Check(result.imports[0].kind == relinker::ImportKind::Bundled, "a module the title ships wins over the AnyBox library");
    Check(pe::Parse(result.bytes).imports[0].dll == "XGameRuntime.dll", "bundled names are kept");
}

void TestHeaderRoom() {
    auto spec = Title();
    spec.sizeOfHeaders = 0x1c0;
    Check(ExpectError([&] { ConvertSpec(spec); }).find("no room for another section header") != std::string::npos, "full headers are reported");
}

void TestChecksum() {
    const auto result = ConvertSpec(Title());
    const pe::PeImage image = pe::Parse(result.bytes);
    Check(image.checksum == pe::ComputeChecksum(result.bytes, image.ChecksumOffset()), "checksum matches the written file");
    Check(image.checksum != 0, "checksum is written");
}

void TestNotAmd64() {
    auto spec = Title();
    spec.machine = 0xaa64;
    Check(ExpectError([&] { ConvertSpec(spec); }).find("not AMD64") != std::string::npos, "ARM64 images are refused");
}

void TestRegistry() {
    auto spec = Title();
    spec.imports.push_back({"xmystery.dll", {"Quote\"d"}, {3}, false, false});
    relinker::ConversionOptions options;
    options.allowUnknown = true;
    const std::string json = relinker::RegistryJson(ConvertSpec(spec, options), "in\\title.exe", "out/title.exe");
    Check(json.find("\"format\": \"anybox-registry\"") != std::string::npos, "registry has its format tag");
    Check(json.find("\"input\": \"in\\\\title.exe\"") != std::string::npos, "backslashes are escaped");
    Check(json.find("\"name\": \"Quote\\\"d\"") != std::string::npos, "quotes are escaped");
    Check(json.find("\"ordinal\": 3") != std::string::npos, "ordinals are listed");
    Check(json.find("\"kind\": \"unknown\"") != std::string::npos, "unknown DLLs are listed");
    Check(json.find("\"before\": \"console\", \"after\": \"windows-gui\"") != std::string::npos, "subsystem change is listed");
}

void TestOrdinal() {
    test::SyntheticPe spec;
    spec.imports.push_back({"XGameRuntime.dll", {"XTaskQueueCreate"}, {4}, false, false});
    spec.imports.push_back({"xmystery.dll", {}, {9}, false, false});
    relinker::ConversionOptions options;
    options.allowUnknown = true;
    const auto result = ConvertSpec(spec, options);
    const pe::PeImage image = pe::Parse(result.bytes);
    Check(image.imports[0].dll == "ab_xgameruntime.dll" && image.imports[0].functions[1].ordinal == 4, "a pinned ordinal import is routed and stays an ordinal import");
    spec.imports[0].ordinals.push_back(5);
    const std::string message = ExpectError([&] { ConvertSpec(spec, options); });
    Check(message.find("XGameRuntime.dll #5") != std::string::npos, "the error names the unpinned ordinal: " + message);
    Check(message.find("#4") == std::string::npos && message.find("#9") == std::string::npos, "pinned and unrouted ordinals are not listed: " + message);
}

void TestExports() {
    test::SyntheticPe spec;
    spec.dll = true;
    spec.subsystem = pe::SubsystemWindowsGui;
    spec.exports = {{"Mine", std::nullopt}, {"Tick", std::string("kernel32.GetTickCount64")}};
    const pe::PeImage image = pe::Parse(test::Build(spec));
    Check(image.IsDll(), "DLL flag is read");
    Check(image.exports.size() == 2 && image.exports[0].name == "Mine" && !image.exports[0].forwarder, "plain export is read");
    Check(image.exports[1].forwarder == "kernel32.GetTickCount64", "forwarder is read");
}

}

int main(int argc, char** argv) {
    const std::map<std::string, void (*)()> tests = {
        {"rename", TestRename}, {"subsystem", TestSubsystem}, {"console", TestConsole}, {"certificate", TestCertificate},
        {"bound", TestBound}, {"delay", TestDelay}, {"unknown", TestUnknown}, {"host", TestHost}, {"bundled", TestBundled},
        {"header-room", TestHeaderRoom}, {"checksum", TestChecksum}, {"not-amd64", TestNotAmd64}, {"registry", TestRegistry},
        {"exports", TestExports}, {"ordinal", TestOrdinal},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        if (argc > 1 && name != argv[1]) {
            continue;
        }
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const Failure& failure) {
            std::cout << "FAIL " << name << ": " << failure.message << '\n';
            ++failures;
        } catch (const std::exception& error) {
            std::cout << "FAIL " << name << ": unexpected exception: " << error.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
