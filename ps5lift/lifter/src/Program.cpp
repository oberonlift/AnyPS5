#include <lifter/Program.hpp>

#include <llvm/IR/LLVMContext.h>
#include <llvm/Object/ArchiveWriter.h>
#include <llvm/Support/Error.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace Ps5Lift {

namespace {

constexpr std::size_t InstructionsPerPart = 50000;

std::size_t Weight(const GuestFunction& function) {
    return std::max<std::size_t>(1, function.Instructions.size());
}

std::vector<std::vector<std::uint64_t>> Partition(const std::map<std::uint64_t, GuestFunction>& functions) {
    std::size_t total = 0;
    for (const auto& [address, function] : functions) total += Weight(function);
    const auto count = std::max<std::size_t>(1, (total + InstructionsPerPart - 1) / InstructionsPerPart);
    const auto target = (total + count - 1) / count;
    std::vector<std::vector<std::uint64_t>> parts(1);
    std::size_t weight = 0;
    for (const auto& [address, function] : functions) {
        if (weight >= target && parts.size() < count) {
            parts.emplace_back();
            weight = 0;
        }
        parts.back().push_back(address);
        weight += Weight(function);
    }
    return parts;
}

void WriteArchive(const std::string& path, const std::vector<llvm::SmallVector<char, 0>>& objects) {
    std::vector<std::string> names;
    for (std::size_t i = 0; i < objects.size(); ++i) names.push_back("part" + std::to_string(i) + ".o");
    std::vector<llvm::NewArchiveMember> members;
    for (std::size_t i = 0; i < objects.size(); ++i) members.emplace_back(llvm::MemoryBufferRef(llvm::StringRef(objects[i].data(), objects[i].size()), names[i]));
    if (auto error = llvm::writeArchive(path, members, llvm::SymtabWritingMode::NormalSymtab, llvm::object::Archive::K_GNU, true, false))
        throw std::runtime_error("Cannot write " + path + ": " + llvm::toString(std::move(error)));
}

}

LiftReport CompileProgram(const GuestImage& image, const std::map<std::uint64_t, GuestFunction>& functions, const LiftOptions& lift, const CodegenOptions& codegen, const ProgramOptions& program) {
    const auto parts = Partition(functions);
    const auto tasks = parts.size() + 1;
    std::vector<llvm::SmallVector<char, 0>> objects(tasks);
    std::vector<LiftReport> reports(tasks);
    std::atomic<std::size_t> next = 0;
    std::atomic<bool> failed = false;
    std::exception_ptr failure;
    std::mutex failureLock;
    const auto work = [&] {
        for (auto task = next++; task < tasks && !failed; task = next++) {
            try {
                llvm::LLVMContext context;
                auto module = task < parts.size() ? LiftFunctions(context, image, functions, parts[task], lift, reports[task]) : LiftTables(context, image, functions, lift);
                Optimize(*module, codegen);
                if (!program.IrPrefix.empty()) WriteIr(*module, program.IrPrefix + "." + std::to_string(task) + ".ll");
                if (!program.Archive.empty()) objects[task] = CompileObject(*module, codegen);
            } catch (...) {
                const std::lock_guard lock(failureLock);
                if (!failure) failure = std::current_exception();
                failed = true;
            }
        }
    };
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < std::min<std::size_t>(program.Jobs, tasks); ++i) threads.emplace_back(work);
    for (auto& thread : threads) thread.join();
    if (failure) std::rethrow_exception(failure);
    if (!program.Archive.empty()) WriteArchive(program.Archive, objects);

    LiftReport report;
    for (const auto& part : reports) {
        report.Functions += part.Functions;
        report.Instructions += part.Instructions;
        for (const auto& [mnemonic, count] : part.Unsupported) report.Unsupported[mnemonic] += count;
    }
    return report;
}

}
