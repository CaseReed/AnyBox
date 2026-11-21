#include "relinker/Conversion.hpp"
#include "relinker/LibraryMap.hpp"
#include "relinker/Registry.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace anybox;

namespace {

constexpr char RuntimeDll[] = "ab_runtime.dll";

struct Arguments {
    fs::path input;
    fs::path output;
    relinker::ConversionOptions options;
    std::vector<fs::path> moduleDirs;
    std::optional<fs::path> libs;
    bool registry = false;
    bool listImports = false;
    bool printMap = false;
};

void Usage() {
    std::cout <<
        "Usage: anybox-relinker [options] <input.exe> <output.exe>\n"
        "\n"
        "Converts a decrypted GDK console PE32+ executable into a\n"
        "Windows program that loads AnyBox's reimplemented system libraries.\n"
        "\n"
        "  --gui                 Windows GUI subsystem (default for console images)\n"
        "  --console             Windows console subsystem\n"
        "  --module-dir <dir>    Convert every .dll under <dir> (relative to the input's\n"
        "                        folder) into the same place beside the output; repeatable\n"
        "  --libs <dir>          Copy the AnyBox libraries the output needs from <dir>\n"
        "  --registry            Write <output>.registry.json for each converted file\n"
        "  --allow-unknown       Keep imports from DLLs nothing provides instead of failing\n"
        "  --list-imports        Print the input's imports and their resolution, write nothing\n"
        "  --print-map           Print the DLL routing table and exit\n";
}

std::optional<Arguments> ParseArguments(int argc, char** argv) {
    Arguments args;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::format("{} needs a value", arg));
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            Usage();
            return std::nullopt;
        } else if (arg == "--gui") {
            args.options.subsystem = relinker::TargetSubsystem::Gui;
        } else if (arg == "--console") {
            args.options.subsystem = relinker::TargetSubsystem::Console;
        } else if (arg == "--module-dir") {
            args.moduleDirs.emplace_back(value());
        } else if (arg == "--libs") {
            args.libs = fs::path(value());
        } else if (arg == "--registry") {
            args.registry = true;
        } else if (arg == "--allow-unknown") {
            args.options.allowUnknown = true;
        } else if (arg == "--list-imports") {
            args.listImports = true;
        } else if (arg == "--print-map") {
            args.printMap = true;
        } else if (arg.starts_with("--")) {
            throw std::runtime_error(std::format("unknown option {}", arg));
        } else {
            positional.push_back(arg);
        }
    }
    if (args.printMap) {
        return args;
    }
    if (positional.size() != (args.listImports ? 1u : 2u)) {
        Usage();
        throw std::runtime_error(args.listImports ? "--list-imports takes one input file" : "expected an input and an output file");
    }
    args.input = positional[0];
    if (!args.listImports) {
        args.output = positional[1];
    }
    return args;
}

std::vector<std::uint8_t> ReadFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error(std::format("cannot open {}", path.string()));
    }
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void WriteFile(const fs::path& path, const void* data, std::size_t size) {
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path());
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!stream) {
        throw std::runtime_error(std::format("cannot write {}", path.string()));
    }
}

bool IsDllFile(const fs::path& path) {
    return relinker::ToLower(path.extension().string()) == ".dll";
}

std::vector<fs::path> ModuleFiles(const fs::path& root) {
    std::vector<fs::path> files;
    if (!fs::is_directory(root)) {
        throw std::runtime_error(std::format("module folder {} does not exist", root.string()));
    }
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && IsDllFile(entry.path())) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

void PrintMap(const relinker::LibraryMap& map) {
    for (const auto& route : map.Routes()) {
        std::cout << std::format("{:<32} -> {:<28} ({})\n", route.pattern, route.hostDll, route.library);
    }
    for (const auto& pattern : map.HostPatterns()) {
        std::cout << std::format("{:<32} -> host\n", pattern);
    }
}

void PrintImports(const relinker::ConversionResult& result) {
    for (const auto& module : result.imports) {
        std::cout << std::format("{}{} [{}]{}\n", module.dll, module.delayLoaded ? " (delay)" : "", relinker::KindName(module.kind),
            module.kind == relinker::ImportKind::Library ? " -> " + module.target : "");
        for (const auto& function : module.functions) {
            std::cout << "    " << (function.ordinal ? std::format("#{}", *function.ordinal) : function.name) << '\n';
        }
    }
}

int Run(const Arguments& args) {
    relinker::LibraryMap map = relinker::LibraryMap::Builtin();
    if (args.printMap) {
        PrintMap(map);
        return 0;
    }

    const fs::path inputDir = args.input.has_parent_path() ? args.input.parent_path() : fs::path(".");
    std::vector<std::pair<fs::path, fs::path>> modules;
    std::vector<std::string> bundled;
    for (const auto& entry : fs::directory_iterator(inputDir)) {
        if (entry.is_regular_file() && IsDllFile(entry.path())) {
            bundled.push_back(entry.path().filename().string());
        }
    }
    std::set<fs::path> seen;
    for (const fs::path& dir : args.moduleDirs) {
        for (const fs::path& file : ModuleFiles(inputDir / dir)) {
            if (seen.insert(fs::weakly_canonical(file)).second) {
                modules.emplace_back(file, fs::relative(file, inputDir));
                bundled.push_back(file.filename().string());
            }
        }
    }
    map.SetBundled(bundled);

    if (args.listImports) {
        relinker::ConversionOptions options = args.options;
        options.allowUnknown = true;
        PrintImports(relinker::Convert(ReadFile(args.input), options, map));
        return 0;
    }

    const fs::path outputDir = args.output.has_parent_path() ? args.output.parent_path() : fs::path(".");
    std::set<std::string> libraries;
    std::vector<std::string> written;

    const auto convertOne = [&](const fs::path& in, const fs::path& out, relinker::ConversionOptions options) {
        relinker::ConversionResult result = relinker::Convert(ReadFile(in), options, map);
        WriteFile(out, result.bytes.data(), result.bytes.size());
        if (args.registry) {
            const std::string json = relinker::RegistryJson(result, in.generic_string(), out.generic_string());
            WriteFile(fs::path(out.string() + ".registry.json"), json.data(), json.size());
        }
        for (const auto& module : result.imports) {
            if (module.kind == relinker::ImportKind::Library) {
                libraries.insert(module.target);
            }
            if (module.kind == relinker::ImportKind::Unknown) {
                std::cerr << std::format("warning: {} imports {} from {}, which nothing provides\n", in.filename().string(), module.functions.size(), module.dll);
            }
        }
        std::cout << std::format("converted {} ({} -> {}{}{})\n", in.generic_string(), pe::SubsystemName(result.subsystemBefore),
            pe::SubsystemName(result.subsystemAfter), result.certificateRemoved ? ", certificate removed" : "",
            result.boundImportsRemoved ? ", unbound" : "");
        written.push_back(fs::relative(out, outputDir).generic_string());
    };

    convertOne(args.input, args.output, args.options);
    for (const auto& [file, relative] : modules) {
        relinker::ConversionOptions options = args.options;
        options.subsystem = relinker::TargetSubsystem::Keep;
        convertOne(file, outputDir / relative, options);
    }

    std::vector<std::string> libraryFiles(libraries.begin(), libraries.end());
    if (!libraryFiles.empty()) {
        libraryFiles.push_back(RuntimeDll);
    }
    if (args.libs) {
        for (const std::string& library : libraryFiles) {
            const fs::path source = *args.libs / library;
            if (!fs::exists(source)) {
                throw std::runtime_error(std::format("{} is not in {}; build the libs target first", library, args.libs->string()));
            }
            fs::copy_file(source, outputDir / library, fs::copy_options::overwrite_existing);
        }
    }

    std::cout << "\nArrange the files like this:\n" << outputDir.generic_string() << "/\n";
    for (const std::string& file : written) {
        std::cout << "  " << file << '\n';
    }
    for (const std::string& library : libraryFiles) {
        std::cout << "  " << library << (args.libs ? "" : "   (from build/libs)") << '\n';
    }
    std::cout << "  ...the rest of the title's package (MicrosoftGame.config, data)\n";
    return 0;
}

}

int main(int argc, char** argv) {
    try {
        const auto args = ParseArguments(argc, argv);
        if (!args) {
            return 0;
        }
        return Run(*args);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
