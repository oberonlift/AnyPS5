#include <lifter/Lifter.hpp>

#include <ps5lift/State.h>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <cstddef>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

namespace Ps5Lift {

namespace {

enum class StringOperation : std::uint32_t {
    Movs = 0,
    Stos = 1
};

struct UnsupportedInstruction : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string Hex(const std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << value;
    return stream.str();
}

std::string GuestSymbol(const std::uint64_t address) {
    return "guest_" + Hex(address);
}

int GprIndex(const ZydisRegister reg) {
    const auto enclosing = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);
    if (enclosing < ZYDIS_REGISTER_RAX || enclosing > ZYDIS_REGISTER_R15) return -1;
    return enclosing - ZYDIS_REGISTER_RAX;
}

int VectorIndex(const ZydisRegister reg) {
    if (reg >= ZYDIS_REGISTER_XMM0 && reg <= ZYDIS_REGISTER_XMM15) return reg - ZYDIS_REGISTER_XMM0;
    if (reg >= ZYDIS_REGISTER_YMM0 && reg <= ZYDIS_REGISTER_YMM15) return reg - ZYDIS_REGISTER_YMM0;
    return -1;
}

bool IsHighByte(const ZydisRegister reg) {
    return reg == ZYDIS_REGISTER_AH || reg == ZYDIS_REGISTER_CH || reg == ZYDIS_REGISTER_DH || reg == ZYDIS_REGISTER_BH;
}

enum class Condition {
    O, NO, B, AE, E, NE, BE, A, S, NS, P, NP, L, GE, LE, G
};

Condition ConditionOf(const ZydisMnemonic mnemonic) {
    switch (mnemonic) {
    case ZYDIS_MNEMONIC_JO: case ZYDIS_MNEMONIC_CMOVO: case ZYDIS_MNEMONIC_SETO: return Condition::O;
    case ZYDIS_MNEMONIC_JNO: case ZYDIS_MNEMONIC_CMOVNO: case ZYDIS_MNEMONIC_SETNO: return Condition::NO;
    case ZYDIS_MNEMONIC_JB: case ZYDIS_MNEMONIC_CMOVB: case ZYDIS_MNEMONIC_SETB: return Condition::B;
    case ZYDIS_MNEMONIC_JNB: case ZYDIS_MNEMONIC_CMOVNB: case ZYDIS_MNEMONIC_SETNB: return Condition::AE;
    case ZYDIS_MNEMONIC_JZ: case ZYDIS_MNEMONIC_CMOVZ: case ZYDIS_MNEMONIC_SETZ: return Condition::E;
    case ZYDIS_MNEMONIC_JNZ: case ZYDIS_MNEMONIC_CMOVNZ: case ZYDIS_MNEMONIC_SETNZ: return Condition::NE;
    case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_CMOVBE: case ZYDIS_MNEMONIC_SETBE: return Condition::BE;
    case ZYDIS_MNEMONIC_JNBE: case ZYDIS_MNEMONIC_CMOVNBE: case ZYDIS_MNEMONIC_SETNBE: return Condition::A;
    case ZYDIS_MNEMONIC_JS: case ZYDIS_MNEMONIC_CMOVS: case ZYDIS_MNEMONIC_SETS: return Condition::S;
    case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_CMOVNS: case ZYDIS_MNEMONIC_SETNS: return Condition::NS;
    case ZYDIS_MNEMONIC_JP: case ZYDIS_MNEMONIC_CMOVP: case ZYDIS_MNEMONIC_SETP: return Condition::P;
    case ZYDIS_MNEMONIC_JNP: case ZYDIS_MNEMONIC_CMOVNP: case ZYDIS_MNEMONIC_SETNP: return Condition::NP;
    case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_CMOVL: case ZYDIS_MNEMONIC_SETL: return Condition::L;
    case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_CMOVNL: case ZYDIS_MNEMONIC_SETNL: return Condition::GE;
    case ZYDIS_MNEMONIC_JLE: case ZYDIS_MNEMONIC_CMOVLE: case ZYDIS_MNEMONIC_SETLE: return Condition::LE;
    case ZYDIS_MNEMONIC_JNLE: case ZYDIS_MNEMONIC_CMOVNLE: case ZYDIS_MNEMONIC_SETNLE: return Condition::G;
    default: throw std::logic_error("Mnemonic has no condition code");
    }
}

bool IsCmov(const ZydisMnemonic mnemonic) {
    return mnemonic >= ZYDIS_MNEMONIC_CMOVB && mnemonic <= ZYDIS_MNEMONIC_CMOVZ;
}

bool IsSetcc(const ZydisMnemonic mnemonic) {
    switch (mnemonic) {
    case ZYDIS_MNEMONIC_SETO: case ZYDIS_MNEMONIC_SETNO: case ZYDIS_MNEMONIC_SETB: case ZYDIS_MNEMONIC_SETNB:
    case ZYDIS_MNEMONIC_SETZ: case ZYDIS_MNEMONIC_SETNZ: case ZYDIS_MNEMONIC_SETBE: case ZYDIS_MNEMONIC_SETNBE:
    case ZYDIS_MNEMONIC_SETS: case ZYDIS_MNEMONIC_SETNS: case ZYDIS_MNEMONIC_SETP: case ZYDIS_MNEMONIC_SETNP:
    case ZYDIS_MNEMONIC_SETL: case ZYDIS_MNEMONIC_SETNL: case ZYDIS_MNEMONIC_SETLE: case ZYDIS_MNEMONIC_SETNLE:
        return true;
    default:
        return false;
    }
}

class ModuleLifter {
public:
    ModuleLifter(llvm::LLVMContext& context, const GuestImage& image, const std::map<std::uint64_t, GuestFunction>& functions, const LiftOptions& options, LiftReport& report)
        : _context(context), _image(image), _functions(functions), _options(options), _report(report), _module(std::make_unique<llvm::Module>("ps5lift", context)) {}

    std::unique_ptr<llvm::Module> Lift() {
        llvm::MDBuilder metadata(_context);
        auto* root = metadata.createTBAARoot("ps5lift");
        auto* stateType = metadata.createTBAAScalarTypeNode("state", root);
        auto* guestType = metadata.createTBAAScalarTypeNode("guest", root);
        _stateTag = metadata.createTBAAStructTagNode(stateType, stateType, 0);
        _guestTag = metadata.createTBAAStructTagNode(guestType, guestType, 0);
        _pointer = llvm::PointerType::get(_context, 0);
        _guestType = llvm::FunctionType::get(llvm::Type::getVoidTy(_context), {_pointer}, false);
        for (const auto& [address, function] : _functions) {
            auto* host = llvm::Function::Create(_guestType, llvm::GlobalValue::InternalLinkage, GuestSymbol(address), *_module);
            host->addParamAttr(0, llvm::Attribute::NoAlias);
            host->addParamAttr(0, llvm::Attribute::NoUndef);
        }
        for (const auto& [address, function] : _functions) {
            if (function.Import) LiftImport(function);
            else LiftFunction(function);
            ++_report.Functions;
        }
        EmitImage();
        std::string errors;
        llvm::raw_string_ostream stream(errors);
        if (llvm::verifyModule(*_module, &stream)) throw std::runtime_error("Lifted module does not verify: " + stream.str());
        return std::move(_module);
    }

    llvm::Function* Guest(const std::uint64_t address) const {
        auto* function = _module->getFunction(GuestSymbol(address));
        if (function == nullptr) throw std::runtime_error("Call to undiscovered guest function 0x" + Hex(address));
        return function;
    }

    llvm::FunctionCallee Runtime(const std::string& name, llvm::FunctionType* type) {
        return _module->getOrInsertFunction(name, type);
    }

    llvm::LLVMContext& Context() { return _context; }
    llvm::MDNode* StateTag() const { return _stateTag; }
    llvm::MDNode* GuestTag() const { return _guestTag; }
    llvm::PointerType* Pointer() const { return _pointer; }
    llvm::FunctionType* GuestType() const { return _guestType; }
    const LiftOptions& Options() const { return _options; }
    LiftReport& Report() { return _report; }
    const std::map<std::uint64_t, GuestFunction>& Functions() const { return _functions; }
    const GuestImage& Image() const { return _image; }

private:
    llvm::LLVMContext& _context;
    const GuestImage& _image;
    const std::map<std::uint64_t, GuestFunction>& _functions;
    const LiftOptions& _options;
    LiftReport& _report;
    std::unique_ptr<llvm::Module> _module;
    llvm::PointerType* _pointer = nullptr;
    llvm::FunctionType* _guestType = nullptr;
    llvm::MDNode* _stateTag = nullptr;
    llvm::MDNode* _guestTag = nullptr;

    void LiftFunction(const GuestFunction& function);

    void LiftImport(const GuestFunction& function) {
        auto* host = Guest(function.Entry);
        llvm::IRBuilder<> builder(llvm::BasicBlock::Create(_context, "entry", host));
        auto* state = host->getArg(0);
        builder.CreateCall(Runtime(BridgeSymbol(*function.Import), _guestType), {state});
        auto* rsp = builder.CreateConstGEP1_64(builder.getInt8Ty(), state, offsetof(Ps5LiftState, Gpr) + 8 * PS5LIFT_RSP);
        auto* load = builder.CreateLoad(builder.getInt64Ty(), rsp);
        load->setMetadata(llvm::LLVMContext::MD_tbaa, _stateTag);
        builder.CreateStore(builder.CreateAdd(load, builder.getInt64(8)), rsp)->setMetadata(llvm::LLVMContext::MD_tbaa, _stateTag);
        builder.CreateRetVoid();
    }

    void EmitImage() {
        auto* i64 = llvm::Type::getInt64Ty(_context);
        auto* i32 = llvm::Type::getInt32Ty(_context);
        auto* segmentType = llvm::StructType::create(_context, {i64, i64, i64, _pointer, i32}, "Ps5LiftSegment");
        auto* functionType = llvm::StructType::create(_context, {i64, _pointer}, "Ps5LiftFunctionEntry");
        auto* imageType = llvm::StructType::create(_context, {_pointer, i64, _pointer, i64, i64, i64, i64, i64, i64}, "Ps5LiftImage");

        std::vector<llvm::Constant*> segments;
        for (const auto& segment : _image.Segments()) {
            auto* data = llvm::ConstantDataArray::get(_context, llvm::ArrayRef<std::uint8_t>(segment.Bytes));
            auto* global = new llvm::GlobalVariable(*_module, data->getType(), true, llvm::GlobalValue::PrivateLinkage, data, "segment_" + Hex(segment.Address));
            global->setAlignment(llvm::Align(16));
            segments.push_back(llvm::ConstantStruct::get(segmentType, {llvm::ConstantInt::get(i64, segment.Address), llvm::ConstantInt::get(i64, segment.MemorySize), llvm::ConstantInt::get(i64, segment.Bytes.size()), global, llvm::ConstantInt::get(i32, segment.Flags)}));
        }
        std::vector<llvm::Constant*> entries;
        for (const auto& [address, function] : _functions) entries.push_back(llvm::ConstantStruct::get(functionType, {llvm::ConstantInt::get(i64, address), Guest(address)}));

        auto* segmentArray = llvm::ConstantArray::get(llvm::ArrayType::get(segmentType, segments.size()), segments);
        auto* segmentGlobal = new llvm::GlobalVariable(*_module, segmentArray->getType(), true, llvm::GlobalValue::PrivateLinkage, segmentArray, "ps5lift_segments");
        auto* functionArray = llvm::ConstantArray::get(llvm::ArrayType::get(functionType, entries.size()), entries);
        auto* functionGlobal = new llvm::GlobalVariable(*_module, functionArray->getType(), true, llvm::GlobalValue::PrivateLinkage, functionArray, "ps5lift_functions");
        const auto& tls = _image.Tls();
        auto* image = llvm::ConstantStruct::get(imageType, {segmentGlobal, llvm::ConstantInt::get(i64, segments.size()), functionGlobal, llvm::ConstantInt::get(i64, entries.size()), llvm::ConstantInt::get(i64, _image.Entry()),
            llvm::ConstantInt::get(i64, tls.Address), llvm::ConstantInt::get(i64, tls.FileSize), llvm::ConstantInt::get(i64, tls.MemorySize), llvm::ConstantInt::get(i64, tls.Alignment)});
        new llvm::GlobalVariable(*_module, imageType, true, llvm::GlobalValue::ExternalLinkage, image, "ps5lift_image");
    }
};

class FunctionLifter {
public:
    FunctionLifter(ModuleLifter& module, const GuestFunction& function)
        : _module(module), _function(function), _host(module.Guest(function.Entry)), _builder(module.Context()) {}

    void Lift() {
        auto* entry = llvm::BasicBlock::Create(Context(), "entry", _host);
        _builder.SetInsertPoint(entry);
        _state = _host->getArg(0);
        FindUsedRegisters();
        for (int i = 0; i < PS5LIFT_GPR_COUNT; ++i) {
            if (_usedGprs[i]) _gpr[i] = _builder.CreateAlloca(I64(), nullptr, "gpr" + std::to_string(i));
        }
        for (int i = 0; i < PS5LIFT_FLAG_COUNT; ++i) {
            if (_usesFlags) _flags[i] = _builder.CreateAlloca(_builder.getInt1Ty(), nullptr, "flag" + std::to_string(i));
        }
        for (int i = 0; i < VectorCount; ++i) {
            if (_usedVectors[i]) _vectors[i] = _builder.CreateAlloca(Int(256), nullptr, "ymm" + std::to_string(i));
        }
        Reload();
        for (const auto leader : _function.Leaders) _blocks[leader] = llvm::BasicBlock::Create(Context(), "b_" + Hex(leader), _host);
        _builder.CreateBr(_blocks.at(_function.Entry));

        std::optional<std::uint64_t> expected;
        for (const auto& [address, instruction] : _function.Instructions) {
            if (_blocks.contains(address)) {
                if (!_builder.GetInsertBlock()->getTerminator()) _builder.CreateBr(_blocks.at(address));
                _builder.SetInsertPoint(_blocks.at(address));
            } else if (!expected || *expected != address || _builder.GetInsertBlock()->getTerminator()) {
                StartDeadBlock();
            }
            _instruction = &instruction;
            EmitInstruction(instruction);
            ++_module.Report().Instructions;
            expected = instruction.Next();
            if (!_builder.GetInsertBlock()->getTerminator() && !_function.Instructions.contains(instruction.Next())) {
                TailCall(instruction.Next());
            }
        }
        for (auto& block : *_host) {
            if (!block.getTerminator()) {
                _builder.SetInsertPoint(&block);
                _builder.CreateUnreachable();
            }
        }
    }

private:
    ModuleLifter& _module;
    const GuestFunction& _function;
    llvm::Function* _host;
    llvm::IRBuilder<> _builder;
    llvm::Value* _state = nullptr;
    llvm::AllocaInst* _gpr[PS5LIFT_GPR_COUNT]{};
    llvm::AllocaInst* _flags[PS5LIFT_FLAG_COUNT]{};
    static constexpr int VectorCount = 16;
    llvm::AllocaInst* _vectors[VectorCount]{};
    std::array<bool, PS5LIFT_GPR_COUNT> _usedGprs{};
    std::array<bool, VectorCount> _usedVectors{};
    bool _usesFlags = false;
    std::map<std::uint64_t, llvm::BasicBlock*> _blocks;
    const Instruction* _instruction = nullptr;

    llvm::LLVMContext& Context() { return _module.Context(); }
    llvm::IntegerType* I64() { return _builder.getInt64Ty(); }
    llvm::IntegerType* Int(const unsigned bits) { return llvm::IntegerType::get(Context(), bits); }
    llvm::ConstantInt* Const(const unsigned bits, const std::uint64_t value) {
        return llvm::ConstantInt::get(Context(), llvm::APInt(bits, bits >= 64 ? value : value & ((std::uint64_t{1} << bits) - 1)));
    }

    llvm::Value* StateField(const std::size_t offset) {
        return _builder.CreateConstGEP1_64(_builder.getInt8Ty(), _state, offset);
    }

    llvm::Value* StateLoad(llvm::Type* type, const std::size_t offset) {
        auto* load = _builder.CreateLoad(type, StateField(offset));
        load->setMetadata(llvm::LLVMContext::MD_tbaa, _module.StateTag());
        return load;
    }

    void StateStore(llvm::Value* value, const std::size_t offset) {
        _builder.CreateStore(value, StateField(offset))->setMetadata(llvm::LLVMContext::MD_tbaa, _module.StateTag());
    }

    static std::size_t VectorOffset(const int index) {
        return offsetof(Ps5LiftState, Vector) + 32 * static_cast<std::size_t>(index);
    }

    void MarkRegister(const ZydisRegister reg) {
        if (reg == ZYDIS_REGISTER_NONE || reg == ZYDIS_REGISTER_RIP) return;
        if (const auto vector = VectorIndex(reg); vector >= 0) _usedVectors[vector] = true;
        if (const auto gpr = GprIndex(reg); gpr >= 0) _usedGprs[gpr] = true;
    }

    void FindUsedRegisters() {
        _usedGprs[PS5LIFT_RSP] = true;
        for (const auto& [address, instruction] : _function.Instructions) {
            const auto mnemonic = instruction.Info.mnemonic;
            if (mnemonic == ZYDIS_MNEMONIC_VZEROUPPER || mnemonic == ZYDIS_MNEMONIC_VZEROALL) _usedVectors.fill(true);
            const auto* flags = instruction.Info.cpu_flags;
            if (flags != nullptr && (flags->tested | flags->modified | flags->set_0 | flags->set_1 | flags->undefined) != 0) _usesFlags = true;
            for (unsigned i = 0; i < instruction.Info.operand_count; ++i) {
                const auto& operand = instruction.Operands[i];
                if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER) MarkRegister(operand.reg.value);
                if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY) {
                    MarkRegister(operand.mem.base);
                    MarkRegister(operand.mem.index);
                }
            }
        }
    }

    bool CrossesCalls(const int flag) const {
        return flag == PS5LIFT_DF || _module.Options().PreserveFlags;
    }

    void Flush() {
        for (int i = 0; i < PS5LIFT_GPR_COUNT; ++i) {
            if (_gpr[i]) StateStore(_builder.CreateLoad(I64(), _gpr[i]), offsetof(Ps5LiftState, Gpr) + 8 * i);
        }
        for (int i = 0; i < PS5LIFT_FLAG_COUNT; ++i) {
            if (_flags[i] && CrossesCalls(i)) StateStore(_builder.CreateZExt(_builder.CreateLoad(_builder.getInt1Ty(), _flags[i]), _builder.getInt8Ty()), offsetof(Ps5LiftState, Flags) + i);
        }
        for (int i = 0; i < VectorCount; ++i) {
            if (_vectors[i]) StateStore(_builder.CreateLoad(Int(256), _vectors[i]), VectorOffset(i));
        }
    }

    void Reload() {
        for (int i = 0; i < PS5LIFT_GPR_COUNT; ++i) {
            if (_gpr[i]) _builder.CreateStore(StateLoad(I64(), offsetof(Ps5LiftState, Gpr) + 8 * i), _gpr[i]);
        }
        for (int i = 0; i < PS5LIFT_FLAG_COUNT; ++i) {
            if (_flags[i] && CrossesCalls(i)) _builder.CreateStore(_builder.CreateTrunc(StateLoad(_builder.getInt8Ty(), offsetof(Ps5LiftState, Flags) + i), _builder.getInt1Ty()), _flags[i]);
            else if (_flags[i]) _builder.CreateStore(_builder.getFalse(), _flags[i]);
        }
        for (int i = 0; i < VectorCount; ++i) {
            if (_vectors[i]) _builder.CreateStore(StateLoad(Int(256), VectorOffset(i)), _vectors[i]);
        }
    }

    void StartDeadBlock() {
        _builder.SetInsertPoint(llvm::BasicBlock::Create(Context(), "dead", _host));
    }

    llvm::Value* Gpr(const int index) {
        if (_gpr[index]) return _builder.CreateLoad(I64(), _gpr[index]);
        return StateLoad(I64(), offsetof(Ps5LiftState, Gpr) + 8 * static_cast<std::size_t>(index));
    }

    void SetGpr(const int index, llvm::Value* value) {
        if (_gpr[index]) _builder.CreateStore(value, _gpr[index]);
        else StateStore(value, offsetof(Ps5LiftState, Gpr) + 8 * static_cast<std::size_t>(index));
    }

    llvm::Value* Flag(const int flag) {
        if (_flags[flag]) return _builder.CreateLoad(_builder.getInt1Ty(), _flags[flag]);
        return _builder.CreateTrunc(StateLoad(_builder.getInt8Ty(), offsetof(Ps5LiftState, Flags) + static_cast<std::size_t>(flag)), _builder.getInt1Ty());
    }

    void SetFlag(const int flag, llvm::Value* value) {
        if (_flags[flag]) _builder.CreateStore(value, _flags[flag]);
        else StateStore(_builder.CreateZExt(value, _builder.getInt8Ty()), offsetof(Ps5LiftState, Flags) + static_cast<std::size_t>(flag));
    }

    llvm::Value* Vector(const int index) {
        if (_vectors[index]) return _builder.CreateLoad(Int(256), _vectors[index]);
        return StateLoad(Int(256), VectorOffset(index));
    }

    void SetVector(const int index, llvm::Value* value) {
        if (_vectors[index]) _builder.CreateStore(value, _vectors[index]);
        else StateStore(value, VectorOffset(index));
    }

    void WriteVector(const int index, llvm::Value* value, const bool zeroUpper) {
        const auto bits = value->getType()->getIntegerBitWidth();
        if (bits == 256) return SetVector(index, value);
        auto* wide = _builder.CreateZExt(value, Int(256));
        if (zeroUpper) return SetVector(index, wide);
        auto* keep = llvm::ConstantInt::get(Context(), llvm::APInt::getHighBitsSet(256, 256 - bits));
        SetVector(index, _builder.CreateOr(_builder.CreateAnd(Vector(index), keep), wide));
    }

    llvm::Value* ReadRegister(const ZydisRegister reg) {
        if (reg == ZYDIS_REGISTER_RIP) return _builder.getInt64(_instruction->Next());
        const auto vector = VectorIndex(reg);
        if (vector >= 0) return _builder.CreateTrunc(Vector(vector), Int(ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, reg)));
        const auto index = GprIndex(reg);
        if (index < 0) throw UnsupportedInstruction(std::string("register ") + ZydisRegisterGetString(reg));
        const auto width = ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, reg);
        auto* value = Gpr(index);
        if (width == 64) return value;
        if (IsHighByte(reg)) value = _builder.CreateLShr(value, 8);
        return _builder.CreateTrunc(value, Int(width));
    }

    void WriteRegister(const ZydisRegister reg, llvm::Value* value) {
        const auto vector = VectorIndex(reg);
        if (vector >= 0) {
            const bool vex = _instruction->Info.encoding == ZYDIS_INSTRUCTION_ENCODING_VEX;
            WriteVector(vector, value, vex && value->getType()->getIntegerBitWidth() == 128);
            return;
        }
        const auto index = GprIndex(reg);
        if (index < 0) throw UnsupportedInstruction(std::string("register ") + ZydisRegisterGetString(reg));
        const auto width = ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, reg);
        if (width == 64) return SetGpr(index, value);
        auto* extended = _builder.CreateZExt(value, I64());
        if (width == 32) return SetGpr(index, extended);
        if (IsHighByte(reg)) return SetGpr(index, _builder.CreateOr(_builder.CreateAnd(Gpr(index), ~std::uint64_t{0xff00}), _builder.CreateShl(extended, 8)));
        const auto mask = width == 16 ? ~std::uint64_t{0xffff} : ~std::uint64_t{0xff};
        SetGpr(index, _builder.CreateOr(_builder.CreateAnd(Gpr(index), mask), extended));
    }

    llvm::Value* Address(const ZydisDecodedOperand& operand, const bool applySegment = true) {
        if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY) throw std::logic_error("Address of a non-memory operand");
        if (_instruction->Info.address_width != 64) throw UnsupportedInstruction("32-bit addressing");
        llvm::Value* address = _builder.getInt64(static_cast<std::uint64_t>(operand.mem.disp.value));
        if (operand.mem.base != ZYDIS_REGISTER_NONE) address = _builder.CreateAdd(address, ReadRegister(operand.mem.base));
        if (operand.mem.index != ZYDIS_REGISTER_NONE) address = _builder.CreateAdd(address, _builder.CreateMul(ReadRegister(operand.mem.index), _builder.getInt64(operand.mem.scale)));
        if (applySegment && operand.mem.segment == ZYDIS_REGISTER_FS) address = _builder.CreateAdd(address, StateLoad(I64(), offsetof(Ps5LiftState, FsBase)));
        if (applySegment && operand.mem.segment == ZYDIS_REGISTER_GS) address = _builder.CreateAdd(address, StateLoad(I64(), offsetof(Ps5LiftState, GsBase)));
        return address;
    }

    bool NeedsOrdering(const ZydisDecodedOperand& operand, const unsigned bits) const {
        if (_module.Options().Memory != MemoryModel::Tso || bits > 64) return false;
        return operand.mem.base != ZYDIS_REGISTER_RSP;
    }

    llvm::Value* Load(llvm::Value* address, const unsigned bits, const bool ordered) {
        auto* load = _builder.CreateLoad(Int(bits), _builder.CreateIntToPtr(address, _module.Pointer()));
        load->setMetadata(llvm::LLVMContext::MD_tbaa, _module.GuestTag());
        if (ordered) {
            load->setAtomic(llvm::AtomicOrdering::Acquire);
            load->setAlignment(llvm::Align(bits / 8));
        } else {
            load->setAlignment(llvm::Align(1));
        }
        return load;
    }

    void Store(llvm::Value* address, llvm::Value* value, const bool ordered) {
        auto* store = _builder.CreateStore(value, _builder.CreateIntToPtr(address, _module.Pointer()));
        store->setMetadata(llvm::LLVMContext::MD_tbaa, _module.GuestTag());
        if (ordered) {
            store->setAtomic(llvm::AtomicOrdering::Release);
            store->setAlignment(llvm::Align(value->getType()->getIntegerBitWidth() / 8));
        } else {
            store->setAlignment(llvm::Align(1));
        }
    }

    const ZydisDecodedOperand& Operand(const int index) const { return _instruction->Operands[index]; }
    unsigned Bits(const int index) const { return Operand(index).size; }

    llvm::Value* Read(const int index) {
        const auto& operand = Operand(index);
        switch (operand.type) {
        case ZYDIS_OPERAND_TYPE_REGISTER: {
            auto* value = ReadRegister(operand.reg.value);
            if (VectorIndex(operand.reg.value) >= 0 && operand.size < value->getType()->getIntegerBitWidth()) return _builder.CreateTrunc(value, Int(operand.size));
            return value;
        }
        case ZYDIS_OPERAND_TYPE_MEMORY:
            return Load(Address(operand), operand.size, NeedsOrdering(operand, operand.size));
        case ZYDIS_OPERAND_TYPE_IMMEDIATE: {
            const bool widen = index > 0 && Operand(0).type != ZYDIS_OPERAND_TYPE_IMMEDIATE && Operand(0).size > operand.size;
            return Const(widen ? Operand(0).size : operand.size, operand.imm.value.u);
        }
        default:
            throw UnsupportedInstruction("operand type");
        }
    }

    llvm::Value* ReadAs(const int index, const unsigned bits) {
        auto* value = Read(index);
        const auto width = value->getType()->getIntegerBitWidth();
        if (width == bits) return value;
        return width > bits ? _builder.CreateTrunc(value, Int(bits)) : _builder.CreateZExt(value, Int(bits));
    }

    void Write(const int index, llvm::Value* value) {
        const auto& operand = Operand(index);
        if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER) return WriteRegister(operand.reg.value, value);
        if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY) return Store(Address(operand), value, NeedsOrdering(operand, value->getType()->getIntegerBitWidth()));
        throw UnsupportedInstruction("write to operand type");
    }

    llvm::Value* SignBit(llvm::Value* value) {
        return _builder.CreateICmpSLT(value, llvm::ConstantInt::get(value->getType(), 0));
    }

    llvm::Value* Parity(llvm::Value* value) {
        auto* low = _builder.CreateTrunc(value, _builder.getInt8Ty());
        auto* count = _builder.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, low);
        return _builder.CreateICmpEQ(_builder.CreateAnd(count, 1), _builder.getInt8(0));
    }

    void ResultFlags(llvm::Value* result) {
        SetFlag(PS5LIFT_ZF, _builder.CreateICmpEQ(result, llvm::ConstantInt::get(result->getType(), 0)));
        SetFlag(PS5LIFT_SF, SignBit(result));
        SetFlag(PS5LIFT_PF, Parity(result));
    }

    void AdjustFlag(llvm::Value* a, llvm::Value* b, llvm::Value* result) {
        auto* bits = _builder.CreateXor(_builder.CreateXor(a, b), result);
        SetFlag(PS5LIFT_AF, _builder.CreateICmpNE(_builder.CreateAnd(bits, llvm::ConstantInt::get(a->getType(), 0x10)), llvm::ConstantInt::get(a->getType(), 0)));
    }

    void LogicFlags(llvm::Value* result) {
        ResultFlags(result);
        SetFlag(PS5LIFT_CF, _builder.getFalse());
        SetFlag(PS5LIFT_OF, _builder.getFalse());
        SetFlag(PS5LIFT_AF, _builder.getFalse());
    }

    llvm::Value* Add(llvm::Value* a, llvm::Value* b, llvm::Value* carry, const bool setCarry = true) {
        const auto bits = a->getType()->getIntegerBitWidth();
        auto* wide = _builder.CreateAdd(_builder.CreateAdd(_builder.CreateZExt(a, Int(bits + 1)), _builder.CreateZExt(b, Int(bits + 1))), _builder.CreateZExt(carry, Int(bits + 1)));
        auto* result = _builder.CreateTrunc(wide, Int(bits));
        if (setCarry) SetFlag(PS5LIFT_CF, _builder.CreateTrunc(_builder.CreateLShr(wide, bits), _builder.getInt1Ty()));
        SetFlag(PS5LIFT_OF, SignBit(_builder.CreateAnd(_builder.CreateXor(a, result), _builder.CreateXor(b, result))));
        AdjustFlag(a, b, result);
        ResultFlags(result);
        return result;
    }

    llvm::Value* Sub(llvm::Value* a, llvm::Value* b, llvm::Value* borrow, const bool setCarry = true) {
        const auto bits = a->getType()->getIntegerBitWidth();
        auto* subtrahend = _builder.CreateAdd(_builder.CreateZExt(b, Int(bits + 1)), _builder.CreateZExt(borrow, Int(bits + 1)));
        auto* result = _builder.CreateSub(_builder.CreateSub(a, b), _builder.CreateZExt(borrow, Int(bits)));
        if (setCarry) SetFlag(PS5LIFT_CF, _builder.CreateICmpULT(_builder.CreateZExt(a, Int(bits + 1)), subtrahend));
        SetFlag(PS5LIFT_OF, SignBit(_builder.CreateAnd(_builder.CreateXor(a, b), _builder.CreateXor(a, result))));
        AdjustFlag(a, b, result);
        ResultFlags(result);
        return result;
    }

    llvm::Value* Test(const Condition condition) {
        switch (condition) {
        case Condition::O: return Flag(PS5LIFT_OF);
        case Condition::NO: return _builder.CreateNot(Flag(PS5LIFT_OF));
        case Condition::B: return Flag(PS5LIFT_CF);
        case Condition::AE: return _builder.CreateNot(Flag(PS5LIFT_CF));
        case Condition::E: return Flag(PS5LIFT_ZF);
        case Condition::NE: return _builder.CreateNot(Flag(PS5LIFT_ZF));
        case Condition::BE: return _builder.CreateOr(Flag(PS5LIFT_CF), Flag(PS5LIFT_ZF));
        case Condition::A: return _builder.CreateNot(_builder.CreateOr(Flag(PS5LIFT_CF), Flag(PS5LIFT_ZF)));
        case Condition::S: return Flag(PS5LIFT_SF);
        case Condition::NS: return _builder.CreateNot(Flag(PS5LIFT_SF));
        case Condition::P: return Flag(PS5LIFT_PF);
        case Condition::NP: return _builder.CreateNot(Flag(PS5LIFT_PF));
        case Condition::L: return _builder.CreateICmpNE(Flag(PS5LIFT_SF), Flag(PS5LIFT_OF));
        case Condition::GE: return _builder.CreateICmpEQ(Flag(PS5LIFT_SF), Flag(PS5LIFT_OF));
        case Condition::LE: return _builder.CreateOr(Flag(PS5LIFT_ZF), _builder.CreateICmpNE(Flag(PS5LIFT_SF), Flag(PS5LIFT_OF)));
        case Condition::G: return _builder.CreateAnd(_builder.CreateNot(Flag(PS5LIFT_ZF)), _builder.CreateICmpEQ(Flag(PS5LIFT_SF), Flag(PS5LIFT_OF)));
        }
        throw std::logic_error("Unknown condition");
    }

    void Push(llvm::Value* value) {
        auto* rsp = _builder.CreateSub(Gpr(PS5LIFT_RSP), _builder.getInt64(8));
        Store(rsp, value, false);
        SetGpr(PS5LIFT_RSP, rsp);
    }

    llvm::Value* Pop() {
        auto* rsp = Gpr(PS5LIFT_RSP);
        auto* value = Load(rsp, 64, false);
        SetGpr(PS5LIFT_RSP, _builder.CreateAdd(rsp, _builder.getInt64(8)));
        return value;
    }

    void CallGuest(llvm::Value* callee, llvm::ArrayRef<llvm::Value*> extra = {}) {
        Flush();
        std::vector<llvm::Value*> arguments{_state};
        arguments.insert(arguments.end(), extra.begin(), extra.end());
        if (auto* function = llvm::dyn_cast<llvm::Function>(callee)) _builder.CreateCall(function, arguments);
        else _builder.CreateCall(_module.GuestType(), callee, arguments);
        Reload();
    }

    llvm::FunctionCallee DispatchCallee() {
        return _module.Runtime("ps5lift_dispatch", llvm::FunctionType::get(_builder.getVoidTy(), {_module.Pointer(), I64()}, false));
    }

    void TailCall(const std::uint64_t target) {
        if (!_module.Functions().contains(target)) throw std::runtime_error("Control leaves 0x" + Hex(_function.Entry) + " for undiscovered 0x" + Hex(target));
        Flush();
        _builder.CreateCall(_module.Guest(target), {_state})->setTailCallKind(llvm::CallInst::TCK_Tail);
        _builder.CreateRetVoid();
    }

    llvm::BasicBlock* Target(const std::uint64_t target) {
        if (_blocks.contains(target)) return _blocks.at(target);
        auto* current = _builder.GetInsertBlock();
        auto* block = llvm::BasicBlock::Create(Context(), "tail_" + Hex(target), _host);
        _builder.SetInsertPoint(block);
        TailCall(target);
        _builder.SetInsertPoint(current);
        return block;
    }

    void Unsupported(const std::string& what) {
        const std::string mnemonic = ZydisMnemonicGetString(_instruction->Info.mnemonic);
        ++_module.Report().Unsupported[mnemonic + (what.empty() ? "" : " (" + what + ")")];
        Flush();
        auto callee = _module.Runtime("ps5lift_unsupported", llvm::FunctionType::get(_builder.getVoidTy(), {_module.Pointer(), I64(), _module.Pointer()}, false));
        auto* call = _builder.CreateCall(callee, {_state, _builder.getInt64(_instruction->Address), _builder.CreateGlobalStringPtr(mnemonic)});
        call->setDoesNotReturn();
        _builder.CreateUnreachable();
    }

    void Trap() {
        Flush();
        auto callee = _module.Runtime("ps5lift_trap", llvm::FunctionType::get(_builder.getVoidTy(), {_module.Pointer(), I64()}, false));
        _builder.CreateCall(callee, {_state, _builder.getInt64(_instruction->Address)})->setDoesNotReturn();
        _builder.CreateUnreachable();
    }

    void StringOp(const StringOperation operation, const unsigned width) {
        Flush();
        auto callee = _module.Runtime("ps5lift_string_op", llvm::FunctionType::get(_builder.getVoidTy(), {_module.Pointer(), _builder.getInt32Ty(), _builder.getInt32Ty(), _builder.getInt32Ty()}, false));
        const bool repeat = (_instruction->Info.attributes & ZYDIS_ATTRIB_HAS_REP) != 0;
        _builder.CreateCall(callee, {_state, _builder.getInt32(static_cast<std::uint32_t>(operation)), _builder.getInt32(width), _builder.getInt32(repeat ? 1 : 0)});
        Reload();
    }

    bool Locked() const {
        return (_instruction->Info.attributes & ZYDIS_ATTRIB_HAS_LOCK) != 0;
    }

    llvm::Value* MemoryPointer(const int index) {
        return _builder.CreateIntToPtr(Address(Operand(index)), _module.Pointer());
    }

    void Shift(const ZydisMnemonic mnemonic) {
        const auto bits = Bits(0);
        auto* value = Read(0);
        auto* count = _builder.CreateAnd(ReadAs(1, 8), _builder.getInt8(bits == 64 ? 63 : 31));
        auto* zero = _builder.CreateICmpEQ(count, _builder.getInt8(0));
        auto* wideCount = _builder.CreateZExt(count, Int(128));
        auto* below = _builder.CreateSelect(zero, llvm::ConstantInt::get(Int(128), 0), _builder.CreateSub(wideCount, llvm::ConstantInt::get(Int(128), 1)));
        llvm::Value* result = nullptr;
        llvm::Value* carry = nullptr;
        llvm::Value* overflow = nullptr;
        const bool rotate = mnemonic == ZYDIS_MNEMONIC_ROL || mnemonic == ZYDIS_MNEMONIC_ROR;
        if (mnemonic == ZYDIS_MNEMONIC_SHL) {
            auto* wide = _builder.CreateShl(_builder.CreateZExt(value, Int(128)), wideCount);
            result = _builder.CreateTrunc(wide, Int(bits));
            carry = _builder.CreateTrunc(_builder.CreateLShr(wide, bits), _builder.getInt1Ty());
            overflow = _builder.CreateXor(SignBit(result), carry);
        } else if (mnemonic == ZYDIS_MNEMONIC_SHR) {
            auto* wide = _builder.CreateZExt(value, Int(128));
            result = _builder.CreateTrunc(_builder.CreateLShr(wide, wideCount), Int(bits));
            carry = _builder.CreateTrunc(_builder.CreateLShr(wide, below), _builder.getInt1Ty());
            overflow = SignBit(value);
        } else if (mnemonic == ZYDIS_MNEMONIC_SAR) {
            auto* wide = _builder.CreateSExt(value, Int(128));
            result = _builder.CreateTrunc(_builder.CreateAShr(wide, wideCount), Int(bits));
            carry = _builder.CreateTrunc(_builder.CreateAShr(wide, below), _builder.getInt1Ty());
            overflow = _builder.getFalse();
        } else {
            auto* amount = _builder.CreateZExtOrTrunc(_builder.CreateURem(count, _builder.getInt8(bits)), Int(bits));
            const auto intrinsic = mnemonic == ZYDIS_MNEMONIC_ROL ? llvm::Intrinsic::fshl : llvm::Intrinsic::fshr;
            result = _builder.CreateIntrinsic(intrinsic, {Int(bits)}, {value, value, amount});
            if (mnemonic == ZYDIS_MNEMONIC_ROL) {
                carry = _builder.CreateTrunc(result, _builder.getInt1Ty());
                overflow = _builder.CreateXor(SignBit(result), carry);
            } else {
                carry = SignBit(result);
                overflow = _builder.CreateXor(carry, _builder.CreateTrunc(_builder.CreateLShr(result, bits - 2), _builder.getInt1Ty()));
            }
        }
        Write(0, result);
        SetFlag(PS5LIFT_CF, _builder.CreateSelect(zero, Flag(PS5LIFT_CF), carry));
        SetFlag(PS5LIFT_OF, _builder.CreateSelect(zero, Flag(PS5LIFT_OF), overflow));
        if (rotate) return;
        SetFlag(PS5LIFT_ZF, _builder.CreateSelect(zero, Flag(PS5LIFT_ZF), _builder.CreateICmpEQ(result, llvm::ConstantInt::get(result->getType(), 0))));
        SetFlag(PS5LIFT_SF, _builder.CreateSelect(zero, Flag(PS5LIFT_SF), SignBit(result)));
        SetFlag(PS5LIFT_PF, _builder.CreateSelect(zero, Flag(PS5LIFT_PF), Parity(result)));
    }

    int ImplicitGpr(const ZydisRegister reg) const {
        return GprIndex(reg);
    }

    llvm::Value* LowAccumulator(const unsigned bits) {
        return _builder.CreateTrunc(Gpr(PS5LIFT_RAX), Int(bits));
    }

    void WriteAccumulatorPair(const unsigned bits, llvm::Value* low, llvm::Value* high) {
        if (bits == 8) {
            auto* ax = _builder.CreateOr(_builder.CreateZExt(low, _builder.getInt16Ty()), _builder.CreateShl(_builder.CreateZExt(high, _builder.getInt16Ty()), 8));
            return WriteRegister(ZYDIS_REGISTER_AX, ax);
        }
        const auto rax = bits == 64 ? ZYDIS_REGISTER_RAX : bits == 32 ? ZYDIS_REGISTER_EAX : ZYDIS_REGISTER_AX;
        const auto rdx = bits == 64 ? ZYDIS_REGISTER_RDX : bits == 32 ? ZYDIS_REGISTER_EDX : ZYDIS_REGISTER_DX;
        WriteRegister(rax, low);
        WriteRegister(rdx, high);
    }

    llvm::Value* AccumulatorPair(const unsigned bits) {
        if (bits == 8) return _builder.CreateTrunc(Gpr(PS5LIFT_RAX), _builder.getInt16Ty());
        auto* low = _builder.CreateZExt(LowAccumulator(bits), Int(bits * 2));
        auto* high = _builder.CreateZExt(_builder.CreateTrunc(Gpr(PS5LIFT_RDX), Int(bits)), Int(bits * 2));
        return _builder.CreateOr(low, _builder.CreateShl(high, bits));
    }

    void Multiply(const bool isSigned) {
        const auto bits = Bits(0);
        auto* source = Read(0);
        auto* a = LowAccumulator(bits);
        auto* wideA = isSigned ? _builder.CreateSExt(a, Int(bits * 2)) : _builder.CreateZExt(a, Int(bits * 2));
        auto* wideB = isSigned ? _builder.CreateSExt(source, Int(bits * 2)) : _builder.CreateZExt(source, Int(bits * 2));
        auto* product = _builder.CreateMul(wideA, wideB);
        auto* low = _builder.CreateTrunc(product, Int(bits));
        auto* high = _builder.CreateTrunc(_builder.CreateLShr(product, bits), Int(bits));
        WriteAccumulatorPair(bits, low, high);
        auto* overflow = isSigned ? _builder.CreateICmpNE(_builder.CreateSExt(low, Int(bits * 2)), product) : _builder.CreateICmpNE(high, llvm::ConstantInt::get(high->getType(), 0));
        SetFlag(PS5LIFT_CF, overflow);
        SetFlag(PS5LIFT_OF, overflow);
        ResultFlags(low);
    }

    void Divide(const bool isSigned) {
        const auto bits = Bits(0);
        auto* divisor = Read(0);
        auto* dividend = AccumulatorPair(bits);
        auto* wideDivisor = isSigned ? _builder.CreateSExt(divisor, Int(bits * 2)) : _builder.CreateZExt(divisor, Int(bits * 2));
        auto* quotient = isSigned ? _builder.CreateSDiv(dividend, wideDivisor) : _builder.CreateUDiv(dividend, wideDivisor);
        auto* remainder = isSigned ? _builder.CreateSRem(dividend, wideDivisor) : _builder.CreateURem(dividend, wideDivisor);
        WriteAccumulatorPair(bits, _builder.CreateTrunc(quotient, Int(bits)), _builder.CreateTrunc(remainder, Int(bits)));
    }

    void SignedMultiply() {
        if (_instruction->Info.operand_count_visible == 1) return Multiply(true);
        const auto bits = Bits(0);
        auto* a = _instruction->Info.operand_count_visible == 3 ? Read(1) : Read(0);
        auto* b = _instruction->Info.operand_count_visible == 3 ? ReadAs(2, bits) : Read(1);
        auto* product = _builder.CreateMul(_builder.CreateSExt(a, Int(bits * 2)), _builder.CreateSExt(b, Int(bits * 2)));
        auto* result = _builder.CreateTrunc(product, Int(bits));
        auto* overflow = _builder.CreateICmpNE(_builder.CreateSExt(result, Int(bits * 2)), product);
        Write(0, result);
        SetFlag(PS5LIFT_CF, overflow);
        SetFlag(PS5LIFT_OF, overflow);
        ResultFlags(result);
    }

    void BitScan(const ZydisMnemonic mnemonic) {
        auto* source = Read(1);
        auto* type = source->getType();
        const auto bits = type->getIntegerBitWidth();
        auto* isZero = _builder.CreateICmpEQ(source, llvm::ConstantInt::get(type, 0));
        switch (mnemonic) {
        case ZYDIS_MNEMONIC_BSF:
        case ZYDIS_MNEMONIC_BSR: {
            llvm::Value* index = _builder.CreateBinaryIntrinsic(llvm::Intrinsic::cttz, source, _builder.getTrue());
            if (mnemonic == ZYDIS_MNEMONIC_BSR) index = _builder.CreateSub(llvm::ConstantInt::get(type, bits - 1), _builder.CreateBinaryIntrinsic(llvm::Intrinsic::ctlz, source, _builder.getTrue()));
            Write(0, _builder.CreateSelect(isZero, Read(0), index));
            SetFlag(PS5LIFT_ZF, isZero);
            return;
        }
        case ZYDIS_MNEMONIC_TZCNT:
        case ZYDIS_MNEMONIC_LZCNT: {
            const auto intrinsic = mnemonic == ZYDIS_MNEMONIC_TZCNT ? llvm::Intrinsic::cttz : llvm::Intrinsic::ctlz;
            auto* result = _builder.CreateBinaryIntrinsic(intrinsic, source, _builder.getFalse());
            Write(0, result);
            SetFlag(PS5LIFT_CF, isZero);
            SetFlag(PS5LIFT_ZF, _builder.CreateICmpEQ(result, llvm::ConstantInt::get(type, 0)));
            return;
        }
        default: {
            Write(0, _builder.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, source));
            SetFlag(PS5LIFT_ZF, isZero);
            for (const auto flag : {PS5LIFT_CF, PS5LIFT_PF, PS5LIFT_AF, PS5LIFT_SF, PS5LIFT_OF}) SetFlag(flag, _builder.getFalse());
            return;
        }
        }
    }

    void BitTest(const ZydisMnemonic mnemonic) {
        if (Operand(0).type == ZYDIS_OPERAND_TYPE_MEMORY && Operand(1).type == ZYDIS_OPERAND_TYPE_REGISTER) throw UnsupportedInstruction("bit string in memory");
        const auto bits = Bits(0);
        auto* value = Read(0);
        auto* index = _builder.CreateAnd(ReadAs(1, bits), bits - 1);
        auto* mask = _builder.CreateShl(llvm::ConstantInt::get(value->getType(), 1), index);
        SetFlag(PS5LIFT_CF, _builder.CreateICmpNE(_builder.CreateAnd(value, mask), llvm::ConstantInt::get(value->getType(), 0)));
        if (mnemonic == ZYDIS_MNEMONIC_BTS) Write(0, _builder.CreateOr(value, mask));
        if (mnemonic == ZYDIS_MNEMONIC_BTR) Write(0, _builder.CreateAnd(value, _builder.CreateNot(mask)));
        if (mnemonic == ZYDIS_MNEMONIC_BTC) Write(0, _builder.CreateXor(value, mask));
    }

    void AtomicRmw(const llvm::AtomicRMWInst::BinOp operation, llvm::Value* operand, const ZydisMnemonic mnemonic) {
        auto* old = _builder.CreateAtomicRMW(operation, MemoryPointer(0), operand, llvm::MaybeAlign(operand->getType()->getIntegerBitWidth() / 8), llvm::AtomicOrdering::SequentiallyConsistent);
        switch (mnemonic) {
        case ZYDIS_MNEMONIC_ADD: Add(old, operand, _builder.getFalse()); return;
        case ZYDIS_MNEMONIC_SUB: Sub(old, operand, _builder.getFalse()); return;
        case ZYDIS_MNEMONIC_INC: Add(old, operand, _builder.getFalse(), false); return;
        case ZYDIS_MNEMONIC_DEC: Sub(old, operand, _builder.getFalse(), false); return;
        case ZYDIS_MNEMONIC_AND: LogicFlags(_builder.CreateAnd(old, operand)); return;
        case ZYDIS_MNEMONIC_OR: LogicFlags(_builder.CreateOr(old, operand)); return;
        case ZYDIS_MNEMONIC_XOR: LogicFlags(_builder.CreateXor(old, operand)); return;
        default: throw UnsupportedInstruction("locked form");
        }
    }

    bool LockedMemory() const {
        return Locked() && Operand(0).type == ZYDIS_OPERAND_TYPE_MEMORY;
    }

    void Exchange() {
        if (Operand(0).type == ZYDIS_OPERAND_TYPE_MEMORY || Operand(1).type == ZYDIS_OPERAND_TYPE_MEMORY) {
            const int memory = Operand(0).type == ZYDIS_OPERAND_TYPE_MEMORY ? 0 : 1;
            const int reg = 1 - memory;
            auto* old = _builder.CreateAtomicRMW(llvm::AtomicRMWInst::Xchg, MemoryPointer(memory), Read(reg), llvm::MaybeAlign(Bits(reg) / 8), llvm::AtomicOrdering::SequentiallyConsistent);
            Write(reg, old);
            return;
        }
        auto* a = Read(0);
        auto* b = Read(1);
        Write(0, b);
        Write(1, a);
    }

    void ExchangeAdd() {
        auto* source = Read(1);
        llvm::Value* old = nullptr;
        if (Operand(0).type == ZYDIS_OPERAND_TYPE_MEMORY) {
            old = _builder.CreateAtomicRMW(llvm::AtomicRMWInst::Add, MemoryPointer(0), source, llvm::MaybeAlign(Bits(1) / 8), llvm::AtomicOrdering::SequentiallyConsistent);
            Add(old, source, _builder.getFalse());
        } else {
            old = Read(0);
            Write(0, Add(old, source, _builder.getFalse()));
        }
        Write(1, old);
    }

    void CompareExchange() {
        const auto bits = Bits(0);
        auto* expected = LowAccumulator(bits);
        auto* desired = Read(1);
        llvm::Value* old = nullptr;
        if (Operand(0).type == ZYDIS_OPERAND_TYPE_MEMORY) {
            auto* exchange = _builder.CreateAtomicCmpXchg(MemoryPointer(0), expected, desired, llvm::MaybeAlign(bits / 8), llvm::AtomicOrdering::SequentiallyConsistent, llvm::AtomicOrdering::SequentiallyConsistent);
            old = _builder.CreateExtractValue(exchange, 0);
        } else {
            old = Read(0);
            Write(0, _builder.CreateSelect(_builder.CreateICmpEQ(old, expected), desired, old));
        }
        Sub(expected, old, _builder.getFalse());
        auto* equal = _builder.CreateICmpEQ(old, expected);
        if (bits == 32) return SetGpr(PS5LIFT_RAX, _builder.CreateSelect(equal, Gpr(PS5LIFT_RAX), _builder.CreateZExt(old, I64())));
        WriteRegister(bits == 64 ? ZYDIS_REGISTER_RAX : bits == 16 ? ZYDIS_REGISTER_AX : ZYDIS_REGISTER_AL, old);
    }

    void SignExtendAccumulator(const ZydisMnemonic mnemonic) {
        switch (mnemonic) {
        case ZYDIS_MNEMONIC_CBW: WriteRegister(ZYDIS_REGISTER_AX, _builder.CreateSExt(LowAccumulator(8), _builder.getInt16Ty())); return;
        case ZYDIS_MNEMONIC_CWDE: WriteRegister(ZYDIS_REGISTER_EAX, _builder.CreateSExt(LowAccumulator(16), _builder.getInt32Ty())); return;
        case ZYDIS_MNEMONIC_CDQE: SetGpr(PS5LIFT_RAX, _builder.CreateSExt(LowAccumulator(32), I64())); return;
        case ZYDIS_MNEMONIC_CWD: WriteRegister(ZYDIS_REGISTER_DX, _builder.CreateAShr(LowAccumulator(16), 15)); return;
        case ZYDIS_MNEMONIC_CDQ: WriteRegister(ZYDIS_REGISTER_EDX, _builder.CreateAShr(LowAccumulator(32), 31)); return;
        default: SetGpr(PS5LIFT_RDX, _builder.CreateAShr(Gpr(PS5LIFT_RAX), 63)); return;
        }
    }

    void VectorBitwise(const ZydisMnemonic mnemonic) {
        const bool threeOperand = _instruction->Info.operand_count_visible == 3;
        const auto bits = Bits(0);
        auto* a = threeOperand ? ReadAs(1, bits) : Read(0);
        auto* b = ReadAs(threeOperand ? 2 : 1, bits);
        llvm::Value* result = nullptr;
        switch (mnemonic) {
        case ZYDIS_MNEMONIC_PXOR: case ZYDIS_MNEMONIC_XORPS: case ZYDIS_MNEMONIC_XORPD:
        case ZYDIS_MNEMONIC_VPXOR: case ZYDIS_MNEMONIC_VXORPS: case ZYDIS_MNEMONIC_VXORPD:
            result = _builder.CreateXor(a, b);
            break;
        case ZYDIS_MNEMONIC_PAND: case ZYDIS_MNEMONIC_ANDPS: case ZYDIS_MNEMONIC_ANDPD:
        case ZYDIS_MNEMONIC_VPAND: case ZYDIS_MNEMONIC_VANDPS: case ZYDIS_MNEMONIC_VANDPD:
            result = _builder.CreateAnd(a, b);
            break;
        default:
            result = _builder.CreateOr(a, b);
            break;
        }
        Write(0, result);
    }

    void VectorMove() {
        const auto& destination = Operand(0);
        const bool toVector = destination.type == ZYDIS_OPERAND_TYPE_REGISTER && VectorIndex(destination.reg.value) >= 0;
        auto* value = Read(1);
        const auto source = value->getType()->getIntegerBitWidth();
        if (!toVector) {
            Write(0, _builder.CreateZExtOrTrunc(value, Int(Bits(0))));
            return;
        }
        const auto target = source > 128 ? 256u : 128u;
        const auto mnemonic = _instruction->Info.mnemonic;
        const bool scalarMerge = (mnemonic == ZYDIS_MNEMONIC_MOVSS || mnemonic == ZYDIS_MNEMONIC_MOVSD) && Operand(1).type == ZYDIS_OPERAND_TYPE_REGISTER;
        if (source < target && scalarMerge) {
            auto* old = ReadRegister(destination.reg.value);
            if (old->getType()->getIntegerBitWidth() > 128) old = _builder.CreateTrunc(old, Int(128));
            auto* mask = llvm::ConstantInt::get(Context(), llvm::APInt::getHighBitsSet(128, 128 - source));
            value = _builder.CreateOr(_builder.CreateAnd(old, mask), _builder.CreateZExt(value, Int(128)));
        } else if (source < target) {
            value = _builder.CreateZExt(value, Int(target));
        }
        WriteRegister(destination.reg.value, value);
    }

    void BitManipulation(const ZydisMnemonic mnemonic) {
        const auto bits = Bits(0);
        auto* type = Int(bits);
        auto* zero = llvm::ConstantInt::get(type, 0);
        auto* countMask = llvm::ConstantInt::get(type, bits - 1);
        switch (mnemonic) {
        case ZYDIS_MNEMONIC_ANDN: {
            auto* result = _builder.CreateAnd(_builder.CreateNot(Read(1)), Read(2));
            Write(0, result);
            LogicFlags(result);
            return;
        }
        case ZYDIS_MNEMONIC_SHLX:
        case ZYDIS_MNEMONIC_SHRX:
        case ZYDIS_MNEMONIC_SARX: {
            auto* count = _builder.CreateAnd(ReadAs(2, bits), countMask);
            auto* value = Read(1);
            auto* result = mnemonic == ZYDIS_MNEMONIC_SHLX ? _builder.CreateShl(value, count) : mnemonic == ZYDIS_MNEMONIC_SHRX ? _builder.CreateLShr(value, count) : _builder.CreateAShr(value, count);
            Write(0, result);
            return;
        }
        case ZYDIS_MNEMONIC_RORX: {
            auto* count = _builder.CreateAnd(ReadAs(2, bits), countMask);
            auto* value = Read(1);
            Write(0, _builder.CreateIntrinsic(llvm::Intrinsic::fshr, {type}, {value, value, count}));
            return;
        }
        case ZYDIS_MNEMONIC_BEXTR: {
            auto* control = ReadAs(2, bits);
            auto* start = _builder.CreateAnd(control, llvm::ConstantInt::get(type, 0xff));
            auto* length = _builder.CreateAnd(_builder.CreateLShr(control, 8), llvm::ConstantInt::get(type, 0xff));
            auto* wide = _builder.CreateZExt(Read(1), Int(512));
            auto* shifted = _builder.CreateLShr(wide, _builder.CreateZExt(start, Int(512)));
            auto* mask = _builder.CreateSub(_builder.CreateShl(llvm::ConstantInt::get(Int(512), 1), _builder.CreateZExt(length, Int(512))), llvm::ConstantInt::get(Int(512), 1));
            auto* result = _builder.CreateTrunc(_builder.CreateAnd(shifted, mask), type);
            Write(0, result);
            LogicFlags(result);
            return;
        }
        case ZYDIS_MNEMONIC_BZHI: {
            auto* index = _builder.CreateAnd(ReadAs(2, bits), llvm::ConstantInt::get(type, 0xff));
            auto* value = Read(1);
            auto* inRange = _builder.CreateICmpULT(index, llvm::ConstantInt::get(type, bits));
            auto* mask = _builder.CreateSub(_builder.CreateShl(llvm::ConstantInt::get(type, 1), _builder.CreateAnd(index, countMask)), llvm::ConstantInt::get(type, 1));
            auto* result = _builder.CreateSelect(inRange, _builder.CreateAnd(value, mask), value);
            Write(0, result);
            LogicFlags(result);
            SetFlag(PS5LIFT_CF, _builder.CreateNot(inRange));
            return;
        }
        case ZYDIS_MNEMONIC_BLSR:
        case ZYDIS_MNEMONIC_BLSI:
        case ZYDIS_MNEMONIC_BLSMSK: {
            auto* value = Read(1);
            auto* minusOne = _builder.CreateSub(value, llvm::ConstantInt::get(type, 1));
            llvm::Value* result = nullptr;
            if (mnemonic == ZYDIS_MNEMONIC_BLSR) result = _builder.CreateAnd(value, minusOne);
            else if (mnemonic == ZYDIS_MNEMONIC_BLSI) result = _builder.CreateAnd(value, _builder.CreateNeg(value));
            else result = _builder.CreateXor(value, minusOne);
            Write(0, result);
            ResultFlags(result);
            SetFlag(PS5LIFT_OF, _builder.getFalse());
            if (mnemonic == ZYDIS_MNEMONIC_BLSI) SetFlag(PS5LIFT_CF, _builder.CreateICmpNE(value, zero));
            else SetFlag(PS5LIFT_CF, _builder.CreateICmpEQ(value, zero));
            if (mnemonic == ZYDIS_MNEMONIC_BLSMSK) SetFlag(PS5LIFT_ZF, _builder.getFalse());
            return;
        }
        default: {
            auto* product = _builder.CreateMul(_builder.CreateZExt(Read(2), Int(bits * 2)), _builder.CreateZExt(LowRdx(bits), Int(bits * 2)));
            auto* low = _builder.CreateTrunc(product, type);
            auto* high = _builder.CreateTrunc(_builder.CreateLShr(product, bits), type);
            Write(1, low);
            Write(0, high);
            return;
        }
        }
    }

    llvm::Value* LowRdx(const unsigned bits) {
        return _builder.CreateTrunc(Gpr(PS5LIFT_RDX), Int(bits));
    }

    void Broadcast() {
        const auto& destination = Operand(0);
        const auto width = ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, destination.reg.value);
        auto* element = Read(1);
        const auto elementBits = Bits(1);
        if (element->getType()->getIntegerBitWidth() != elementBits) element = _builder.CreateTrunc(element, Int(elementBits));
        auto* vectorType = llvm::FixedVectorType::get(Int(elementBits), width / elementBits);
        auto* splat = _builder.CreateVectorSplat(vectorType->getNumElements(), element);
        WriteRegister(destination.reg.value, _builder.CreateBitCast(splat, Int(width)));
    }

    enum class VectorKind {
        Add, Sub, AddSignedSaturate, AddUnsignedSaturate, SubSignedSaturate, SubUnsignedSaturate,
        MultiplyLow, MultiplyHighSigned, MultiplyHighUnsigned, MultiplyEvenUnsigned, MultiplyEvenSigned, MultiplyAddWords,
        MinSigned, MaxSigned, MinUnsigned, MaxUnsigned, CompareEqual, CompareGreater, Average, AndNot, Absolute,
        ShiftLeft, ShiftRightLogical, ShiftRightArithmetic, ShiftLeftVariable, ShiftRightLogicalVariable, ShiftRightArithmeticVariable,
        ShiftLeftBytes, ShiftRightBytes, ShuffleDwords, ShuffleLowWords, ShuffleHighWords, ShufflePairs, UnpackLow, UnpackHigh,
        PackSignedSaturate, PackUnsignedSaturate, AlignRight, ShuffleBytes, PermuteQwords, PermuteDwords, Permute128,
        Insert128, Extract128, BlendImmediate, BlendVariable, ZeroExtend, SignExtend, MoveMask, Extract, Insert, Test
    };

    struct VectorSpec {
        VectorKind Kind;
        unsigned Element;
        unsigned Source = 0;
    };

    static std::optional<VectorSpec> VectorSpecOf(const ZydisMnemonic mnemonic) {
        using K = VectorKind;
#define PS5LIFT_BOTH(name) case ZYDIS_MNEMONIC_##name: case ZYDIS_MNEMONIC_V##name
        switch (mnemonic) {
        PS5LIFT_BOTH(PADDB): return VectorSpec{K::Add, 8};
        PS5LIFT_BOTH(PADDW): return VectorSpec{K::Add, 16};
        PS5LIFT_BOTH(PADDD): return VectorSpec{K::Add, 32};
        PS5LIFT_BOTH(PADDQ): return VectorSpec{K::Add, 64};
        PS5LIFT_BOTH(PSUBB): return VectorSpec{K::Sub, 8};
        PS5LIFT_BOTH(PSUBW): return VectorSpec{K::Sub, 16};
        PS5LIFT_BOTH(PSUBD): return VectorSpec{K::Sub, 32};
        PS5LIFT_BOTH(PSUBQ): return VectorSpec{K::Sub, 64};
        PS5LIFT_BOTH(PADDSB): return VectorSpec{K::AddSignedSaturate, 8};
        PS5LIFT_BOTH(PADDSW): return VectorSpec{K::AddSignedSaturate, 16};
        PS5LIFT_BOTH(PADDUSB): return VectorSpec{K::AddUnsignedSaturate, 8};
        PS5LIFT_BOTH(PADDUSW): return VectorSpec{K::AddUnsignedSaturate, 16};
        PS5LIFT_BOTH(PSUBSB): return VectorSpec{K::SubSignedSaturate, 8};
        PS5LIFT_BOTH(PSUBSW): return VectorSpec{K::SubSignedSaturate, 16};
        PS5LIFT_BOTH(PSUBUSB): return VectorSpec{K::SubUnsignedSaturate, 8};
        PS5LIFT_BOTH(PSUBUSW): return VectorSpec{K::SubUnsignedSaturate, 16};
        PS5LIFT_BOTH(PMULLW): return VectorSpec{K::MultiplyLow, 16};
        PS5LIFT_BOTH(PMULLD): return VectorSpec{K::MultiplyLow, 32};
        PS5LIFT_BOTH(PMULHW): return VectorSpec{K::MultiplyHighSigned, 16};
        PS5LIFT_BOTH(PMULHUW): return VectorSpec{K::MultiplyHighUnsigned, 16};
        PS5LIFT_BOTH(PMULUDQ): return VectorSpec{K::MultiplyEvenUnsigned, 64};
        PS5LIFT_BOTH(PMULDQ): return VectorSpec{K::MultiplyEvenSigned, 64};
        PS5LIFT_BOTH(PMADDWD): return VectorSpec{K::MultiplyAddWords, 32};
        PS5LIFT_BOTH(PMINSB): return VectorSpec{K::MinSigned, 8};
        PS5LIFT_BOTH(PMINSW): return VectorSpec{K::MinSigned, 16};
        PS5LIFT_BOTH(PMINSD): return VectorSpec{K::MinSigned, 32};
        PS5LIFT_BOTH(PMINUB): return VectorSpec{K::MinUnsigned, 8};
        PS5LIFT_BOTH(PMINUW): return VectorSpec{K::MinUnsigned, 16};
        PS5LIFT_BOTH(PMINUD): return VectorSpec{K::MinUnsigned, 32};
        PS5LIFT_BOTH(PMAXSB): return VectorSpec{K::MaxSigned, 8};
        PS5LIFT_BOTH(PMAXSW): return VectorSpec{K::MaxSigned, 16};
        PS5LIFT_BOTH(PMAXSD): return VectorSpec{K::MaxSigned, 32};
        PS5LIFT_BOTH(PMAXUB): return VectorSpec{K::MaxUnsigned, 8};
        PS5LIFT_BOTH(PMAXUW): return VectorSpec{K::MaxUnsigned, 16};
        PS5LIFT_BOTH(PMAXUD): return VectorSpec{K::MaxUnsigned, 32};
        PS5LIFT_BOTH(PCMPEQB): return VectorSpec{K::CompareEqual, 8};
        PS5LIFT_BOTH(PCMPEQW): return VectorSpec{K::CompareEqual, 16};
        PS5LIFT_BOTH(PCMPEQD): return VectorSpec{K::CompareEqual, 32};
        PS5LIFT_BOTH(PCMPEQQ): return VectorSpec{K::CompareEqual, 64};
        PS5LIFT_BOTH(PCMPGTB): return VectorSpec{K::CompareGreater, 8};
        PS5LIFT_BOTH(PCMPGTW): return VectorSpec{K::CompareGreater, 16};
        PS5LIFT_BOTH(PCMPGTD): return VectorSpec{K::CompareGreater, 32};
        PS5LIFT_BOTH(PCMPGTQ): return VectorSpec{K::CompareGreater, 64};
        PS5LIFT_BOTH(PAVGB): return VectorSpec{K::Average, 8};
        PS5LIFT_BOTH(PAVGW): return VectorSpec{K::Average, 16};
        PS5LIFT_BOTH(PANDN): case ZYDIS_MNEMONIC_ANDNPS: case ZYDIS_MNEMONIC_VANDNPS: case ZYDIS_MNEMONIC_ANDNPD: case ZYDIS_MNEMONIC_VANDNPD: return VectorSpec{K::AndNot, 64};
        PS5LIFT_BOTH(PABSB): return VectorSpec{K::Absolute, 8};
        PS5LIFT_BOTH(PABSW): return VectorSpec{K::Absolute, 16};
        PS5LIFT_BOTH(PABSD): return VectorSpec{K::Absolute, 32};
        PS5LIFT_BOTH(PSLLW): return VectorSpec{K::ShiftLeft, 16};
        PS5LIFT_BOTH(PSLLD): return VectorSpec{K::ShiftLeft, 32};
        PS5LIFT_BOTH(PSLLQ): return VectorSpec{K::ShiftLeft, 64};
        PS5LIFT_BOTH(PSRLW): return VectorSpec{K::ShiftRightLogical, 16};
        PS5LIFT_BOTH(PSRLD): return VectorSpec{K::ShiftRightLogical, 32};
        PS5LIFT_BOTH(PSRLQ): return VectorSpec{K::ShiftRightLogical, 64};
        PS5LIFT_BOTH(PSRAW): return VectorSpec{K::ShiftRightArithmetic, 16};
        PS5LIFT_BOTH(PSRAD): return VectorSpec{K::ShiftRightArithmetic, 32};
        case ZYDIS_MNEMONIC_VPSLLVD: return VectorSpec{K::ShiftLeftVariable, 32};
        case ZYDIS_MNEMONIC_VPSLLVQ: return VectorSpec{K::ShiftLeftVariable, 64};
        case ZYDIS_MNEMONIC_VPSRLVD: return VectorSpec{K::ShiftRightLogicalVariable, 32};
        case ZYDIS_MNEMONIC_VPSRLVQ: return VectorSpec{K::ShiftRightLogicalVariable, 64};
        case ZYDIS_MNEMONIC_VPSRAVD: return VectorSpec{K::ShiftRightArithmeticVariable, 32};
        PS5LIFT_BOTH(PSLLDQ): return VectorSpec{K::ShiftLeftBytes, 8};
        PS5LIFT_BOTH(PSRLDQ): return VectorSpec{K::ShiftRightBytes, 8};
        PS5LIFT_BOTH(PSHUFD): case ZYDIS_MNEMONIC_VPERMILPS: return VectorSpec{K::ShuffleDwords, 32};
        PS5LIFT_BOTH(PSHUFLW): return VectorSpec{K::ShuffleLowWords, 16};
        PS5LIFT_BOTH(PSHUFHW): return VectorSpec{K::ShuffleHighWords, 16};
        PS5LIFT_BOTH(SHUFPS): return VectorSpec{K::ShufflePairs, 32};
        PS5LIFT_BOTH(PUNPCKLBW): return VectorSpec{K::UnpackLow, 8};
        PS5LIFT_BOTH(PUNPCKLWD): return VectorSpec{K::UnpackLow, 16};
        PS5LIFT_BOTH(PUNPCKLDQ): PS5LIFT_BOTH(UNPCKLPS): return VectorSpec{K::UnpackLow, 32};
        PS5LIFT_BOTH(PUNPCKLQDQ): PS5LIFT_BOTH(UNPCKLPD): return VectorSpec{K::UnpackLow, 64};
        PS5LIFT_BOTH(PUNPCKHBW): return VectorSpec{K::UnpackHigh, 8};
        PS5LIFT_BOTH(PUNPCKHWD): return VectorSpec{K::UnpackHigh, 16};
        PS5LIFT_BOTH(PUNPCKHDQ): PS5LIFT_BOTH(UNPCKHPS): return VectorSpec{K::UnpackHigh, 32};
        PS5LIFT_BOTH(PUNPCKHQDQ): PS5LIFT_BOTH(UNPCKHPD): return VectorSpec{K::UnpackHigh, 64};
        PS5LIFT_BOTH(PACKSSWB): return VectorSpec{K::PackSignedSaturate, 8};
        PS5LIFT_BOTH(PACKSSDW): return VectorSpec{K::PackSignedSaturate, 16};
        PS5LIFT_BOTH(PACKUSWB): return VectorSpec{K::PackUnsignedSaturate, 8};
        PS5LIFT_BOTH(PACKUSDW): return VectorSpec{K::PackUnsignedSaturate, 16};
        PS5LIFT_BOTH(PALIGNR): return VectorSpec{K::AlignRight, 8};
        PS5LIFT_BOTH(PSHUFB): return VectorSpec{K::ShuffleBytes, 8};
        case ZYDIS_MNEMONIC_VPERMQ: case ZYDIS_MNEMONIC_VPERMPD: return VectorSpec{K::PermuteQwords, 64};
        case ZYDIS_MNEMONIC_VPERMD: case ZYDIS_MNEMONIC_VPERMPS: return VectorSpec{K::PermuteDwords, 32};
        case ZYDIS_MNEMONIC_VPERM2I128: case ZYDIS_MNEMONIC_VPERM2F128: return VectorSpec{K::Permute128, 64};
        case ZYDIS_MNEMONIC_VINSERTI128: case ZYDIS_MNEMONIC_VINSERTF128: return VectorSpec{K::Insert128, 128};
        case ZYDIS_MNEMONIC_VEXTRACTI128: case ZYDIS_MNEMONIC_VEXTRACTF128: return VectorSpec{K::Extract128, 128};
        case ZYDIS_MNEMONIC_VPBLENDD: PS5LIFT_BOTH(BLENDPS): return VectorSpec{K::BlendImmediate, 32};
        PS5LIFT_BOTH(BLENDPD): return VectorSpec{K::BlendImmediate, 64};
        PS5LIFT_BOTH(PBLENDW): return VectorSpec{K::BlendImmediate, 16};
        PS5LIFT_BOTH(PBLENDVB): return VectorSpec{K::BlendVariable, 8};
        PS5LIFT_BOTH(BLENDVPS): return VectorSpec{K::BlendVariable, 32};
        PS5LIFT_BOTH(BLENDVPD): return VectorSpec{K::BlendVariable, 64};
        PS5LIFT_BOTH(PMOVZXBW): return VectorSpec{K::ZeroExtend, 16, 8};
        PS5LIFT_BOTH(PMOVZXBD): return VectorSpec{K::ZeroExtend, 32, 8};
        PS5LIFT_BOTH(PMOVZXBQ): return VectorSpec{K::ZeroExtend, 64, 8};
        PS5LIFT_BOTH(PMOVZXWD): return VectorSpec{K::ZeroExtend, 32, 16};
        PS5LIFT_BOTH(PMOVZXWQ): return VectorSpec{K::ZeroExtend, 64, 16};
        PS5LIFT_BOTH(PMOVZXDQ): return VectorSpec{K::ZeroExtend, 64, 32};
        PS5LIFT_BOTH(PMOVSXBW): return VectorSpec{K::SignExtend, 16, 8};
        PS5LIFT_BOTH(PMOVSXBD): return VectorSpec{K::SignExtend, 32, 8};
        PS5LIFT_BOTH(PMOVSXBQ): return VectorSpec{K::SignExtend, 64, 8};
        PS5LIFT_BOTH(PMOVSXWD): return VectorSpec{K::SignExtend, 32, 16};
        PS5LIFT_BOTH(PMOVSXWQ): return VectorSpec{K::SignExtend, 64, 16};
        PS5LIFT_BOTH(PMOVSXDQ): return VectorSpec{K::SignExtend, 64, 32};
        PS5LIFT_BOTH(PMOVMSKB): return VectorSpec{K::MoveMask, 8};
        PS5LIFT_BOTH(MOVMSKPS): return VectorSpec{K::MoveMask, 32};
        PS5LIFT_BOTH(MOVMSKPD): return VectorSpec{K::MoveMask, 64};
        PS5LIFT_BOTH(PEXTRB): return VectorSpec{K::Extract, 8};
        PS5LIFT_BOTH(PEXTRW): return VectorSpec{K::Extract, 16};
        PS5LIFT_BOTH(PEXTRD): PS5LIFT_BOTH(EXTRACTPS): return VectorSpec{K::Extract, 32};
        PS5LIFT_BOTH(PEXTRQ): return VectorSpec{K::Extract, 64};
        PS5LIFT_BOTH(PINSRB): return VectorSpec{K::Insert, 8};
        PS5LIFT_BOTH(PINSRW): return VectorSpec{K::Insert, 16};
        PS5LIFT_BOTH(PINSRD): return VectorSpec{K::Insert, 32};
        PS5LIFT_BOTH(PINSRQ): return VectorSpec{K::Insert, 64};
        PS5LIFT_BOTH(PTEST): return VectorSpec{K::Test, 64};
        default: return std::nullopt;
        }
#undef PS5LIFT_BOTH
    }

    llvm::Value* AsVector(llvm::Value* value, const unsigned element) {
        const auto bits = value->getType()->getIntegerBitWidth();
        return _builder.CreateBitCast(value, llvm::FixedVectorType::get(Int(element), bits / element));
    }

    llvm::Value* AsInteger(llvm::Value* vector) {
        return _builder.CreateBitCast(vector, Int(vector->getType()->getPrimitiveSizeInBits().getFixedValue()));
    }

    llvm::Value* Splat(llvm::Value* vector, llvm::Value* scalar) {
        return _builder.CreateVectorSplat(llvm::cast<llvm::FixedVectorType>(vector->getType())->getNumElements(), scalar);
    }

    llvm::Value* ShuffleWithZero(llvm::Value* a, llvm::Value* b, const std::vector<int>& mask) {
        const auto count = static_cast<int>(mask.size());
        std::vector<int> picked(mask.size());
        std::vector<int> zeroed(mask.size());
        bool anyZero = false;
        for (int i = 0; i < count; ++i) {
            picked[i] = mask[i] < 0 ? 0 : mask[i];
            zeroed[i] = mask[i] < 0 ? count + i : i;
            anyZero = anyZero || mask[i] < 0;
        }
        auto* result = _builder.CreateShuffleVector(a, b, picked);
        if (!anyZero) return result;
        return _builder.CreateShuffleVector(result, llvm::Constant::getNullValue(result->getType()), zeroed);
    }

    std::uint64_t Immediate(const int index) const {
        const auto& operand = Operand(index);
        if (operand.type != ZYDIS_OPERAND_TYPE_IMMEDIATE) throw UnsupportedInstruction("expected an immediate");
        return operand.imm.value.u;
    }

    int Base(const int legacyVisible) const {
        return _instruction->Info.operand_count_visible > legacyVisible ? 1 : 0;
    }

    llvm::Value* ShiftByCount(const VectorKind kind, llvm::Value* vector, llvm::Value* counts, const unsigned element) {
        auto* limit = Splat(vector, llvm::ConstantInt::get(Int(element), element - 1));
        auto* clamped = _builder.CreateBinaryIntrinsic(llvm::Intrinsic::umin, counts, limit);
        auto* tooLarge = _builder.CreateICmpUGT(counts, limit);
        auto* zero = llvm::Constant::getNullValue(vector->getType());
        switch (kind) {
        case VectorKind::ShiftLeft:
        case VectorKind::ShiftLeftVariable:
            return _builder.CreateSelect(tooLarge, zero, _builder.CreateShl(vector, clamped));
        case VectorKind::ShiftRightLogical:
        case VectorKind::ShiftRightLogicalVariable:
            return _builder.CreateSelect(tooLarge, zero, _builder.CreateLShr(vector, clamped));
        default:
            return _builder.CreateAShr(vector, clamped);
        }
    }

    llvm::Value* Saturate(llvm::Value* wide, const unsigned element, const bool isSigned) {
        auto* type = wide->getType();
        const auto wideBits = llvm::cast<llvm::FixedVectorType>(type)->getElementType()->getIntegerBitWidth();
        const auto low = isSigned ? llvm::APInt::getSignedMinValue(element).sext(wideBits) : llvm::APInt::getZero(wideBits);
        const auto high = isSigned ? llvm::APInt::getSignedMaxValue(element).sext(wideBits) : llvm::APInt::getMaxValue(element).zext(wideBits);
        auto* clamped = _builder.CreateBinaryIntrinsic(llvm::Intrinsic::smax, wide, llvm::ConstantInt::get(type, low));
        clamped = _builder.CreateBinaryIntrinsic(llvm::Intrinsic::smin, clamped, llvm::ConstantInt::get(type, high));
        return _builder.CreateTrunc(clamped, llvm::FixedVectorType::get(Int(element), llvm::cast<llvm::FixedVectorType>(type)->getNumElements()));
    }

    bool EmitVector(const ZydisMnemonic mnemonic) {
        const auto spec = VectorSpecOf(mnemonic);
        if (!spec) return false;
        const auto element = spec->Element;
        const auto kind = spec->Kind;
        using K = VectorKind;

        if (kind == K::MoveMask) {
            auto* source = Read(1);
            auto* negative = _builder.CreateICmpSLT(AsVector(source, element), llvm::Constant::getNullValue(AsVector(source, element)->getType()));
            auto* bits = _builder.CreateBitCast(negative, Int(source->getType()->getIntegerBitWidth() / element));
            Write(0, _builder.CreateZExt(bits, Int(Bits(0))));
            return true;
        }
        if (kind == K::Extract) {
            auto* source = AsVector(ReadRegister(Operand(1).reg.value), element);
            const auto count = llvm::cast<llvm::FixedVectorType>(source->getType())->getNumElements();
            auto* value = _builder.CreateExtractElement(source, _builder.getInt64(Immediate(2) & (std::min(count, 128 / element) - 1)));
            Write(0, _builder.CreateZExtOrTrunc(value, Int(Bits(0))));
            return true;
        }
        if (kind == K::Extract128) {
            auto* source = AsVector(ReadRegister(Operand(1).reg.value), 128);
            Write(0, _builder.CreateExtractElement(source, _builder.getInt64(Immediate(2) & 1)));
            return true;
        }
        if (kind == K::Test) {
            auto* a = Read(0);
            auto* b = ReadAs(1, a->getType()->getIntegerBitWidth());
            auto* zero = llvm::ConstantInt::get(a->getType(), 0);
            SetFlag(PS5LIFT_ZF, _builder.CreateICmpEQ(_builder.CreateAnd(a, b), zero));
            SetFlag(PS5LIFT_CF, _builder.CreateICmpEQ(_builder.CreateAnd(_builder.CreateNot(a), b), zero));
            for (const auto flag : {PS5LIFT_PF, PS5LIFT_AF, PS5LIFT_SF, PS5LIFT_OF}) SetFlag(flag, _builder.getFalse());
            return true;
        }

        const auto width = Bits(0);
        const unsigned lanes = width / 128;
        const unsigned count = width / element;
        const unsigned perLane = 128 / element;
        llvm::Value* result = nullptr;

        switch (kind) {
        case K::Absolute:
            result = _builder.CreateBinaryIntrinsic(llvm::Intrinsic::abs, AsVector(ReadAs(1, width), element), _builder.getFalse());
            break;
        case K::ZeroExtend:
        case K::SignExtend: {
            auto* source = Read(1);
            auto* narrow = _builder.CreateZExtOrTrunc(source, Int(count * spec->Source));
            auto* vector = AsVector(narrow, spec->Source);
            auto* target = llvm::FixedVectorType::get(Int(element), count);
            result = kind == K::ZeroExtend ? _builder.CreateZExt(vector, target) : _builder.CreateSExt(vector, target);
            break;
        }
        case K::ShuffleDwords:
        case K::ShuffleLowWords:
        case K::ShuffleHighWords: {
            auto* source = AsVector(ReadAs(1, width), element);
            const auto control = Immediate(2);
            std::vector<int> mask(count);
            for (unsigned lane = 0; lane < lanes; ++lane) {
                for (unsigned i = 0; i < perLane; ++i) {
                    const auto base = lane * perLane;
                    int picked = static_cast<int>(base + i);
                    if (kind == K::ShuffleDwords) picked = static_cast<int>(base + ((control >> (2 * i)) & 3));
                    if (kind == K::ShuffleLowWords && i < 4) picked = static_cast<int>(base + ((control >> (2 * i)) & 3));
                    if (kind == K::ShuffleHighWords && i >= 4) picked = static_cast<int>(base + 4 + ((control >> (2 * (i - 4))) & 3));
                    mask[base + i] = picked;
                }
            }
            result = _builder.CreateShuffleVector(source, source, mask);
            break;
        }
        case K::PermuteQwords: {
            auto* source = AsVector(ReadAs(1, width), element);
            const auto control = Immediate(2);
            std::vector<int> mask(count);
            for (unsigned i = 0; i < count; ++i) mask[i] = static_cast<int>((control >> (2 * i)) & 3);
            result = _builder.CreateShuffleVector(source, source, mask);
            break;
        }
        case K::ShiftLeftBytes:
        case K::ShiftRightBytes: {
            const int base = Base(2);
            auto* source = AsVector(ReadAs(base, width), 8);
            const auto amount = static_cast<int>(std::min<std::uint64_t>(Immediate(base + 1), 16));
            std::vector<int> mask(count);
            for (unsigned lane = 0; lane < lanes; ++lane) {
                for (int i = 0; i < 16; ++i) {
                    const int from = kind == K::ShiftLeftBytes ? i - amount : i + amount;
                    mask[lane * 16 + i] = from >= 0 && from < 16 ? static_cast<int>(lane * 16) + from : -1;
                }
            }
            result = ShuffleWithZero(source, source, mask);
            break;
        }
        case K::ShiftLeft:
        case K::ShiftRightLogical:
        case K::ShiftRightArithmetic: {
            const int base = Base(2);
            auto* vector = AsVector(ReadAs(base, width), element);
            llvm::Value* amount = nullptr;
            if (Operand(base + 1).type == ZYDIS_OPERAND_TYPE_IMMEDIATE) amount = _builder.getInt64(Immediate(base + 1) & 0xff);
            else amount = _builder.CreateTrunc(ReadAs(base + 1, 128), I64());
            auto* clampedScalar = _builder.CreateBinaryIntrinsic(llvm::Intrinsic::umin, amount, _builder.getInt64(element));
            result = ShiftByCount(kind, vector, Splat(vector, _builder.CreateTrunc(clampedScalar, Int(element))), element);
            break;
        }
        default: {
            const bool hasImmediate = kind == K::AlignRight || kind == K::BlendImmediate || kind == K::Permute128 || kind == K::Insert128 || kind == K::ShufflePairs || kind == K::Insert;
            const int base = Base(hasImmediate ? 3 : (kind == K::BlendVariable ? (_instruction->Info.encoding == ZYDIS_INSTRUCTION_ENCODING_VEX ? 3 : 2) : 2));
            auto* a = ReadAs(base, width);
            auto* rawB = Read(base + 1);
            auto* b = kind == K::Insert || kind == K::Insert128 ? rawB : _builder.CreateZExtOrTrunc(rawB, Int(width));
            auto* va = AsVector(a, element);
            auto* vb = kind == K::Insert || kind == K::Insert128 ? nullptr : AsVector(b, element);
            switch (kind) {
            case K::Add: result = _builder.CreateAdd(va, vb); break;
            case K::Sub: result = _builder.CreateSub(va, vb); break;
            case K::AddSignedSaturate: result = _builder.CreateBinaryIntrinsic(llvm::Intrinsic::sadd_sat, va, vb); break;
            case K::AddUnsignedSaturate: result = _builder.CreateBinaryIntrinsic(llvm::Intrinsic::uadd_sat, va, vb); break;
            case K::SubSignedSaturate: result = _builder.CreateBinaryIntrinsic(llvm::Intrinsic::ssub_sat, va, vb); break;
            case K::SubUnsignedSaturate: result = _builder.CreateBinaryIntrinsic(llvm::Intrinsic::usub_sat, va, vb); break;
            case K::MultiplyLow: result = _builder.CreateMul(va, vb); break;
            case K::MinSigned: result = _builder.CreateBinaryIntrinsic(llvm::Intrinsic::smin, va, vb); break;
            case K::MaxSigned: result = _builder.CreateBinaryIntrinsic(llvm::Intrinsic::smax, va, vb); break;
            case K::MinUnsigned: result = _builder.CreateBinaryIntrinsic(llvm::Intrinsic::umin, va, vb); break;
            case K::MaxUnsigned: result = _builder.CreateBinaryIntrinsic(llvm::Intrinsic::umax, va, vb); break;
            case K::CompareEqual: result = _builder.CreateSExt(_builder.CreateICmpEQ(va, vb), va->getType()); break;
            case K::CompareGreater: result = _builder.CreateSExt(_builder.CreateICmpSGT(va, vb), va->getType()); break;
            case K::AndNot: result = _builder.CreateAnd(_builder.CreateNot(va), vb); break;
            case K::MultiplyHighSigned:
            case K::MultiplyHighUnsigned: {
                auto* wide = llvm::FixedVectorType::get(Int(element * 2), count);
                const bool isSigned = kind == K::MultiplyHighSigned;
                auto* product = _builder.CreateMul(isSigned ? _builder.CreateSExt(va, wide) : _builder.CreateZExt(va, wide), isSigned ? _builder.CreateSExt(vb, wide) : _builder.CreateZExt(vb, wide));
                result = _builder.CreateTrunc(_builder.CreateLShr(product, element), va->getType());
                break;
            }
            case K::MultiplyEvenUnsigned:
            case K::MultiplyEvenSigned: {
                auto* shift = Splat(va, _builder.getInt64(32));
                auto* left = kind == K::MultiplyEvenUnsigned ? _builder.CreateAnd(va, Splat(va, _builder.getInt64(0xffffffff))) : _builder.CreateAShr(_builder.CreateShl(va, shift), shift);
                auto* right = kind == K::MultiplyEvenUnsigned ? _builder.CreateAnd(vb, Splat(vb, _builder.getInt64(0xffffffff))) : _builder.CreateAShr(_builder.CreateShl(vb, shift), shift);
                result = _builder.CreateMul(left, right);
                break;
            }
            case K::MultiplyAddWords: {
                auto* words = llvm::FixedVectorType::get(Int(32), count * 2);
                auto* product = _builder.CreateMul(_builder.CreateSExt(AsVector(a, 16), words), _builder.CreateSExt(AsVector(b, 16), words));
                std::vector<int> even(count);
                std::vector<int> odd(count);
                for (unsigned i = 0; i < count; ++i) {
                    even[i] = static_cast<int>(2 * i);
                    odd[i] = static_cast<int>(2 * i + 1);
                }
                result = _builder.CreateAdd(_builder.CreateShuffleVector(product, product, even), _builder.CreateShuffleVector(product, product, odd));
                break;
            }
            case K::Average: {
                auto* wide = llvm::FixedVectorType::get(Int(element * 2), count);
                auto* sum = _builder.CreateAdd(_builder.CreateAdd(_builder.CreateZExt(va, wide), _builder.CreateZExt(vb, wide)), llvm::ConstantInt::get(wide, 1));
                result = _builder.CreateTrunc(_builder.CreateLShr(sum, 1), va->getType());
                break;
            }
            case K::ShiftLeftVariable:
            case K::ShiftRightLogicalVariable:
            case K::ShiftRightArithmeticVariable:
                result = ShiftByCount(kind, va, vb, element);
                break;
            case K::UnpackLow:
            case K::UnpackHigh: {
                std::vector<int> mask(count);
                for (unsigned lane = 0; lane < lanes; ++lane) {
                    for (unsigned i = 0; i < perLane / 2; ++i) {
                        const auto from = lane * perLane + (kind == K::UnpackHigh ? perLane / 2 : 0) + i;
                        mask[lane * perLane + 2 * i] = static_cast<int>(from);
                        mask[lane * perLane + 2 * i + 1] = static_cast<int>(count + from);
                    }
                }
                result = _builder.CreateShuffleVector(va, vb, mask);
                break;
            }
            case K::ShufflePairs: {
                const auto control = Immediate(base + 2);
                std::vector<int> mask(count);
                for (unsigned lane = 0; lane < lanes; ++lane) {
                    for (unsigned i = 0; i < 4; ++i) {
                        const auto selected = static_cast<int>(lane * 4 + ((control >> (2 * i)) & 3));
                        mask[lane * 4 + i] = i < 2 ? selected : static_cast<int>(count) + selected;
                    }
                }
                result = _builder.CreateShuffleVector(va, vb, mask);
                break;
            }
            case K::PackSignedSaturate:
            case K::PackUnsignedSaturate: {
                auto* wa = AsVector(a, element * 2);
                auto* wb = AsVector(b, element * 2);
                auto* na = Saturate(wa, element, kind == K::PackSignedSaturate);
                auto* nb = Saturate(wb, element, kind == K::PackSignedSaturate);
                const unsigned half = perLane / 2;
                const unsigned narrowCount = count / 2;
                std::vector<int> mask(count);
                for (unsigned lane = 0; lane < lanes; ++lane) {
                    for (unsigned i = 0; i < half; ++i) {
                        mask[lane * perLane + i] = static_cast<int>(lane * half + i);
                        mask[lane * perLane + half + i] = static_cast<int>(narrowCount + lane * half + i);
                    }
                }
                result = _builder.CreateShuffleVector(na, nb, mask);
                break;
            }
            case K::AlignRight: {
                const auto amount = static_cast<int>(Immediate(base + 2) & 0xff);
                std::vector<int> mask(count);
                for (unsigned lane = 0; lane < lanes; ++lane) {
                    for (int i = 0; i < 16; ++i) {
                        const int from = i + amount;
                        const int laneBase = static_cast<int>(lane * 16);
                        mask[laneBase + i] = from < 16 ? static_cast<int>(count) + laneBase + from : from < 32 ? laneBase + from - 16 : -1;
                    }
                }
                result = ShuffleWithZero(va, vb, mask);
                break;
            }
            case K::ShuffleBytes: {
                result = llvm::Constant::getNullValue(va->getType());
                for (unsigned i = 0; i < count; ++i) {
                    auto* selector = _builder.CreateExtractElement(vb, _builder.getInt64(i));
                    auto* index = _builder.CreateAdd(_builder.CreateZExt(_builder.CreateAnd(selector, 15), I64()), _builder.getInt64((i / 16) * 16));
                    auto* picked = _builder.CreateExtractElement(va, index);
                    auto* value = _builder.CreateSelect(_builder.CreateICmpSLT(selector, _builder.getInt8(0)), _builder.getInt8(0), picked);
                    result = _builder.CreateInsertElement(result, value, _builder.getInt64(i));
                }
                break;
            }
            case K::PermuteDwords: {
                result = llvm::Constant::getNullValue(va->getType());
                for (unsigned i = 0; i < count; ++i) {
                    auto* index = _builder.CreateAnd(_builder.CreateExtractElement(va, _builder.getInt64(i)), count - 1);
                    result = _builder.CreateInsertElement(result, _builder.CreateExtractElement(vb, index), _builder.getInt64(i));
                }
                break;
            }
            case K::Permute128: {
                const auto control = Immediate(base + 2);
                std::vector<int> mask(4);
                for (int half = 0; half < 2; ++half) {
                    const auto selector = (control >> (4 * half)) & 0xf;
                    for (int i = 0; i < 2; ++i) {
                        const int source = static_cast<int>(selector & 3);
                        const int from = (source & 2 ? 4 : 0) + (source & 1) * 2 + i;
                        mask[half * 2 + i] = selector & 8 ? -1 : from;
                    }
                }
                result = ShuffleWithZero(va, vb, mask);
                break;
            }
            case K::Insert128:
                result = _builder.CreateInsertElement(AsVector(a, 128), _builder.CreateZExtOrTrunc(rawB, Int(128)), _builder.getInt64(Immediate(base + 2) & 1));
                break;
            case K::Insert: {
                const auto index = Immediate(base + 2) & (count - 1);
                result = _builder.CreateInsertElement(va, _builder.CreateZExtOrTrunc(rawB, Int(element)), _builder.getInt64(index));
                break;
            }
            case K::BlendImmediate: {
                const auto control = Immediate(base + 2);
                std::vector<int> mask(count);
                for (unsigned i = 0; i < count; ++i) {
                    const auto bit = element == 16 ? i % 8 : i;
                    mask[i] = (control >> bit) & 1 ? static_cast<int>(count + i) : static_cast<int>(i);
                }
                result = _builder.CreateShuffleVector(va, vb, mask);
                break;
            }
            case K::BlendVariable: {
                auto* selector = _instruction->Info.encoding == ZYDIS_INSTRUCTION_ENCODING_VEX ? ReadAs(base + 2, width) : _builder.CreateTrunc(ReadRegister(ZYDIS_REGISTER_XMM0), Int(width));
                auto* negative = _builder.CreateICmpSLT(AsVector(selector, element), llvm::Constant::getNullValue(va->getType()));
                result = _builder.CreateSelect(negative, vb, va);
                break;
            }
            default:
                throw UnsupportedInstruction("vector form");
            }
            break;
        }
        }
        Write(0, AsInteger(result));
        return true;
    }

    enum class FloatKind {
        Add, Sub, Mul, Div, Min, Max, Sqrt, Reciprocal, ReciprocalSqrt, Round, Compare, AddSub, HorizontalAdd, HorizontalSub, Fma
    };

    struct FloatSpec {
        FloatKind Kind;
        unsigned Element;
        bool Scalar;
        int Order = 0;
        bool NegateProduct = false;
        bool SubtractAddend = false;
        int Alternate = 0;
    };

    static std::optional<FloatSpec> FloatSpecOf(const ZydisMnemonic mnemonic) {
        using K = FloatKind;
#define PS5LIFT_FLOAT4(name, kind)                                                       \
    case ZYDIS_MNEMONIC_##name##PS: case ZYDIS_MNEMONIC_V##name##PS: return FloatSpec{kind, 32, false}; \
    case ZYDIS_MNEMONIC_##name##PD: case ZYDIS_MNEMONIC_V##name##PD: return FloatSpec{kind, 64, false}; \
    case ZYDIS_MNEMONIC_##name##SS: case ZYDIS_MNEMONIC_V##name##SS: return FloatSpec{kind, 32, true};  \
    case ZYDIS_MNEMONIC_##name##SD: case ZYDIS_MNEMONIC_V##name##SD: return FloatSpec{kind, 64, true}
#define PS5LIFT_FMA(prefix, suffix, negate, subtract, alternate)                                                       \
    case ZYDIS_MNEMONIC_##prefix##132##suffix: return FloatSpec{K::Fma, Element(#suffix), Scalar(#suffix), 132, negate, subtract, alternate}; \
    case ZYDIS_MNEMONIC_##prefix##213##suffix: return FloatSpec{K::Fma, Element(#suffix), Scalar(#suffix), 213, negate, subtract, alternate}; \
    case ZYDIS_MNEMONIC_##prefix##231##suffix: return FloatSpec{K::Fma, Element(#suffix), Scalar(#suffix), 231, negate, subtract, alternate}
        constexpr auto Element = [](const char* suffix) { return suffix[1] == 'S' ? 32u : 64u; };
        constexpr auto Scalar = [](const char* suffix) { return suffix[0] == 'S'; };
        switch (mnemonic) {
        PS5LIFT_FLOAT4(ADD, K::Add);
        PS5LIFT_FLOAT4(SUB, K::Sub);
        PS5LIFT_FLOAT4(MUL, K::Mul);
        PS5LIFT_FLOAT4(DIV, K::Div);
        PS5LIFT_FLOAT4(MIN, K::Min);
        PS5LIFT_FLOAT4(MAX, K::Max);
        PS5LIFT_FLOAT4(SQRT, K::Sqrt);
        PS5LIFT_FLOAT4(ROUND, K::Round);
        PS5LIFT_FLOAT4(CMP, K::Compare);
        case ZYDIS_MNEMONIC_RCPPS: case ZYDIS_MNEMONIC_VRCPPS: return FloatSpec{K::Reciprocal, 32, false};
        case ZYDIS_MNEMONIC_RCPSS: case ZYDIS_MNEMONIC_VRCPSS: return FloatSpec{K::Reciprocal, 32, true};
        case ZYDIS_MNEMONIC_RSQRTPS: case ZYDIS_MNEMONIC_VRSQRTPS: return FloatSpec{K::ReciprocalSqrt, 32, false};
        case ZYDIS_MNEMONIC_RSQRTSS: case ZYDIS_MNEMONIC_VRSQRTSS: return FloatSpec{K::ReciprocalSqrt, 32, true};
        case ZYDIS_MNEMONIC_ADDSUBPS: case ZYDIS_MNEMONIC_VADDSUBPS: return FloatSpec{K::AddSub, 32, false};
        case ZYDIS_MNEMONIC_ADDSUBPD: case ZYDIS_MNEMONIC_VADDSUBPD: return FloatSpec{K::AddSub, 64, false};
        case ZYDIS_MNEMONIC_HADDPS: case ZYDIS_MNEMONIC_VHADDPS: return FloatSpec{K::HorizontalAdd, 32, false};
        case ZYDIS_MNEMONIC_HADDPD: case ZYDIS_MNEMONIC_VHADDPD: return FloatSpec{K::HorizontalAdd, 64, false};
        case ZYDIS_MNEMONIC_HSUBPS: case ZYDIS_MNEMONIC_VHSUBPS: return FloatSpec{K::HorizontalSub, 32, false};
        case ZYDIS_MNEMONIC_HSUBPD: case ZYDIS_MNEMONIC_VHSUBPD: return FloatSpec{K::HorizontalSub, 64, false};
        PS5LIFT_FMA(VFMADD, PS, false, false, 0);
        PS5LIFT_FMA(VFMADD, PD, false, false, 0);
        PS5LIFT_FMA(VFMADD, SS, false, false, 0);
        PS5LIFT_FMA(VFMADD, SD, false, false, 0);
        PS5LIFT_FMA(VFMSUB, PS, false, true, 0);
        PS5LIFT_FMA(VFMSUB, PD, false, true, 0);
        PS5LIFT_FMA(VFMSUB, SS, false, true, 0);
        PS5LIFT_FMA(VFMSUB, SD, false, true, 0);
        PS5LIFT_FMA(VFNMADD, PS, true, false, 0);
        PS5LIFT_FMA(VFNMADD, PD, true, false, 0);
        PS5LIFT_FMA(VFNMADD, SS, true, false, 0);
        PS5LIFT_FMA(VFNMADD, SD, true, false, 0);
        PS5LIFT_FMA(VFNMSUB, PS, true, true, 0);
        PS5LIFT_FMA(VFNMSUB, PD, true, true, 0);
        PS5LIFT_FMA(VFNMSUB, SS, true, true, 0);
        PS5LIFT_FMA(VFNMSUB, SD, true, true, 0);
        PS5LIFT_FMA(VFMADDSUB, PS, false, false, 1);
        PS5LIFT_FMA(VFMADDSUB, PD, false, false, 1);
        PS5LIFT_FMA(VFMSUBADD, PS, false, false, 2);
        PS5LIFT_FMA(VFMSUBADD, PD, false, false, 2);
        default: return std::nullopt;
        }
#undef PS5LIFT_FLOAT4
#undef PS5LIFT_FMA
    }

    llvm::Type* FloatType(const unsigned bits) {
        if (bits == 16) return _builder.getHalfTy();
        return bits == 32 ? _builder.getFloatTy() : _builder.getDoubleTy();
    }

    llvm::Value* AsFloats(llvm::Value* value, const unsigned element) {
        const auto bits = value->getType()->getIntegerBitWidth();
        if (bits == element) return _builder.CreateBitCast(value, FloatType(element));
        return _builder.CreateBitCast(value, llvm::FixedVectorType::get(FloatType(element), bits / element));
    }

    llvm::Value* FloatBits(llvm::Value* value) {
        return _builder.CreateBitCast(value, Int(value->getType()->getPrimitiveSizeInBits().getFixedValue()));
    }

    llvm::Type* IntegerLike(llvm::Type* type) {
        const auto element = type->getScalarSizeInBits();
        if (auto* vector = llvm::dyn_cast<llvm::FixedVectorType>(type)) return llvm::FixedVectorType::get(Int(element), vector->getNumElements());
        return Int(element);
    }

    llvm::Value* Quiet(llvm::Value* value) {
        const auto element = value->getType()->getScalarSizeInBits();
        auto* integerType = IntegerLike(value->getType());
        auto* bit = llvm::ConstantInt::get(integerType, std::uint64_t{1} << (element == 32 ? 22 : element == 64 ? 51 : 9));
        return _builder.CreateBitCast(_builder.CreateOr(_builder.CreateBitCast(value, integerType), bit), value->getType());
    }

    llvm::Value* DefaultNan(llvm::Type* type) {
        const auto element = type->getScalarSizeInBits();
        const auto bits = element == 32 ? std::uint64_t{0xffc00000} : element == 64 ? std::uint64_t{0xfff8000000000000} : std::uint64_t{0xfe00};
        return _builder.CreateBitCast(llvm::ConstantInt::get(IntegerLike(type), bits), type);
    }

    llvm::Value* IsNan(llvm::Value* value) {
        return _builder.CreateFCmpUNO(value, value);
    }

    bool ExactFloat() const {
        return _module.Options().Float == FloatModel::Exact;
    }

    llvm::Value* NanResult(llvm::Value* result, std::initializer_list<llvm::Value*> sources) {
        if (!ExactFloat()) return result;
        llvm::Value* value = _builder.CreateSelect(IsNan(result), DefaultNan(result->getType()), result);
        std::vector<llvm::Value*> ordered(sources);
        for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) value = _builder.CreateSelect(IsNan(*it), Quiet(*it), value);
        return value;
    }

    llvm::Value* DenormalsAreZero(llvm::Value* value) {
        if (!ExactFloat()) return value;
        const auto element = value->getType()->getScalarSizeInBits();
        auto* integerType = IntegerLike(value->getType());
        auto* bits = _builder.CreateBitCast(value, integerType);
        const auto mantissa = element == 32 ? std::uint64_t{0x7fffff} : std::uint64_t{0xfffffffffffff};
        const auto exponent = element == 32 ? std::uint64_t{0x7f800000} : std::uint64_t{0x7ff0000000000000};
        auto* zero = llvm::Constant::getNullValue(integerType);
        auto* denormal = _builder.CreateAnd(_builder.CreateICmpEQ(_builder.CreateAnd(bits, llvm::ConstantInt::get(integerType, exponent)), zero), _builder.CreateICmpNE(_builder.CreateAnd(bits, llvm::ConstantInt::get(integerType, mantissa)), zero));
        auto* mxcsr = StateLoad(_builder.getInt32Ty(), offsetof(Ps5LiftState, Mxcsr));
        llvm::Value* enabled = _builder.CreateICmpNE(_builder.CreateAnd(mxcsr, 0x40), _builder.getInt32(0));
        if (value->getType()->isVectorTy()) enabled = _builder.CreateVectorSplat(llvm::cast<llvm::FixedVectorType>(value->getType())->getNumElements(), enabled);
        auto* signOnly = _builder.CreateAnd(bits, llvm::ConstantInt::get(integerType, std::uint64_t{1} << (element - 1)));
        return _builder.CreateBitCast(_builder.CreateSelect(_builder.CreateAnd(enabled, denormal), signOnly, bits), value->getType());
    }

    llvm::Value* FloatConstant(llvm::Type* type, const double value) {
        return llvm::ConstantFP::get(type, value);
    }

    llvm::Value* Arithmetic(const FloatKind kind, llvm::Value* a, llvm::Value* b) {
        switch (kind) {
        case FloatKind::Add: return NanResult(_builder.CreateFAdd(a, b), {a, b});
        case FloatKind::Sub: return NanResult(_builder.CreateFSub(a, b), {a, b});
        case FloatKind::Mul: return NanResult(_builder.CreateFMul(a, b), {a, b});
        case FloatKind::Div: return NanResult(_builder.CreateFDiv(a, b), {a, b});
        case FloatKind::Min:
        case FloatKind::Max: {
            auto* left = DenormalsAreZero(a);
            auto* right = DenormalsAreZero(b);
            auto* compare = kind == FloatKind::Min ? _builder.CreateFCmpOLT(left, right) : _builder.CreateFCmpOGT(left, right);
            return _builder.CreateSelect(compare, left, right);
        }
        case FloatKind::Sqrt: return NanResult(_builder.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, b), {b});
        case FloatKind::Reciprocal: return NanResult(_builder.CreateFDiv(FloatConstant(b->getType(), 1.0), b), {b});
        case FloatKind::ReciprocalSqrt: return NanResult(_builder.CreateFDiv(FloatConstant(b->getType(), 1.0), _builder.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, b)), {b});
        default: throw std::logic_error("not an arithmetic kind");
        }
    }

    llvm::Value* RoundValue(llvm::Value* value, const std::uint64_t control) {
        llvm::Intrinsic::ID intrinsic = llvm::Intrinsic::rint;
        if ((control & 4) == 0) {
            switch (control & 3) {
            case 0: intrinsic = llvm::Intrinsic::roundeven; break;
            case 1: intrinsic = llvm::Intrinsic::floor; break;
            case 2: intrinsic = llvm::Intrinsic::ceil; break;
            default: intrinsic = llvm::Intrinsic::trunc; break;
            }
        }
        return NanResult(_builder.CreateUnaryIntrinsic(intrinsic, value), {value});
    }

    llvm::Value* CompareMask(llvm::Value* a, llvm::Value* b, const std::uint64_t predicate) {
        static constexpr llvm::CmpInst::Predicate Predicates[16] = {
            llvm::CmpInst::FCMP_OEQ, llvm::CmpInst::FCMP_OLT, llvm::CmpInst::FCMP_OLE, llvm::CmpInst::FCMP_UNO,
            llvm::CmpInst::FCMP_UNE, llvm::CmpInst::FCMP_UGE, llvm::CmpInst::FCMP_UGT, llvm::CmpInst::FCMP_ORD,
            llvm::CmpInst::FCMP_UEQ, llvm::CmpInst::FCMP_ULT, llvm::CmpInst::FCMP_ULE, llvm::CmpInst::FCMP_FALSE,
            llvm::CmpInst::FCMP_ONE, llvm::CmpInst::FCMP_OGE, llvm::CmpInst::FCMP_OGT, llvm::CmpInst::FCMP_TRUE};
        auto* compare = _builder.CreateFCmp(Predicates[predicate & 15], a, b);
        return _builder.CreateBitCast(_builder.CreateSExt(compare, IntegerLike(a->getType())), a->getType());
    }

    llvm::Value* FusedMultiplyAdd(const FloatSpec& spec, llvm::Value* x1, llvm::Value* x2, llvm::Value* x3) {
        llvm::Value* left = nullptr;
        llvm::Value* right = nullptr;
        llvm::Value* addend = nullptr;
        if (spec.Order == 132) {
            left = x1;
            right = x3;
            addend = x2;
        } else if (spec.Order == 213) {
            left = x2;
            right = x1;
            addend = x3;
        } else {
            left = x2;
            right = x3;
            addend = x1;
        }
        auto* type = left->getType();
        auto* product = spec.NegateProduct ? _builder.CreateFNeg(left) : left;
        auto* add = _builder.CreateIntrinsic(llvm::Intrinsic::fma, {type}, {product, right, addend});
        auto* subtract = _builder.CreateIntrinsic(llvm::Intrinsic::fma, {type}, {product, right, _builder.CreateFNeg(addend)});
        llvm::Value* result = spec.SubtractAddend ? subtract : add;
        if (spec.Alternate != 0) {
            const auto count = llvm::cast<llvm::FixedVectorType>(type)->getNumElements();
            std::vector<int> mask(count);
            for (unsigned i = 0; i < count; ++i) {
                const bool even = i % 2 == 0;
                const bool useSubtract = spec.Alternate == 1 ? even : !even;
                mask[i] = static_cast<int>(useSubtract ? i : count + i);
            }
            result = _builder.CreateShuffleVector(subtract, add, mask);
        }
        return NanResult(result, {left, right, addend});
    }

    llvm::Value* LoadScalarFloat(const int index, const unsigned element) {
        return _builder.CreateBitCast(ReadAs(index, element), FloatType(element));
    }

    void EmitFloatArithmetic(const FloatSpec& spec) {
        const auto element = spec.Element;
        const auto kind = spec.Kind;
        const bool unary = kind == FloatKind::Sqrt || kind == FloatKind::Reciprocal || kind == FloatKind::ReciprocalSqrt;
        if (spec.Scalar) {
            int first = 0;
            int second = 1;
            if (kind == FloatKind::Fma) {
                auto* destination = AsFloats(ReadAs(0, 128), element);
                auto* x1 = _builder.CreateExtractElement(destination, _builder.getInt64(0));
                auto* x2 = _builder.CreateExtractElement(AsFloats(ReadAs(1, 128), element), _builder.getInt64(0));
                auto* x3 = LoadScalarFloat(2, element);
                Write(0, FloatBits(_builder.CreateInsertElement(destination, FusedMultiplyAdd(spec, x1, x2, x3), _builder.getInt64(0))));
                return;
            }
            const bool withImmediate = kind == FloatKind::Round || kind == FloatKind::Compare;
            first = Base(withImmediate ? 3 : 2);
            second = first + 1;
            auto* source = AsFloats(ReadAs(first, 128), element);
            auto* a = _builder.CreateExtractElement(source, _builder.getInt64(0));
            auto* b = LoadScalarFloat(second, element);
            llvm::Value* value = nullptr;
            if (kind == FloatKind::Round) value = RoundValue(b, Immediate(second + 1));
            else if (kind == FloatKind::Compare) value = CompareMask(a, b, Immediate(second + 1));
            else value = Arithmetic(kind, a, b);
            Write(0, FloatBits(_builder.CreateInsertElement(source, value, _builder.getInt64(0))));
            return;
        }

        const auto width = Bits(0);
        const unsigned count = width / element;
        const unsigned perLane = 128 / element;
        if (kind == FloatKind::Fma) {
            Write(0, FloatBits(FusedMultiplyAdd(spec, AsFloats(ReadAs(0, width), element), AsFloats(ReadAs(1, width), element), AsFloats(ReadAs(2, width), element))));
            return;
        }
        if (unary || kind == FloatKind::Round) {
            auto* source = AsFloats(ReadAs(1, width), element);
            Write(0, FloatBits(kind == FloatKind::Round ? RoundValue(source, Immediate(2)) : Arithmetic(kind, source, source)));
            return;
        }
        const int base = Base(kind == FloatKind::Compare ? 3 : 2);
        auto* a = AsFloats(ReadAs(base, width), element);
        auto* b = AsFloats(ReadAs(base + 1, width), element);
        llvm::Value* result = nullptr;
        switch (kind) {
        case FloatKind::Compare:
            result = CompareMask(a, b, Immediate(base + 2));
            break;
        case FloatKind::AddSub: {
            auto* sum = Arithmetic(FloatKind::Add, a, b);
            auto* difference = Arithmetic(FloatKind::Sub, a, b);
            std::vector<int> mask(count);
            for (unsigned i = 0; i < count; ++i) mask[i] = static_cast<int>(i % 2 == 0 ? i : count + i);
            result = _builder.CreateShuffleVector(difference, sum, mask);
            break;
        }
        case FloatKind::HorizontalAdd:
        case FloatKind::HorizontalSub: {
            std::vector<int> even(count);
            std::vector<int> odd(count);
            const unsigned lanes = width / 128;
            for (unsigned lane = 0; lane < lanes; ++lane) {
                for (unsigned i = 0; i < perLane; ++i) {
                    const unsigned pair = i % (perLane / 2);
                    const unsigned source = (i < perLane / 2 ? 0 : count) + lane * perLane + 2 * pair;
                    even[lane * perLane + i] = static_cast<int>(source);
                    odd[lane * perLane + i] = static_cast<int>(source + 1);
                }
            }
            auto* left = _builder.CreateShuffleVector(a, b, even);
            auto* right = _builder.CreateShuffleVector(a, b, odd);
            result = Arithmetic(kind == FloatKind::HorizontalAdd ? FloatKind::Add : FloatKind::Sub, left, right);
            break;
        }
        default:
            result = Arithmetic(kind, a, b);
            break;
        }
        Write(0, FloatBits(result));
    }

    llvm::Value* ConvertNan(llvm::Value* source, llvm::Value* converted) {
        if (!ExactFloat()) return converted;
        const auto from = source->getType()->getScalarSizeInBits();
        const auto to = converted->getType()->getScalarSizeInBits();
        const auto mantissa = [](const unsigned bits) { return bits == 16 ? 10u : bits == 32 ? 23u : 52u; };
        auto* integerSource = _builder.CreateBitCast(source, IntegerLike(source->getType()));
        auto* targetInteger = IntegerLike(converted->getType());
        auto* sign = _builder.CreateICmpSLT(integerSource, llvm::Constant::getNullValue(integerSource->getType()));
        auto* payload = _builder.CreateAnd(integerSource, llvm::ConstantInt::get(integerSource->getType(), (std::uint64_t{1} << mantissa(from)) - 1));
        llvm::Value* moved = nullptr;
        if (mantissa(to) >= mantissa(from)) moved = _builder.CreateShl(_builder.CreateZExt(payload, targetInteger), mantissa(to) - mantissa(from));
        else moved = _builder.CreateTrunc(_builder.CreateLShr(payload, mantissa(from) - mantissa(to)), targetInteger);
        const auto exponentAndQuiet = ((std::uint64_t{1} << (to - 1 - mantissa(to))) - 1) << mantissa(to) | (std::uint64_t{1} << (mantissa(to) - 1));
        auto* signBit = _builder.CreateSelect(sign, llvm::ConstantInt::get(targetInteger, std::uint64_t{1} << (to - 1)), llvm::Constant::getNullValue(targetInteger));
        auto* nan = _builder.CreateOr(_builder.CreateOr(moved, llvm::ConstantInt::get(targetInteger, exponentAndQuiet)), signBit);
        return _builder.CreateSelect(IsNan(source), _builder.CreateBitCast(nan, converted->getType()), converted);
    }

    llvm::Value* SingleToHalf(llvm::Value* source, const unsigned mode) {
        auto* type = llvm::cast<llvm::FixedVectorType>(source->getType());
        auto* i32 = llvm::FixedVectorType::get(Int(32), type->getNumElements());
        auto* x = _builder.CreateBitCast(source, i32);
        const auto constant = [&](const std::uint32_t value) { return llvm::ConstantInt::get(i32, value); };
        auto* sign = _builder.CreateAnd(_builder.CreateLShr(x, 16), constant(0x8000));
        auto* negative = _builder.CreateICmpNE(sign, constant(0));
        auto* magnitude = _builder.CreateAnd(x, constant(0x7fffffff));
        auto* exponent = _builder.CreateLShr(magnitude, 23);
        auto* mantissa = _builder.CreateOr(_builder.CreateAnd(magnitude, constant(0x7fffff)), constant(0x800000));
        auto* subnormal = _builder.CreateICmpULT(magnitude, constant(0x38800000));
        auto* rawShift = _builder.CreateSub(constant(126), exponent);
        auto* shift = _builder.CreateSelect(subnormal, _builder.CreateBinaryIntrinsic(llvm::Intrinsic::umin, rawShift, constant(25)), constant(13));
        auto* base = _builder.CreateSelect(subnormal, _builder.CreateLShr(mantissa, shift), _builder.CreateLShr(_builder.CreateSub(magnitude, constant(0x38000000)), 13));
        auto* remainderMask = _builder.CreateSub(_builder.CreateShl(constant(1), shift), constant(1));
        auto* remainder = _builder.CreateAnd(_builder.CreateSelect(subnormal, mantissa, magnitude), remainderMask);
        auto* half = _builder.CreateShl(constant(1), _builder.CreateSub(shift, constant(1)));
        auto* inexact = _builder.CreateICmpNE(remainder, constant(0));
        llvm::Value* increment = nullptr;
        switch (mode) {
        case 0:
            increment = _builder.CreateOr(_builder.CreateICmpUGT(remainder, half), _builder.CreateAnd(_builder.CreateICmpEQ(remainder, half), _builder.CreateICmpNE(_builder.CreateAnd(base, constant(1)), constant(0))));
            break;
        case 1:
            increment = _builder.CreateAnd(inexact, negative);
            break;
        case 2:
            increment = _builder.CreateAnd(inexact, _builder.CreateNot(negative));
            break;
        default:
            increment = llvm::Constant::getNullValue(llvm::FixedVectorType::get(_builder.getInt1Ty(), type->getNumElements()));
            break;
        }
        auto* rounded = _builder.CreateAdd(base, _builder.CreateZExt(increment, i32));
        const bool infinityPositive = mode == 0 || mode == 2;
        const bool infinityNegative = mode == 0 || mode == 1;
        auto* overflowValue = _builder.CreateSelect(negative, constant(infinityNegative ? 0x7c00 : 0x7bff), constant(infinityPositive ? 0x7c00 : 0x7bff));
        auto* overflow = _builder.CreateOr(_builder.CreateICmpUGE(magnitude, constant(0x47800000)), _builder.CreateICmpUGT(rounded, constant(0x7bff)));
        llvm::Value* finite = _builder.CreateSelect(overflow, overflowValue, rounded);
        auto* nan = _builder.CreateOr(constant(0x7e00), _builder.CreateLShr(_builder.CreateAnd(magnitude, constant(0x7fffff)), 13));
        auto* isNan = _builder.CreateICmpUGT(magnitude, constant(0x7f800000));
        auto* isInfinity = _builder.CreateICmpEQ(magnitude, constant(0x7f800000));
        finite = _builder.CreateSelect(isInfinity, constant(0x7c00), finite);
        finite = _builder.CreateSelect(isNan, nan, finite);
        auto* result = _builder.CreateOr(finite, sign);
        return _builder.CreateTrunc(result, llvm::FixedVectorType::get(Int(16), type->getNumElements()));
    }

    llvm::Value* FloatToInteger(llvm::Value* value, const unsigned bits, const bool truncate) {
        auto* rounded = _builder.CreateUnaryIntrinsic(truncate ? llvm::Intrinsic::trunc : llvm::Intrinsic::rint, value);
        auto* lower = FloatConstant(value->getType(), -std::ldexp(1.0, static_cast<int>(bits) - 1));
        auto* upper = FloatConstant(value->getType(), std::ldexp(1.0, static_cast<int>(bits) - 1));
        auto* inRange = _builder.CreateAnd(_builder.CreateFCmpOGE(rounded, lower), _builder.CreateFCmpOLT(rounded, upper));
        auto* targetType = value->getType()->isVectorTy() ? static_cast<llvm::Type*>(llvm::FixedVectorType::get(Int(bits), llvm::cast<llvm::FixedVectorType>(value->getType())->getNumElements())) : Int(bits);
        auto* safe = _builder.CreateSelect(inRange, rounded, llvm::Constant::getNullValue(value->getType()));
        auto* converted = _builder.CreateFPToSI(safe, targetType);
        return _builder.CreateSelect(inRange, converted, llvm::ConstantInt::get(targetType, std::uint64_t{1} << (bits - 1)));
    }

    llvm::Value* LowElements(llvm::Value* vector, const unsigned count) {
        const auto total = llvm::cast<llvm::FixedVectorType>(vector->getType())->getNumElements();
        if (total == count) return vector;
        std::vector<int> mask(count);
        for (unsigned i = 0; i < count; ++i) mask[i] = static_cast<int>(i);
        return _builder.CreateShuffleVector(vector, vector, mask);
    }

    void WriteVectorLow(const int index, llvm::Value* vector, const unsigned width) {
        auto* bits = FloatBits(vector);
        Write(index, _builder.CreateZExtOrTrunc(bits, Int(width)));
    }

    bool EmitFloatConversion(const ZydisMnemonic mnemonic) {
        const bool scalarSource = mnemonic == ZYDIS_MNEMONIC_CVTSI2SS || mnemonic == ZYDIS_MNEMONIC_VCVTSI2SS || mnemonic == ZYDIS_MNEMONIC_CVTSI2SD || mnemonic == ZYDIS_MNEMONIC_VCVTSI2SD
            || mnemonic == ZYDIS_MNEMONIC_CVTSS2SD || mnemonic == ZYDIS_MNEMONIC_VCVTSS2SD || mnemonic == ZYDIS_MNEMONIC_CVTSD2SS || mnemonic == ZYDIS_MNEMONIC_VCVTSD2SS;
        if (scalarSource) {
            const int first = Base(2);
            const auto target = mnemonic == ZYDIS_MNEMONIC_CVTSI2SS || mnemonic == ZYDIS_MNEMONIC_VCVTSI2SS || mnemonic == ZYDIS_MNEMONIC_CVTSD2SS || mnemonic == ZYDIS_MNEMONIC_VCVTSD2SS ? 32u : 64u;
            auto* destination = AsFloats(ReadAs(first, 128), target);
            llvm::Value* value = nullptr;
            if (mnemonic == ZYDIS_MNEMONIC_CVTSI2SS || mnemonic == ZYDIS_MNEMONIC_VCVTSI2SS || mnemonic == ZYDIS_MNEMONIC_CVTSI2SD || mnemonic == ZYDIS_MNEMONIC_VCVTSI2SD) {
                value = _builder.CreateSIToFP(Read(first + 1), FloatType(target));
            } else {
                auto* source = LoadScalarFloat(first + 1, target == 32 ? 64 : 32);
                value = ConvertNan(source, target == 32 ? _builder.CreateFPTrunc(source, FloatType(32)) : _builder.CreateFPExt(source, FloatType(64)));
            }
            Write(0, FloatBits(_builder.CreateInsertElement(destination, value, _builder.getInt64(0))));
            return true;
        }
        switch (mnemonic) {
        case ZYDIS_MNEMONIC_CVTTSS2SI: case ZYDIS_MNEMONIC_VCVTTSS2SI: case ZYDIS_MNEMONIC_CVTSS2SI: case ZYDIS_MNEMONIC_VCVTSS2SI:
        case ZYDIS_MNEMONIC_CVTTSD2SI: case ZYDIS_MNEMONIC_VCVTTSD2SI: case ZYDIS_MNEMONIC_CVTSD2SI: case ZYDIS_MNEMONIC_VCVTSD2SI: {
            const bool isDouble = mnemonic == ZYDIS_MNEMONIC_CVTTSD2SI || mnemonic == ZYDIS_MNEMONIC_VCVTTSD2SI || mnemonic == ZYDIS_MNEMONIC_CVTSD2SI || mnemonic == ZYDIS_MNEMONIC_VCVTSD2SI;
            const bool truncate = mnemonic == ZYDIS_MNEMONIC_CVTTSS2SI || mnemonic == ZYDIS_MNEMONIC_VCVTTSS2SI || mnemonic == ZYDIS_MNEMONIC_CVTTSD2SI || mnemonic == ZYDIS_MNEMONIC_VCVTTSD2SI;
            Write(0, FloatToInteger(LoadScalarFloat(1, isDouble ? 64 : 32), Bits(0), truncate));
            return true;
        }
        case ZYDIS_MNEMONIC_CVTDQ2PS: case ZYDIS_MNEMONIC_VCVTDQ2PS: {
            const auto width = Bits(0);
            Write(0, FloatBits(_builder.CreateSIToFP(AsVector(ReadAs(1, width), 32), llvm::FixedVectorType::get(FloatType(32), width / 32))));
            return true;
        }
        case ZYDIS_MNEMONIC_CVTTPS2DQ: case ZYDIS_MNEMONIC_VCVTTPS2DQ: case ZYDIS_MNEMONIC_CVTPS2DQ: case ZYDIS_MNEMONIC_VCVTPS2DQ: {
            const auto width = Bits(0);
            const bool truncate = mnemonic == ZYDIS_MNEMONIC_CVTTPS2DQ || mnemonic == ZYDIS_MNEMONIC_VCVTTPS2DQ;
            Write(0, AsInteger(FloatToInteger(AsFloats(ReadAs(1, width), 32), 32, truncate)));
            return true;
        }
        case ZYDIS_MNEMONIC_CVTDQ2PD: case ZYDIS_MNEMONIC_VCVTDQ2PD: {
            const auto count = Bits(0) / 64;
            auto* source = AsVector(_builder.CreateZExtOrTrunc(ReadRegisterOrMemory(1, count * 32), Int(count * 32)), 32);
            Write(0, FloatBits(_builder.CreateSIToFP(source, llvm::FixedVectorType::get(FloatType(64), count))));
            return true;
        }
        case ZYDIS_MNEMONIC_CVTPS2PD: case ZYDIS_MNEMONIC_VCVTPS2PD: {
            const auto count = Bits(0) / 64;
            auto* source = AsFloats(_builder.CreateZExtOrTrunc(ReadRegisterOrMemory(1, count * 32), Int(count * 32)), 32);
            Write(0, FloatBits(ConvertNan(source, _builder.CreateFPExt(source, llvm::FixedVectorType::get(FloatType(64), count)))));
            return true;
        }
        case ZYDIS_MNEMONIC_CVTPD2PS: case ZYDIS_MNEMONIC_VCVTPD2PS:
        case ZYDIS_MNEMONIC_CVTPD2DQ: case ZYDIS_MNEMONIC_VCVTPD2DQ: case ZYDIS_MNEMONIC_CVTTPD2DQ: case ZYDIS_MNEMONIC_VCVTTPD2DQ: {
            const auto sourceBits = Bits(1);
            const auto count = sourceBits / 64;
            auto* source = AsFloats(ReadAs(1, sourceBits), 64);
            llvm::Value* converted = nullptr;
            if (mnemonic == ZYDIS_MNEMONIC_CVTPD2PS || mnemonic == ZYDIS_MNEMONIC_VCVTPD2PS) converted = ConvertNan(source, _builder.CreateFPTrunc(source, llvm::FixedVectorType::get(FloatType(32), count)));
            else converted = FloatToInteger(source, 32, mnemonic == ZYDIS_MNEMONIC_CVTTPD2DQ || mnemonic == ZYDIS_MNEMONIC_VCVTTPD2DQ);
            WriteVectorLow(0, converted, 128);
            return true;
        }
        case ZYDIS_MNEMONIC_VCVTPH2PS: {
            const auto count = Bits(0) / 32;
            auto* source = _builder.CreateBitCast(_builder.CreateZExtOrTrunc(ReadRegisterOrMemory(1, count * 16), Int(count * 16)), llvm::FixedVectorType::get(FloatType(16), count));
            Write(0, FloatBits(ConvertNan(source, _builder.CreateFPExt(source, llvm::FixedVectorType::get(FloatType(32), count)))));
            return true;
        }
        case ZYDIS_MNEMONIC_VCVTPS2PH: {
            const auto control = Immediate(2);
            const auto sourceBits = ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, Operand(1).reg.value);
            const auto count = sourceBits / 32;
            auto* source = AsFloats(ReadRegister(Operand(1).reg.value), 32);
            llvm::Value* bits = nullptr;
            if (control & 4) bits = FloatBits(ConvertNan(source, _builder.CreateFPTrunc(source, llvm::FixedVectorType::get(FloatType(16), count))));
            else bits = AsInteger(SingleToHalf(source, static_cast<unsigned>(control & 3)));
            if (Operand(0).type == ZYDIS_OPERAND_TYPE_MEMORY) Write(0, bits);
            else WriteRegister(Operand(0).reg.value, _builder.CreateZExt(bits, Int(128)));
            return true;
        }
        default:
            return false;
        }
    }

    llvm::Value* ReadRegisterOrMemory(const int index, const unsigned bits) {
        const auto& operand = Operand(index);
        if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY) return Load(Address(operand), bits, false);
        return _builder.CreateZExtOrTrunc(ReadRegister(operand.reg.value), Int(bits));
    }

    void CompareScalar(const unsigned element) {
        auto* a = LoadScalarFloat(0, element);
        auto* b = LoadScalarFloat(1, element);
        auto* unordered = _builder.CreateFCmpUNO(a, b);
        SetFlag(PS5LIFT_ZF, _builder.CreateFCmpUEQ(a, b));
        SetFlag(PS5LIFT_PF, unordered);
        SetFlag(PS5LIFT_CF, _builder.CreateFCmpULT(a, b));
        for (const auto flag : {PS5LIFT_OF, PS5LIFT_SF, PS5LIFT_AF}) SetFlag(flag, _builder.getFalse());
    }

    void HalfMove(const ZydisMnemonic mnemonic) {
        const bool vex = _instruction->Info.encoding == ZYDIS_INSTRUCTION_ENCODING_VEX;
        if (Operand(0).type == ZYDIS_OPERAND_TYPE_MEMORY) {
            auto* source = AsVector(ReadRegister(Operand(1).reg.value), 64);
            const bool high = mnemonic == ZYDIS_MNEMONIC_MOVHPS || mnemonic == ZYDIS_MNEMONIC_VMOVHPS || mnemonic == ZYDIS_MNEMONIC_MOVHPD || mnemonic == ZYDIS_MNEMONIC_VMOVHPD;
            Write(0, _builder.CreateExtractElement(source, _builder.getInt64(high ? 1 : 0)));
            return;
        }
        const int base = vex ? 1 : 0;
        auto* keep = AsVector(_builder.CreateTrunc(ReadRegister(Operand(base).reg.value), Int(128)), 64);
        auto* other = Operand(base + 1).type == ZYDIS_OPERAND_TYPE_MEMORY ? Load(Address(Operand(base + 1)), 64, false) : nullptr;
        auto* otherVector = other == nullptr ? AsVector(_builder.CreateTrunc(ReadRegister(Operand(base + 1).reg.value), Int(128)), 64) : nullptr;
        llvm::Value* result = nullptr;
        switch (mnemonic) {
        case ZYDIS_MNEMONIC_MOVHLPS: case ZYDIS_MNEMONIC_VMOVHLPS:
            result = _builder.CreateInsertElement(keep, _builder.CreateExtractElement(otherVector, _builder.getInt64(1)), _builder.getInt64(0));
            break;
        case ZYDIS_MNEMONIC_MOVLHPS: case ZYDIS_MNEMONIC_VMOVLHPS:
            result = _builder.CreateInsertElement(keep, _builder.CreateExtractElement(otherVector, _builder.getInt64(0)), _builder.getInt64(1));
            break;
        case ZYDIS_MNEMONIC_MOVHPS: case ZYDIS_MNEMONIC_VMOVHPS: case ZYDIS_MNEMONIC_MOVHPD: case ZYDIS_MNEMONIC_VMOVHPD:
            result = _builder.CreateInsertElement(keep, other, _builder.getInt64(1));
            break;
        default:
            result = _builder.CreateInsertElement(keep, other, _builder.getInt64(0));
            break;
        }
        Write(0, AsInteger(result));
    }

    bool EmitFloat(const ZydisMnemonic mnemonic) {
        if (auto spec = FloatSpecOf(mnemonic)) {
            if (mnemonic == ZYDIS_MNEMONIC_CMPSD && Operand(0).type != ZYDIS_OPERAND_TYPE_REGISTER) return false;
            EmitFloatArithmetic(*spec);
            return true;
        }
        if (EmitFloatConversion(mnemonic)) return true;
        switch (mnemonic) {
        case ZYDIS_MNEMONIC_COMISS: case ZYDIS_MNEMONIC_VCOMISS: case ZYDIS_MNEMONIC_UCOMISS: case ZYDIS_MNEMONIC_VUCOMISS:
            CompareScalar(32);
            return true;
        case ZYDIS_MNEMONIC_COMISD: case ZYDIS_MNEMONIC_VCOMISD: case ZYDIS_MNEMONIC_UCOMISD: case ZYDIS_MNEMONIC_VUCOMISD:
            CompareScalar(64);
            return true;
        case ZYDIS_MNEMONIC_LDMXCSR: case ZYDIS_MNEMONIC_VLDMXCSR: {
            StateStore(Read(0), offsetof(Ps5LiftState, Mxcsr));
            _builder.CreateCall(_module.Runtime("ps5lift_apply_mxcsr", llvm::FunctionType::get(_builder.getVoidTy(), {_module.Pointer()}, false)), {_state});
            return true;
        }
        case ZYDIS_MNEMONIC_STMXCSR: case ZYDIS_MNEMONIC_VSTMXCSR: {
            auto callee = _module.Runtime("ps5lift_read_mxcsr", llvm::FunctionType::get(_builder.getInt32Ty(), {_module.Pointer()}, false));
            Write(0, _builder.CreateCall(callee, {_state}));
            return true;
        }
        case ZYDIS_MNEMONIC_MOVHLPS: case ZYDIS_MNEMONIC_VMOVHLPS: case ZYDIS_MNEMONIC_MOVLHPS: case ZYDIS_MNEMONIC_VMOVLHPS:
        case ZYDIS_MNEMONIC_MOVHPS: case ZYDIS_MNEMONIC_VMOVHPS: case ZYDIS_MNEMONIC_MOVLPS: case ZYDIS_MNEMONIC_VMOVLPS:
        case ZYDIS_MNEMONIC_MOVHPD: case ZYDIS_MNEMONIC_VMOVHPD: case ZYDIS_MNEMONIC_MOVLPD: case ZYDIS_MNEMONIC_VMOVLPD:
            HalfMove(mnemonic);
            return true;
        case ZYDIS_MNEMONIC_MOVSHDUP: case ZYDIS_MNEMONIC_VMOVSHDUP: case ZYDIS_MNEMONIC_MOVSLDUP: case ZYDIS_MNEMONIC_VMOVSLDUP: {
            const auto width = Bits(0);
            auto* source = AsVector(ReadAs(1, width), 32);
            const bool high = mnemonic == ZYDIS_MNEMONIC_MOVSHDUP || mnemonic == ZYDIS_MNEMONIC_VMOVSHDUP;
            std::vector<int> mask(width / 32);
            for (unsigned i = 0; i < mask.size(); ++i) mask[i] = static_cast<int>((i & ~1u) + (high ? 1 : 0));
            Write(0, AsInteger(_builder.CreateShuffleVector(source, source, mask)));
            return true;
        }
        case ZYDIS_MNEMONIC_MOVDDUP: case ZYDIS_MNEMONIC_VMOVDDUP: {
            const auto width = Bits(0);
            auto* source = AsVector(_builder.CreateZExtOrTrunc(ReadRegisterOrMemory(1, width == 128 ? 64 : 256), Int(width)), 64);
            std::vector<int> mask(width / 64);
            for (unsigned i = 0; i < mask.size(); ++i) mask[i] = static_cast<int>(i & ~1u);
            Write(0, AsInteger(_builder.CreateShuffleVector(source, source, mask)));
            return true;
        }
        case ZYDIS_MNEMONIC_SHUFPD: case ZYDIS_MNEMONIC_VSHUFPD: {
            const int base = Base(3);
            const auto width = Bits(0);
            auto* a = AsVector(ReadAs(base, width), 64);
            auto* b = AsVector(ReadAs(base + 1, width), 64);
            const auto control = Immediate(base + 2);
            const unsigned count = width / 64;
            std::vector<int> mask(count);
            for (unsigned i = 0; i < count; ++i) {
                const unsigned lane = i / 2 * 2;
                const unsigned pick = lane + ((control >> i) & 1);
                mask[i] = static_cast<int>(i % 2 == 0 ? pick : count + pick);
            }
            Write(0, AsInteger(_builder.CreateShuffleVector(a, b, mask)));
            return true;
        }
        case ZYDIS_MNEMONIC_VPERMILPD: {
            if (Operand(2).type != ZYDIS_OPERAND_TYPE_IMMEDIATE) throw UnsupportedInstruction("variable control");
            const auto width = Bits(0);
            auto* source = AsVector(ReadAs(1, width), 64);
            const auto control = Immediate(2);
            std::vector<int> mask(width / 64);
            for (unsigned i = 0; i < mask.size(); ++i) mask[i] = static_cast<int>(i / 2 * 2 + ((control >> i) & 1));
            Write(0, AsInteger(_builder.CreateShuffleVector(source, source, mask)));
            return true;
        }
        case ZYDIS_MNEMONIC_INSERTPS: case ZYDIS_MNEMONIC_VINSERTPS: {
            const int base = Base(3);
            auto* destination = AsVector(ReadAs(base, 128), 32);
            const auto control = Immediate(base + 2);
            llvm::Value* element = nullptr;
            if (Operand(base + 1).type == ZYDIS_OPERAND_TYPE_MEMORY) element = Load(Address(Operand(base + 1)), 32, false);
            else element = _builder.CreateExtractElement(AsVector(_builder.CreateTrunc(ReadRegister(Operand(base + 1).reg.value), Int(128)), 32), _builder.getInt64((control >> 6) & 3));
            llvm::Value* result = _builder.CreateInsertElement(destination, element, _builder.getInt64((control >> 4) & 3));
            for (unsigned i = 0; i < 4; ++i) {
                if ((control >> i) & 1) result = _builder.CreateInsertElement(result, _builder.getInt32(0), _builder.getInt64(i));
            }
            Write(0, AsInteger(result));
            return true;
        }
        case ZYDIS_MNEMONIC_VBROADCASTSS: case ZYDIS_MNEMONIC_VBROADCASTSD: case ZYDIS_MNEMONIC_VBROADCASTF128: case ZYDIS_MNEMONIC_VBROADCASTI128:
            Broadcast();
            return true;
        default:
            return false;
        }
    }

    llvm::Value* CallX87(const std::uint32_t operation, const std::uint32_t format, llvm::Value* address, const std::uint32_t index) {
        auto callee = _module.Runtime("ps5lift_x87", llvm::FunctionType::get(_builder.getInt32Ty(), {_module.Pointer(), _builder.getInt32Ty(), _builder.getInt32Ty(), I64(), _builder.getInt32Ty()}, false));
        return _builder.CreateCall(callee, {_state, _builder.getInt32(operation), _builder.getInt32(format), address ? address : _builder.getInt64(0), _builder.getInt32(index)});
    }

    static bool IsStackRegister(const ZydisDecodedOperand& operand) {
        return operand.type == ZYDIS_OPERAND_TYPE_REGISTER && operand.reg.value >= ZYDIS_REGISTER_ST0 && operand.reg.value <= ZYDIS_REGISTER_ST7;
    }

    static std::uint32_t StackIndex(const ZydisDecodedOperand& operand) {
        return static_cast<std::uint32_t>(operand.reg.value - ZYDIS_REGISTER_ST0);
    }

    const ZydisDecodedOperand* X87Memory() const {
        for (unsigned i = 0; i < _instruction->Info.operand_count_visible; ++i) {
            if (Operand(static_cast<int>(i)).type == ZYDIS_OPERAND_TYPE_MEMORY) return &Operand(static_cast<int>(i));
        }
        return nullptr;
    }

    std::uint32_t X87Format(const bool integer) const {
        const auto* memory = X87Memory();
        if (memory == nullptr) return PS5LIFT_X87_REGISTER;
        switch (memory->size) {
        case 16: return integer ? PS5LIFT_X87_INTEGER16 : throw UnsupportedInstruction("16-bit x87 float");
        case 32: return integer ? PS5LIFT_X87_INTEGER32 : PS5LIFT_X87_FLOAT32;
        case 64: return integer ? PS5LIFT_X87_INTEGER64 : PS5LIFT_X87_FLOAT64;
        case 80: return PS5LIFT_X87_FLOAT80;
        default: throw UnsupportedInstruction("x87 operand size");
        }
    }

    std::uint32_t X87Index(const std::uint32_t fallback) const {
        std::uint32_t index = fallback;
        bool found = false;
        for (unsigned i = 0; i < _instruction->Info.operand_count_visible; ++i) {
            const auto& operand = Operand(static_cast<int>(i));
            if (!IsStackRegister(operand)) continue;
            if (StackIndex(operand) != 0 || !found) index = StackIndex(operand);
            found = found || StackIndex(operand) != 0;
        }
        return index;
    }

    bool X87ToRegister() const {
        return _instruction->Info.operand_count_visible >= 2 && IsStackRegister(Operand(0)) && StackIndex(Operand(0)) != 0;
    }

    void X87CompareFlags(llvm::Value* result) {
        auto* unordered = _builder.CreateICmpEQ(result, _builder.getInt32(3));
        SetFlag(PS5LIFT_ZF, _builder.CreateOr(unordered, _builder.CreateICmpEQ(result, _builder.getInt32(2))));
        SetFlag(PS5LIFT_PF, unordered);
        SetFlag(PS5LIFT_CF, _builder.CreateOr(unordered, _builder.CreateICmpEQ(result, _builder.getInt32(1))));
        for (const auto flag : {PS5LIFT_OF, PS5LIFT_SF, PS5LIFT_AF}) SetFlag(flag, _builder.getFalse());
    }

    llvm::Value* X87Address() {
        const auto* memory = X87Memory();
        return memory ? Address(*memory) : nullptr;
    }

    bool EmitX87(const ZydisMnemonic mnemonic) {
        using M = std::uint32_t;
        const auto arithmetic = [&](const M operation, const bool integer, const bool pop) {
            M format = X87Format(integer);
            if (format == PS5LIFT_X87_REGISTER && (X87ToRegister() || pop)) format |= PS5LIFT_X87_TO_REGISTER;
            if (pop) format |= PS5LIFT_X87_POP;
            CallX87(operation, format, X87Address(), X87Index(1));
        };
        switch (mnemonic) {
        case ZYDIS_MNEMONIC_FLD: CallX87(PS5LIFT_X87_LOAD, X87Format(false), X87Address(), X87Index(0)); return true;
        case ZYDIS_MNEMONIC_FILD: CallX87(PS5LIFT_X87_LOAD, X87Format(true), X87Address(), 0); return true;
        case ZYDIS_MNEMONIC_FST: CallX87(PS5LIFT_X87_STORE, X87Format(false), X87Address(), X87Index(0)); return true;
        case ZYDIS_MNEMONIC_FSTP: CallX87(PS5LIFT_X87_STORE, X87Format(false) | PS5LIFT_X87_POP, X87Address(), X87Index(0)); return true;
        case ZYDIS_MNEMONIC_FIST: CallX87(PS5LIFT_X87_STORE, X87Format(true), X87Address(), 0); return true;
        case ZYDIS_MNEMONIC_FISTP: CallX87(PS5LIFT_X87_STORE, X87Format(true) | PS5LIFT_X87_POP, X87Address(), 0); return true;
        case ZYDIS_MNEMONIC_FISTTP: CallX87(PS5LIFT_X87_STORE, X87Format(true) | PS5LIFT_X87_POP | PS5LIFT_X87_TRUNCATE, X87Address(), 0); return true;
        case ZYDIS_MNEMONIC_FADD: arithmetic(PS5LIFT_X87_ADD, false, false); return true;
        case ZYDIS_MNEMONIC_FADDP: arithmetic(PS5LIFT_X87_ADD, false, true); return true;
        case ZYDIS_MNEMONIC_FIADD: arithmetic(PS5LIFT_X87_ADD, true, false); return true;
        case ZYDIS_MNEMONIC_FSUB: arithmetic(PS5LIFT_X87_SUBTRACT, false, false); return true;
        case ZYDIS_MNEMONIC_FSUBP: arithmetic(PS5LIFT_X87_SUBTRACT, false, true); return true;
        case ZYDIS_MNEMONIC_FISUB: arithmetic(PS5LIFT_X87_SUBTRACT, true, false); return true;
        case ZYDIS_MNEMONIC_FSUBR: arithmetic(PS5LIFT_X87_SUBTRACT_REVERSE, false, false); return true;
        case ZYDIS_MNEMONIC_FSUBRP: arithmetic(PS5LIFT_X87_SUBTRACT_REVERSE, false, true); return true;
        case ZYDIS_MNEMONIC_FISUBR: arithmetic(PS5LIFT_X87_SUBTRACT_REVERSE, true, false); return true;
        case ZYDIS_MNEMONIC_FMUL: arithmetic(PS5LIFT_X87_MULTIPLY, false, false); return true;
        case ZYDIS_MNEMONIC_FMULP: arithmetic(PS5LIFT_X87_MULTIPLY, false, true); return true;
        case ZYDIS_MNEMONIC_FIMUL: arithmetic(PS5LIFT_X87_MULTIPLY, true, false); return true;
        case ZYDIS_MNEMONIC_FDIV: arithmetic(PS5LIFT_X87_DIVIDE, false, false); return true;
        case ZYDIS_MNEMONIC_FDIVP: arithmetic(PS5LIFT_X87_DIVIDE, false, true); return true;
        case ZYDIS_MNEMONIC_FIDIV: arithmetic(PS5LIFT_X87_DIVIDE, true, false); return true;
        case ZYDIS_MNEMONIC_FDIVR: arithmetic(PS5LIFT_X87_DIVIDE_REVERSE, false, false); return true;
        case ZYDIS_MNEMONIC_FDIVRP: arithmetic(PS5LIFT_X87_DIVIDE_REVERSE, false, true); return true;
        case ZYDIS_MNEMONIC_FIDIVR: arithmetic(PS5LIFT_X87_DIVIDE_REVERSE, true, false); return true;
        case ZYDIS_MNEMONIC_FCOM: CallX87(PS5LIFT_X87_COMPARE, X87Format(false), X87Address(), X87Index(1)); return true;
        case ZYDIS_MNEMONIC_FCOMP: CallX87(PS5LIFT_X87_COMPARE, X87Format(false) | PS5LIFT_X87_POP, X87Address(), X87Index(1)); return true;
        case ZYDIS_MNEMONIC_FCOMPP: CallX87(PS5LIFT_X87_COMPARE, PS5LIFT_X87_POP_TWICE, nullptr, 1); return true;
        case ZYDIS_MNEMONIC_FUCOM: CallX87(PS5LIFT_X87_COMPARE_UNORDERED, PS5LIFT_X87_REGISTER, nullptr, X87Index(1)); return true;
        case ZYDIS_MNEMONIC_FUCOMP: CallX87(PS5LIFT_X87_COMPARE_UNORDERED, PS5LIFT_X87_POP, nullptr, X87Index(1)); return true;
        case ZYDIS_MNEMONIC_FUCOMPP: CallX87(PS5LIFT_X87_COMPARE_UNORDERED, PS5LIFT_X87_POP_TWICE, nullptr, 1); return true;
        case ZYDIS_MNEMONIC_FICOM: CallX87(PS5LIFT_X87_COMPARE, X87Format(true), X87Address(), 0); return true;
        case ZYDIS_MNEMONIC_FICOMP: CallX87(PS5LIFT_X87_COMPARE, X87Format(true) | PS5LIFT_X87_POP, X87Address(), 0); return true;
        case ZYDIS_MNEMONIC_FCOMI: X87CompareFlags(CallX87(PS5LIFT_X87_COMPARE_FLAGS, 0, nullptr, X87Index(1))); return true;
        case ZYDIS_MNEMONIC_FCOMIP: X87CompareFlags(CallX87(PS5LIFT_X87_COMPARE_FLAGS, PS5LIFT_X87_POP, nullptr, X87Index(1))); return true;
        case ZYDIS_MNEMONIC_FUCOMI: X87CompareFlags(CallX87(PS5LIFT_X87_COMPARE_FLAGS_UNORDERED, 0, nullptr, X87Index(1))); return true;
        case ZYDIS_MNEMONIC_FUCOMIP: X87CompareFlags(CallX87(PS5LIFT_X87_COMPARE_FLAGS_UNORDERED, PS5LIFT_X87_POP, nullptr, X87Index(1))); return true;
        case ZYDIS_MNEMONIC_FTST: CallX87(PS5LIFT_X87_TEST, 0, nullptr, 0); return true;
        case ZYDIS_MNEMONIC_FXAM: CallX87(PS5LIFT_X87_EXAMINE, 0, nullptr, 0); return true;
        case ZYDIS_MNEMONIC_FCHS: CallX87(PS5LIFT_X87_CHANGE_SIGN, 0, nullptr, 0); return true;
        case ZYDIS_MNEMONIC_FABS: CallX87(PS5LIFT_X87_ABSOLUTE, 0, nullptr, 0); return true;
        case ZYDIS_MNEMONIC_FSQRT: CallX87(PS5LIFT_X87_SQUARE_ROOT, 0, nullptr, 0); return true;
        case ZYDIS_MNEMONIC_FRNDINT: CallX87(PS5LIFT_X87_ROUND, 0, nullptr, 0); return true;
        case ZYDIS_MNEMONIC_FSCALE: CallX87(PS5LIFT_X87_SCALE, 0, nullptr, 0); return true;
        case ZYDIS_MNEMONIC_FPREM: CallX87(PS5LIFT_X87_PARTIAL_REMAINDER, 0, nullptr, 0); return true;
        case ZYDIS_MNEMONIC_FPREM1: CallX87(PS5LIFT_X87_IEEE_REMAINDER, 0, nullptr, 0); return true;
        case ZYDIS_MNEMONIC_FXCH: CallX87(PS5LIFT_X87_EXCHANGE, 0, nullptr, X87Index(1)); return true;
        case ZYDIS_MNEMONIC_FFREE: CallX87(PS5LIFT_X87_FREE, 0, nullptr, X87Index(0)); return true;
        case ZYDIS_MNEMONIC_FINCSTP: CallX87(PS5LIFT_X87_INCREMENT_TOP, 0, nullptr, 0); return true;
        case ZYDIS_MNEMONIC_FDECSTP: CallX87(PS5LIFT_X87_DECREMENT_TOP, 0, nullptr, 0); return true;
        case ZYDIS_MNEMONIC_FNINIT: CallX87(PS5LIFT_X87_INITIALIZE, 0, nullptr, 0); return true;
        case ZYDIS_MNEMONIC_FNCLEX: CallX87(PS5LIFT_X87_CLEAR_EXCEPTIONS, 0, nullptr, 0); return true;
        case ZYDIS_MNEMONIC_FWAIT: CallX87(PS5LIFT_X87_WAIT, 0, nullptr, 0); return true;
        case ZYDIS_MNEMONIC_FNSTSW: Write(0, _builder.CreateTrunc(CallX87(PS5LIFT_X87_STORE_STATUS, 0, nullptr, 0), _builder.getInt16Ty())); return true;
        case ZYDIS_MNEMONIC_FNSTCW: CallX87(PS5LIFT_X87_STORE_CONTROL, 0, X87Address(), 0); return true;
        case ZYDIS_MNEMONIC_FLDCW: CallX87(PS5LIFT_X87_LOAD_CONTROL, 0, X87Address(), 0); return true;
        case ZYDIS_MNEMONIC_FNSTENV: CallX87(PS5LIFT_X87_STORE_ENVIRONMENT, 0, X87Address(), 0); return true;
        case ZYDIS_MNEMONIC_FLDENV: CallX87(PS5LIFT_X87_LOAD_ENVIRONMENT, 0, X87Address(), 0); return true;
        case ZYDIS_MNEMONIC_FLD1: CallX87(PS5LIFT_X87_LOAD_CONSTANT, 0, nullptr, 0); return true;
        case ZYDIS_MNEMONIC_FLDL2T: CallX87(PS5LIFT_X87_LOAD_CONSTANT, 0, nullptr, 1); return true;
        case ZYDIS_MNEMONIC_FLDL2E: CallX87(PS5LIFT_X87_LOAD_CONSTANT, 0, nullptr, 2); return true;
        case ZYDIS_MNEMONIC_FLDPI: CallX87(PS5LIFT_X87_LOAD_CONSTANT, 0, nullptr, 3); return true;
        case ZYDIS_MNEMONIC_FLDLG2: CallX87(PS5LIFT_X87_LOAD_CONSTANT, 0, nullptr, 4); return true;
        case ZYDIS_MNEMONIC_FLDLN2: CallX87(PS5LIFT_X87_LOAD_CONSTANT, 0, nullptr, 5); return true;
        case ZYDIS_MNEMONIC_FLDZ: CallX87(PS5LIFT_X87_LOAD_CONSTANT, 0, nullptr, 6); return true;
        case ZYDIS_MNEMONIC_FCMOVB: case ZYDIS_MNEMONIC_FCMOVE: case ZYDIS_MNEMONIC_FCMOVBE: case ZYDIS_MNEMONIC_FCMOVU:
        case ZYDIS_MNEMONIC_FCMOVNB: case ZYDIS_MNEMONIC_FCMOVNE: case ZYDIS_MNEMONIC_FCMOVNBE: case ZYDIS_MNEMONIC_FCMOVNU: {
            llvm::Value* condition = nullptr;
            switch (mnemonic) {
            case ZYDIS_MNEMONIC_FCMOVB: condition = Flag(PS5LIFT_CF); break;
            case ZYDIS_MNEMONIC_FCMOVE: condition = Flag(PS5LIFT_ZF); break;
            case ZYDIS_MNEMONIC_FCMOVBE: condition = _builder.CreateOr(Flag(PS5LIFT_CF), Flag(PS5LIFT_ZF)); break;
            case ZYDIS_MNEMONIC_FCMOVU: condition = Flag(PS5LIFT_PF); break;
            case ZYDIS_MNEMONIC_FCMOVNB: condition = _builder.CreateNot(Flag(PS5LIFT_CF)); break;
            case ZYDIS_MNEMONIC_FCMOVNE: condition = _builder.CreateNot(Flag(PS5LIFT_ZF)); break;
            case ZYDIS_MNEMONIC_FCMOVNBE: condition = _builder.CreateNot(_builder.CreateOr(Flag(PS5LIFT_CF), Flag(PS5LIFT_ZF))); break;
            default: condition = _builder.CreateNot(Flag(PS5LIFT_PF)); break;
            }
            auto* move = llvm::BasicBlock::Create(Context(), "fcmov", _host);
            auto* after = llvm::BasicBlock::Create(Context(), "fcmov_done", _host);
            _builder.CreateCondBr(condition, move, after);
            _builder.SetInsertPoint(move);
            CallX87(PS5LIFT_X87_MOVE_REGISTER, 0, nullptr, X87Index(1));
            _builder.CreateBr(after);
            _builder.SetInsertPoint(after);
            return true;
        }
        default:
            return false;
        }
    }

    void EmitInstruction(const Instruction& instruction) {
        try {
            EmitSemantics(instruction.Info.mnemonic);
        } catch (const UnsupportedInstruction& error) {
            Unsupported(error.what());
            StartDeadBlock();
        }
    }

    void EmitSemantics(const ZydisMnemonic mnemonic) {
        if (IsConditionalJump(mnemonic)) {
            const auto target = *DirectTarget(*_instruction);
            llvm::Value* condition = nullptr;
            if (mnemonic == ZYDIS_MNEMONIC_JRCXZ) condition = _builder.CreateICmpEQ(Gpr(PS5LIFT_RCX), _builder.getInt64(0));
            else if (mnemonic == ZYDIS_MNEMONIC_JECXZ) condition = _builder.CreateICmpEQ(_builder.CreateTrunc(Gpr(PS5LIFT_RCX), _builder.getInt32Ty()), _builder.getInt32(0));
            else condition = Test(ConditionOf(mnemonic));
            _builder.CreateCondBr(condition, Target(target), Target(_instruction->Next()));
            return;
        }
        if (IsCmov(mnemonic)) {
            Write(0, _builder.CreateSelect(Test(ConditionOf(mnemonic)), Read(1), Read(0)));
            return;
        }
        if (IsSetcc(mnemonic)) {
            Write(0, _builder.CreateZExt(Test(ConditionOf(mnemonic)), _builder.getInt8Ty()));
            return;
        }
        switch (mnemonic) {
        case ZYDIS_MNEMONIC_NOP:
        case ZYDIS_MNEMONIC_ENDBR64:
        case ZYDIS_MNEMONIC_PAUSE:
        case ZYDIS_MNEMONIC_PREFETCHT0:
        case ZYDIS_MNEMONIC_PREFETCHT1:
        case ZYDIS_MNEMONIC_PREFETCHT2:
        case ZYDIS_MNEMONIC_PREFETCHNTA:
            return;
        case ZYDIS_MNEMONIC_MFENCE:
            _builder.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
            return;
        case ZYDIS_MNEMONIC_LFENCE:
        case ZYDIS_MNEMONIC_SFENCE:
            _builder.CreateFence(llvm::AtomicOrdering::AcquireRelease);
            return;
        case ZYDIS_MNEMONIC_MOV:
            Write(0, Read(1));
            return;
        case ZYDIS_MNEMONIC_MOVZX:
            Write(0, _builder.CreateZExt(Read(1), Int(Bits(0))));
            return;
        case ZYDIS_MNEMONIC_MOVSX:
        case ZYDIS_MNEMONIC_MOVSXD:
            Write(0, _builder.CreateSExtOrTrunc(Read(1), Int(Bits(0))));
            return;
        case ZYDIS_MNEMONIC_LEA:
            Write(0, _builder.CreateTrunc(Address(Operand(1), false), Int(Bits(0))));
            return;
        case ZYDIS_MNEMONIC_ADD:
            if (LockedMemory()) return AtomicRmw(llvm::AtomicRMWInst::Add, ReadAs(1, Bits(0)), mnemonic);
            Write(0, Add(Read(0), ReadAs(1, Bits(0)), _builder.getFalse()));
            return;
        case ZYDIS_MNEMONIC_ADC:
            Write(0, Add(Read(0), ReadAs(1, Bits(0)), Flag(PS5LIFT_CF)));
            return;
        case ZYDIS_MNEMONIC_SUB:
            if (LockedMemory()) return AtomicRmw(llvm::AtomicRMWInst::Sub, ReadAs(1, Bits(0)), mnemonic);
            Write(0, Sub(Read(0), ReadAs(1, Bits(0)), _builder.getFalse()));
            return;
        case ZYDIS_MNEMONIC_SBB:
            Write(0, Sub(Read(0), ReadAs(1, Bits(0)), Flag(PS5LIFT_CF)));
            return;
        case ZYDIS_MNEMONIC_CMP:
            Sub(Read(0), ReadAs(1, Bits(0)), _builder.getFalse());
            return;
        case ZYDIS_MNEMONIC_AND:
        case ZYDIS_MNEMONIC_OR:
        case ZYDIS_MNEMONIC_XOR: {
            const auto operation = mnemonic == ZYDIS_MNEMONIC_AND ? llvm::Instruction::And : mnemonic == ZYDIS_MNEMONIC_OR ? llvm::Instruction::Or : llvm::Instruction::Xor;
            if (LockedMemory()) {
                const auto rmw = mnemonic == ZYDIS_MNEMONIC_AND ? llvm::AtomicRMWInst::And : mnemonic == ZYDIS_MNEMONIC_OR ? llvm::AtomicRMWInst::Or : llvm::AtomicRMWInst::Xor;
                return AtomicRmw(rmw, ReadAs(1, Bits(0)), mnemonic);
            }
            auto* result = _builder.CreateBinOp(operation, Read(0), ReadAs(1, Bits(0)));
            LogicFlags(result);
            Write(0, result);
            return;
        }
        case ZYDIS_MNEMONIC_TEST:
            LogicFlags(_builder.CreateAnd(Read(0), ReadAs(1, Bits(0))));
            return;
        case ZYDIS_MNEMONIC_INC:
            if (LockedMemory()) return AtomicRmw(llvm::AtomicRMWInst::Add, Const(Bits(0), 1), mnemonic);
            Write(0, Add(Read(0), Const(Bits(0), 1), _builder.getFalse(), false));
            return;
        case ZYDIS_MNEMONIC_DEC:
            if (LockedMemory()) return AtomicRmw(llvm::AtomicRMWInst::Sub, Const(Bits(0), 1), mnemonic);
            Write(0, Sub(Read(0), Const(Bits(0), 1), _builder.getFalse(), false));
            return;
        case ZYDIS_MNEMONIC_NEG: {
            auto* value = Read(0);
            auto* result = Sub(llvm::ConstantInt::get(value->getType(), 0), value, _builder.getFalse());
            SetFlag(PS5LIFT_CF, _builder.CreateICmpNE(value, llvm::ConstantInt::get(value->getType(), 0)));
            Write(0, result);
            return;
        }
        case ZYDIS_MNEMONIC_NOT:
            Write(0, _builder.CreateNot(Read(0)));
            return;
        case ZYDIS_MNEMONIC_SHL:
        case ZYDIS_MNEMONIC_SHR:
        case ZYDIS_MNEMONIC_SAR:
        case ZYDIS_MNEMONIC_ROL:
        case ZYDIS_MNEMONIC_ROR:
            Shift(mnemonic);
            return;
        case ZYDIS_MNEMONIC_IMUL:
            SignedMultiply();
            return;
        case ZYDIS_MNEMONIC_MUL:
            Multiply(false);
            return;
        case ZYDIS_MNEMONIC_DIV:
            Divide(false);
            return;
        case ZYDIS_MNEMONIC_IDIV:
            Divide(true);
            return;
        case ZYDIS_MNEMONIC_CBW:
        case ZYDIS_MNEMONIC_CWDE:
        case ZYDIS_MNEMONIC_CDQE:
        case ZYDIS_MNEMONIC_CWD:
        case ZYDIS_MNEMONIC_CDQ:
        case ZYDIS_MNEMONIC_CQO:
            SignExtendAccumulator(mnemonic);
            return;
        case ZYDIS_MNEMONIC_BSF:
        case ZYDIS_MNEMONIC_BSR:
        case ZYDIS_MNEMONIC_TZCNT:
        case ZYDIS_MNEMONIC_LZCNT:
        case ZYDIS_MNEMONIC_POPCNT:
            BitScan(mnemonic);
            return;
        case ZYDIS_MNEMONIC_BT:
        case ZYDIS_MNEMONIC_BTS:
        case ZYDIS_MNEMONIC_BTR:
        case ZYDIS_MNEMONIC_BTC:
            BitTest(mnemonic);
            return;
        case ZYDIS_MNEMONIC_XCHG:
            Exchange();
            return;
        case ZYDIS_MNEMONIC_XADD:
            ExchangeAdd();
            return;
        case ZYDIS_MNEMONIC_CMPXCHG:
            CompareExchange();
            return;
        case ZYDIS_MNEMONIC_PUSH:
            Push(_builder.CreateSExt(Read(0), I64()));
            return;
        case ZYDIS_MNEMONIC_POP:
            Write(0, Pop());
            return;
        case ZYDIS_MNEMONIC_LEAVE:
            SetGpr(PS5LIFT_RSP, Gpr(PS5LIFT_RBP));
            SetGpr(PS5LIFT_RBP, Pop());
            return;
        case ZYDIS_MNEMONIC_CLD:
            SetFlag(PS5LIFT_DF, _builder.getFalse());
            return;
        case ZYDIS_MNEMONIC_STD:
            SetFlag(PS5LIFT_DF, _builder.getTrue());
            return;
        case ZYDIS_MNEMONIC_CLC:
            SetFlag(PS5LIFT_CF, _builder.getFalse());
            return;
        case ZYDIS_MNEMONIC_STC:
            SetFlag(PS5LIFT_CF, _builder.getTrue());
            return;
        case ZYDIS_MNEMONIC_CMC:
            SetFlag(PS5LIFT_CF, _builder.CreateNot(Flag(PS5LIFT_CF)));
            return;
        case ZYDIS_MNEMONIC_MOVSB: StringOp(StringOperation::Movs, 1); return;
        case ZYDIS_MNEMONIC_MOVSW: StringOp(StringOperation::Movs, 2); return;
        case ZYDIS_MNEMONIC_MOVSQ: StringOp(StringOperation::Movs, 8); return;
        case ZYDIS_MNEMONIC_STOSB: StringOp(StringOperation::Stos, 1); return;
        case ZYDIS_MNEMONIC_STOSW: StringOp(StringOperation::Stos, 2); return;
        case ZYDIS_MNEMONIC_STOSD: StringOp(StringOperation::Stos, 4); return;
        case ZYDIS_MNEMONIC_STOSQ: StringOp(StringOperation::Stos, 8); return;
        case ZYDIS_MNEMONIC_MOVSD:
            if (_instruction->Info.operand_count_visible == 0) return StringOp(StringOperation::Movs, 4);
            VectorMove();
            return;
        case ZYDIS_MNEMONIC_MOVAPS: case ZYDIS_MNEMONIC_MOVUPS: case ZYDIS_MNEMONIC_MOVAPD: case ZYDIS_MNEMONIC_MOVUPD:
        case ZYDIS_MNEMONIC_MOVDQA: case ZYDIS_MNEMONIC_MOVDQU: case ZYDIS_MNEMONIC_MOVQ: case ZYDIS_MNEMONIC_MOVD:
        case ZYDIS_MNEMONIC_MOVSS: case ZYDIS_MNEMONIC_VMOVSS: case ZYDIS_MNEMONIC_VMOVSD:
        case ZYDIS_MNEMONIC_VMOVAPS: case ZYDIS_MNEMONIC_VMOVUPS: case ZYDIS_MNEMONIC_VMOVAPD: case ZYDIS_MNEMONIC_VMOVUPD:
        case ZYDIS_MNEMONIC_VMOVDQA: case ZYDIS_MNEMONIC_VMOVDQU: case ZYDIS_MNEMONIC_VMOVQ: case ZYDIS_MNEMONIC_VMOVD:
        case ZYDIS_MNEMONIC_MOVNTDQ: case ZYDIS_MNEMONIC_VMOVNTDQ: case ZYDIS_MNEMONIC_MOVNTPS: case ZYDIS_MNEMONIC_VMOVNTPS:
        case ZYDIS_MNEMONIC_MOVNTDQA: case ZYDIS_MNEMONIC_VMOVNTDQA: case ZYDIS_MNEMONIC_LDDQU: case ZYDIS_MNEMONIC_VLDDQU:
            if (_instruction->Info.operand_count_visible == 3 && (mnemonic == ZYDIS_MNEMONIC_VMOVSS || mnemonic == ZYDIS_MNEMONIC_VMOVSD)) {
                const auto element = mnemonic == ZYDIS_MNEMONIC_VMOVSS ? 32u : 64u;
                auto* upper = AsVector(ReadAs(1, 128), element);
                auto* low = _builder.CreateExtractElement(AsVector(ReadAs(2, 128), element), _builder.getInt64(0));
                Write(0, AsInteger(_builder.CreateInsertElement(upper, low, _builder.getInt64(0))));
                return;
            }
            if (_instruction->Info.operand_count_visible != 2) throw UnsupportedInstruction("merge form");
            VectorMove();
            return;
        case ZYDIS_MNEMONIC_PXOR: case ZYDIS_MNEMONIC_XORPS: case ZYDIS_MNEMONIC_XORPD:
        case ZYDIS_MNEMONIC_PAND: case ZYDIS_MNEMONIC_ANDPS: case ZYDIS_MNEMONIC_ANDPD:
        case ZYDIS_MNEMONIC_POR: case ZYDIS_MNEMONIC_ORPS: case ZYDIS_MNEMONIC_ORPD:
        case ZYDIS_MNEMONIC_VPXOR: case ZYDIS_MNEMONIC_VXORPS: case ZYDIS_MNEMONIC_VXORPD:
        case ZYDIS_MNEMONIC_VPAND: case ZYDIS_MNEMONIC_VANDPS: case ZYDIS_MNEMONIC_VANDPD:
        case ZYDIS_MNEMONIC_VPOR: case ZYDIS_MNEMONIC_VORPS: case ZYDIS_MNEMONIC_VORPD:
            VectorBitwise(mnemonic);
            return;
        case ZYDIS_MNEMONIC_ANDN: case ZYDIS_MNEMONIC_SHLX: case ZYDIS_MNEMONIC_SHRX: case ZYDIS_MNEMONIC_SARX:
        case ZYDIS_MNEMONIC_RORX: case ZYDIS_MNEMONIC_BEXTR: case ZYDIS_MNEMONIC_BZHI: case ZYDIS_MNEMONIC_BLSR:
        case ZYDIS_MNEMONIC_BLSI: case ZYDIS_MNEMONIC_BLSMSK: case ZYDIS_MNEMONIC_MULX:
            BitManipulation(mnemonic);
            return;
        case ZYDIS_MNEMONIC_VPBROADCASTB: case ZYDIS_MNEMONIC_VPBROADCASTW: case ZYDIS_MNEMONIC_VPBROADCASTD: case ZYDIS_MNEMONIC_VPBROADCASTQ:
            Broadcast();
            return;
        case ZYDIS_MNEMONIC_VZEROUPPER:
            for (int i = 0; i < VectorCount; ++i) SetVector(i, _builder.CreateZExt(_builder.CreateTrunc(Vector(i), Int(128)), Int(256)));
            return;
        case ZYDIS_MNEMONIC_CALL: {
            auto target = DirectTarget(*_instruction);
            if (!target) target = ImportTarget(_module.Image(), *_instruction);
            auto* destination = target ? nullptr : Read(0);
            Push(_builder.getInt64(_instruction->Next()));
            if (target) CallGuest(_module.Guest(*target));
            else CallGuest(DispatchCallee().getCallee(), {destination});
            return;
        }
        case ZYDIS_MNEMONIC_JMP: {
            const auto target = DirectTarget(*_instruction);
            if (const auto import = target ? std::nullopt : ImportTarget(_module.Image(), *_instruction)) {
                TailCall(*import);
                return;
            }
            if (target) {
                _builder.CreateBr(Target(*target));
                return;
            }
            auto* destination = Read(0);
            const auto table = _function.JumpTables.find(_instruction->Address);
            auto* fallback = llvm::BasicBlock::Create(Context(), "dispatch", _host);
            if (table != _function.JumpTables.end()) {
                auto* selector = _builder.CreateSwitch(destination, fallback, static_cast<unsigned>(table->second.size()));
                std::set<std::uint64_t> seen;
                for (const auto target : table->second) {
                    if (seen.insert(target).second) selector->addCase(_builder.getInt64(target), Target(target));
                }
            } else {
                _builder.CreateBr(fallback);
            }
            _builder.SetInsertPoint(fallback);
            Flush();
            _builder.CreateCall(DispatchCallee(), {_state, destination});
            _builder.CreateRetVoid();
            return;
        }
        case ZYDIS_MNEMONIC_RET: {
            auto* pop = _builder.getInt64(8 + (_instruction->Info.operand_count_visible == 1 ? Operand(0).imm.value.u : 0));
            SetGpr(PS5LIFT_RSP, _builder.CreateAdd(Gpr(PS5LIFT_RSP), pop));
            Flush();
            _builder.CreateRetVoid();
            return;
        }
        case ZYDIS_MNEMONIC_UD2:
        case ZYDIS_MNEMONIC_INT3:
        case ZYDIS_MNEMONIC_HLT:
            Trap();
            return;
        default:
            if (EmitVector(mnemonic)) return;
            if (EmitFloat(mnemonic)) return;
            if (EmitX87(mnemonic)) return;
            throw UnsupportedInstruction("");
        }
    }
};

void ModuleLifter::LiftFunction(const GuestFunction& function) {
    FunctionLifter(*this, function).Lift();
}

}

std::unique_ptr<llvm::Module> LiftImage(llvm::LLVMContext& context, const GuestImage& image, const std::map<std::uint64_t, GuestFunction>& functions, const LiftOptions& options, LiftReport& report) {
    return ModuleLifter(context, image, functions, options, report).Lift();
}

}
