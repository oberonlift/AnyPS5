#include <lifter/Program.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string_view>

namespace Ps5Lift {

namespace {

constexpr std::string_view Origin = "$ORIGIN/";
constexpr std::uint64_t ImageAlignment = 0x1000000;

std::optional<std::filesystem::path> GuestModulePath(const std::string& image, const std::string& needed) {
    if (!needed.starts_with(Origin)) return std::nullopt;
    return std::filesystem::weakly_canonical(std::filesystem::path(image).parent_path() / needed.substr(Origin.size()));
}

}

GuestProgram GuestProgram::Load(const std::string& executable) {
    GuestProgram program;
    program.Images.push_back(GuestImage::Read(executable, std::nullopt));
    std::map<std::filesystem::path, std::size_t> loaded;
    std::vector<std::vector<std::size_t>> dependencies(1);
    for (std::size_t index = 0; index < program.Images.size(); ++index) {
        const auto path = program.Images[index].Path();
        const auto needed = program.Images[index].Needed();
        for (const auto& name : needed) {
            const auto module = GuestModulePath(path, name);
            if (!module) continue;
            const auto [found, inserted] = loaded.emplace(*module, program.Images.size());
            if (inserted) {
                const auto base = (program.Images.back().End() + ImageAlignment - 1) & ~(ImageAlignment - 1);
                program.Images.push_back(GuestImage::Read(module->string(), base));
                dependencies.emplace_back();
            }
            dependencies[index].push_back(found->second);
        }
    }

    const auto resolve = [&](const std::string& name) -> std::optional<std::uint64_t> {
        for (const auto& image : program.Images) {
            if (const auto address = image.Export(name)) return address;
        }
        return std::nullopt;
    };
    std::uint64_t tlsOffset = 0;
    for (std::size_t index = 0; index < program.Images.size(); ++index) {
        auto& image = program.Images[index];
        const auto& tls = image.Tls();
        if (tls.MemorySize == 0) continue;
        tlsOffset = (tlsOffset + tls.MemorySize + tls.Alignment - 1) / tls.Alignment * tls.Alignment;
        image.PlaceTls(index + 1, tlsOffset);
    }
    for (auto& image : program.Images) image.Relocate(resolve);

    std::vector<bool> visited(program.Images.size());
    const std::function<void(std::size_t)> visit = [&](const std::size_t index) {
        visited[index] = true;
        for (const auto dependency : dependencies[index]) {
            if (!visited[dependency]) visit(dependency);
        }
        if (index != 0) program.Initialization.push_back(index);
    };
    visit(0);
    return program;
}

}
