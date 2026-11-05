#include "../FunctionLifter.hpp"

#include <optional>
#include <set>

namespace Ps5Lift::Lifting {

llvm::Value* FunctionLifter::Loop(const ZydisMnemonic mnemonic) {
    const auto counter = _instruction->Info.address_width == 64 ? ZYDIS_REGISTER_RCX : ZYDIS_REGISTER_ECX;
    auto* count = _builder.CreateSub(ReadRegister(counter), llvm::ConstantInt::get(Int(_instruction->Info.address_width), 1));
    WriteRegister(counter, count);
    auto* running = _builder.CreateICmpNE(count, llvm::ConstantInt::get(count->getType(), 0));
    if (mnemonic == ZYDIS_MNEMONIC_LOOPE) return _builder.CreateAnd(running, Flag(PS5LIFT_ZF));
    if (mnemonic == ZYDIS_MNEMONIC_LOOPNE) return _builder.CreateAnd(running, _builder.CreateNot(Flag(PS5LIFT_ZF)));
    return running;
}

void FunctionLifter::Branch(llvm::Value* condition) {
    _builder.CreateCondBr(condition, BranchTarget(), Target(_instruction->Next()));
}

llvm::Value* FunctionLifter::CounterIsZero(const unsigned bits) {
    return _builder.CreateICmpEQ(_builder.CreateTrunc(Gpr(PS5LIFT_RCX), Int(bits)), Const(bits, 0));
}

void FunctionLifter::ConditionalMove(const Condition condition) {
    Write(0, _builder.CreateSelect(Test(condition), Read(1), Read(0)));
}

void FunctionLifter::SetCondition(const Condition condition) {
    Write(0, _builder.CreateZExt(Test(condition), _builder.getInt8Ty()));
}

void FunctionLifter::Leave() {
    SetGpr(PS5LIFT_RSP, Gpr(PS5LIFT_RBP));
    if (_instruction->Info.operand_width == 64) SetGpr(PS5LIFT_RBP, Pop());
    else WriteRegister(ZYDIS_REGISTER_BP, Pop(16));
}

void FunctionLifter::Enter() {
    const auto bits = _instruction->Info.operand_width;
    const auto bytes = bits / 8;
    const auto level = Operand(1).imm.value.u & 31;
    Push(_builder.CreateTrunc(Gpr(PS5LIFT_RBP), Int(bits)));
    auto* frame = Gpr(PS5LIFT_RSP);
    for (std::uint64_t i = 1; i < level; ++i) {
        SetGpr(PS5LIFT_RBP, _builder.CreateSub(Gpr(PS5LIFT_RBP), _builder.getInt64(bytes)));
        Push(Load(Gpr(PS5LIFT_RBP), bits, false));
    }
    if (level > 0) Push(_builder.CreateTrunc(frame, Int(bits)));
    if (bits == 64) SetGpr(PS5LIFT_RBP, frame);
    else WriteRegister(ZYDIS_REGISTER_BP, _builder.CreateTrunc(frame, Int(16)));
    SetGpr(PS5LIFT_RSP, _builder.CreateSub(Gpr(PS5LIFT_RSP), _builder.getInt64(Operand(0).imm.value.u)));
}

void FunctionLifter::TableLookup() {
    const auto& table = Operand(0);
    const auto width = _instruction->Info.address_width;
    auto* index = _builder.CreateAdd(ReadRegister(table.mem.base), _builder.CreateZExt(ReadRegister(ZYDIS_REGISTER_AL), Int(width)));
    auto* address = SegmentBase(_builder.CreateZExt(index, I64()), table.mem.segment);
    Write(1, Load(address, 8, NeedsOrdering(table, 8)));
}

void FunctionLifter::Call() {
    auto target = DirectTarget(*_instruction);
    if (!target) target = ImportTarget(_module.Image(), *_instruction);
    if (target && !DispatchesDirectTarget(_module.Image(), *_instruction)) return PushAndCall(_module.Guest(*target), _module.Functions().at(*target).Effects);
    auto* destination = target ? _builder.getInt64(*target) : Read(0);
    PushAndCall(DispatchCallee().getCallee(), {EveryRegister, EveryRegister}, {destination});
}

void FunctionLifter::PushAndCall(llvm::Value* callee, const RegisterEffects& effects, llvm::ArrayRef<llvm::Value*> extra) {
    Push(Const(_instruction->Info.operand_width, _instruction->Next()));
    CallGuest(callee, effects, extra);
}

void FunctionLifter::Jump() {
    const auto target = DirectTarget(*_instruction);
    if (const auto import = target ? std::nullopt : ImportTarget(_module.Image(), *_instruction)) {
        TailCall(*import);
        return;
    }
    if (target) {
        _builder.CreateBr(BranchTarget());
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
    if (_function.ClosedJumpTables.contains(_instruction->Address)) {
        auto left = _module.Runtime("ps5lift_left_jump_table", llvm::FunctionType::get(_builder.getVoidTy(), {_module.Pointer(), I64()}, false));
        _builder.CreateCall(left, {_state, _builder.getInt64(_instruction->Address)})->setDoesNotReturn();
        _builder.CreateUnreachable();
        return;
    }
    TailDispatch(destination);
}

void FunctionLifter::Return() {
    auto* rsp = Gpr(PS5LIFT_RSP);
    auto* popped = _builder.CreateAdd(rsp, _builder.getInt64(8 + (_instruction->Info.operand_count_visible == 1 ? Operand(0).imm.value.u : 0)));
    if (_assignsStackPointer) ResumeIfStackSwitched(rsp, popped);
    StateStore(rsp, offsetof(Ps5LiftState, ReturnSlot));
    SetGpr(PS5LIFT_RSP, popped);
    Flush();
    _builder.CreateRetVoid();
}

}
