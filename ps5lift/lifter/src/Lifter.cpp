#include <lifter/Lifter.hpp>

#include <ps5lift/State.h>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <cstddef>
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
    llvm::PointerType* Pointer() const { return _pointer; }
    llvm::FunctionType* GuestType() const { return _guestType; }
    const LiftOptions& Options() const { return _options; }
    LiftReport& Report() { return _report; }
    const std::map<std::uint64_t, GuestFunction>& Functions() const { return _functions; }

private:
    llvm::LLVMContext& _context;
    const GuestImage& _image;
    const std::map<std::uint64_t, GuestFunction>& _functions;
    const LiftOptions& _options;
    LiftReport& _report;
    std::unique_ptr<llvm::Module> _module;
    llvm::PointerType* _pointer = nullptr;
    llvm::FunctionType* _guestType = nullptr;

    void LiftFunction(const GuestFunction& function);

    void LiftImport(const GuestFunction& function) {
        auto* host = Guest(function.Entry);
        llvm::IRBuilder<> builder(llvm::BasicBlock::Create(_context, "entry", host));
        auto* state = host->getArg(0);
        builder.CreateCall(Runtime("ps5lift_import_" + *function.Import, _guestType), {state});
        auto* rsp = builder.CreateConstGEP1_64(builder.getInt8Ty(), state, offsetof(Ps5LiftState, Gpr) + 8 * PS5LIFT_RSP);
        builder.CreateStore(builder.CreateAdd(builder.CreateLoad(builder.getInt64Ty(), rsp), builder.getInt64(8)), rsp);
        builder.CreateRetVoid();
    }

    void EmitImage() {
        auto* i64 = llvm::Type::getInt64Ty(_context);
        auto* i32 = llvm::Type::getInt32Ty(_context);
        auto* segmentType = llvm::StructType::create(_context, {i64, i64, i64, _pointer, i32}, "Ps5LiftSegment");
        auto* functionType = llvm::StructType::create(_context, {i64, _pointer}, "Ps5LiftFunctionEntry");
        auto* imageType = llvm::StructType::create(_context, {_pointer, i64, _pointer, i64, i64}, "Ps5LiftImage");

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
        auto* image = llvm::ConstantStruct::get(imageType, {segmentGlobal, llvm::ConstantInt::get(i64, segments.size()), functionGlobal, llvm::ConstantInt::get(i64, entries.size()), llvm::ConstantInt::get(i64, _image.Entry())});
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
        for (int i = 0; i < PS5LIFT_GPR_COUNT; ++i) _gpr[i] = _builder.CreateAlloca(I64(), nullptr, "gpr" + std::to_string(i));
        for (int i = 0; i < PS5LIFT_FLAG_COUNT; ++i) _flags[i] = _builder.CreateAlloca(_builder.getInt1Ty(), nullptr, "flag" + std::to_string(i));
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

    void Flush() {
        for (int i = 0; i < PS5LIFT_GPR_COUNT; ++i) _builder.CreateStore(_builder.CreateLoad(I64(), _gpr[i]), StateField(offsetof(Ps5LiftState, Gpr) + 8 * i));
        for (int i = 0; i < PS5LIFT_FLAG_COUNT; ++i) _builder.CreateStore(_builder.CreateZExt(_builder.CreateLoad(_builder.getInt1Ty(), _flags[i]), _builder.getInt8Ty()), StateField(offsetof(Ps5LiftState, Flags) + i));
    }

    void Reload() {
        for (int i = 0; i < PS5LIFT_GPR_COUNT; ++i) _builder.CreateStore(_builder.CreateLoad(I64(), StateField(offsetof(Ps5LiftState, Gpr) + 8 * i)), _gpr[i]);
        for (int i = 0; i < PS5LIFT_FLAG_COUNT; ++i) _builder.CreateStore(_builder.CreateTrunc(_builder.CreateLoad(_builder.getInt8Ty(), StateField(offsetof(Ps5LiftState, Flags) + i)), _builder.getInt1Ty()), _flags[i]);
    }

    void StartDeadBlock() {
        _builder.SetInsertPoint(llvm::BasicBlock::Create(Context(), "dead", _host));
    }

    llvm::Value* Gpr(const int index) { return _builder.CreateLoad(I64(), _gpr[index]); }
    void SetGpr(const int index, llvm::Value* value) { _builder.CreateStore(value, _gpr[index]); }
    llvm::Value* Flag(const int flag) { return _builder.CreateLoad(_builder.getInt1Ty(), _flags[flag]); }
    void SetFlag(const int flag, llvm::Value* value) { _builder.CreateStore(value, _flags[flag]); }

    llvm::Value* VectorField(const int index) {
        return StateField(offsetof(Ps5LiftState, Vector) + 32 * static_cast<std::size_t>(index));
    }

    llvm::Value* ReadRegister(const ZydisRegister reg) {
        if (reg == ZYDIS_REGISTER_RIP) return _builder.getInt64(_instruction->Next());
        const auto vector = VectorIndex(reg);
        if (vector >= 0) return _builder.CreateLoad(Int(ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, reg)), VectorField(vector));
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
            _builder.CreateStore(value, VectorField(vector));
            if (_instruction->Info.encoding == ZYDIS_INSTRUCTION_ENCODING_VEX && value->getType()->getIntegerBitWidth() == 128)
                _builder.CreateStore(llvm::ConstantInt::get(Int(128), 0), _builder.CreateConstGEP1_64(_builder.getInt8Ty(), VectorField(vector), 16));
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
        if (applySegment && operand.mem.segment == ZYDIS_REGISTER_FS) address = _builder.CreateAdd(address, _builder.CreateLoad(I64(), StateField(offsetof(Ps5LiftState, FsBase))));
        if (applySegment && operand.mem.segment == ZYDIS_REGISTER_GS) address = _builder.CreateAdd(address, _builder.CreateLoad(I64(), StateField(offsetof(Ps5LiftState, GsBase))));
        return address;
    }

    bool NeedsOrdering(const ZydisDecodedOperand& operand, const unsigned bits) const {
        if (_module.Options().Memory != MemoryModel::Tso || bits > 64) return false;
        return operand.mem.base != ZYDIS_REGISTER_RSP;
    }

    llvm::Value* Load(llvm::Value* address, const unsigned bits, const bool ordered) {
        auto* load = _builder.CreateLoad(Int(bits), _builder.CreateIntToPtr(address, _module.Pointer()));
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
        case ZYDIS_MNEMONIC_MOVSS:
        case ZYDIS_MNEMONIC_VMOVAPS: case ZYDIS_MNEMONIC_VMOVUPS: case ZYDIS_MNEMONIC_VMOVAPD: case ZYDIS_MNEMONIC_VMOVUPD:
        case ZYDIS_MNEMONIC_VMOVDQA: case ZYDIS_MNEMONIC_VMOVDQU: case ZYDIS_MNEMONIC_VMOVQ: case ZYDIS_MNEMONIC_VMOVD:
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
            for (int i = 0; i < 16; ++i) _builder.CreateStore(llvm::ConstantInt::get(Int(128), 0), _builder.CreateConstGEP1_64(_builder.getInt8Ty(), VectorField(i), 16));
            return;
        case ZYDIS_MNEMONIC_CALL: {
            const auto target = DirectTarget(*_instruction);
            auto* destination = target ? nullptr : Read(0);
            Push(_builder.getInt64(_instruction->Next()));
            if (target) CallGuest(_module.Guest(*target));
            else CallGuest(DispatchCallee().getCallee(), {destination});
            return;
        }
        case ZYDIS_MNEMONIC_JMP: {
            const auto target = DirectTarget(*_instruction);
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
