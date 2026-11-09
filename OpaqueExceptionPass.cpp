#include "OpaqueExceptionPass.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace llvm;

cl::opt<bool> OpaqueExceptionEnable("obfopaqueexception", cl::init(true), cl::desc("Master Toggle"));

static constexpr unsigned StashOffset = 0x180;

static std::mt19937_64& LocalRng()
{
    static std::mt19937_64 r{std::random_device{}()};
    return r;
}

static uint64_t RandomSentinel()
{
    std::uniform_int_distribution<uint64_t> d;
    uint64_t s = d(LocalRng());
    while (s == 0 || s == 1 || s == UINT64_MAX)
        s = d(LocalRng());
    return s;
}

static bool HasAnnotationTag(const Function& f, StringRef needle)
{
    const Module* m = f.getParent();
    if (!m) return false;

    const GlobalVariable* ga = m->getGlobalVariable("llvm.global.annotations");
    if (!ga || !ga->hasInitializer()) return false;

    auto* ca = dyn_cast<ConstantArray>(ga->getInitializer());
    if (!ca) return false;

    for (const Value* opv : ca->operands())
    {
        auto* cs = dyn_cast<ConstantStruct>(opv);
        if (!cs || cs->getNumOperands() < 2) continue;

        const Function* af = dyn_cast<Function>(cs->getOperand(0)->stripPointerCasts());
        if (af != &f) continue;

        auto* anngv = dyn_cast<GlobalVariable>(cs->getOperand(1)->stripPointerCasts());
        if (!anngv || !anngv->hasInitializer()) continue;

        auto* cds = dyn_cast<ConstantDataSequential>(anngv->getInitializer());
        if (!cds || !cds->isCString()) continue;

        if (cds->getAsCString() == needle) return true;
    }
    return false;
}

static bool HasOpqExcAnnotation(const Function& f) { return HasAnnotationTag(f, "opqexc"); }
static bool HasNoOpqExcAnnotation(const Function& f) { return HasAnnotationTag(f, "noopqexc"); }
static bool HasObfuscateAnnotation(const Function& f) { return HasAnnotationTag(f, "obfuscate"); }
static bool HasNoObfuscateAnnotation(const Function& f) { return HasAnnotationTag(f, "noobfuscate"); }

static bool HasInBodyReentry(const Function& f)
{
    for (const BasicBlock& bb : f)
    {
        for (const Instruction& i : bb)
        {
            if (isa<InvokeInst>(&i)) return true;
            if (isa<LandingPadInst>(&i)) return true;
            if (isa<CatchPadInst>(&i)) return true;
            if (isa<CleanupPadInst>(&i)) return true;
            if (isa<CatchSwitchInst>(&i)) return true;
            if (isa<CatchReturnInst>(&i)) return true;
            if (isa<CleanupReturnInst>(&i)) return true;

            if (auto* cb = dyn_cast<CallBase>(&i))
            {
                if (const Function* callee = cb->getCalledFunction())
                {
                    if (callee->hasFnAttribute(Attribute::ReturnsTwice))
                        return true;
                }
            }
        }
    }
    return false;
}

static bool IsEligible(const Function& f)
{
    if (f.isDeclaration() || f.empty()) return false;
    if (HasNoOpqExcAnnotation(f) || HasNoObfuscateAnnotation(f)) return false;
    if (!HasOpqExcAnnotation(f) && !HasObfuscateAnnotation(f)) return false;
    if (HasInBodyReentry(f)) return false;
    return true;
}

static void EmitSentinelWrite(IRBuilder<>& b, LLVMContext& c, uint64_t sentinel)
{
    FunctionType* ft = FunctionType::get(Type::getVoidTy(c), {}, false);

    char buf[160];
    std::snprintf(buf, sizeof(buf), "movabsq $$0x%016llx, %%rax\n\tmovq %%rax, -%u(%%rsp)", (unsigned long long)sentinel, (unsigned)StashOffset);
    InlineAsm* a = InlineAsm::get(ft, std::string(buf), "~{rax},~{memory}", true, false, InlineAsm::AD_ATT);
    b.CreateCall(ft, a, {}, "");
}

static void EmitTriggerAV(IRBuilder<>& b, LLVMContext& c)
{
    FunctionType* ft = FunctionType::get(Type::getVoidTy(c), {}, false);
    InlineAsm* a = InlineAsm::get(ft, ".byte 0xc6, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00", "~{memory}", true, false, InlineAsm::AD_ATT);
    b.CreateCall(ft, a, {}, "");
}

static Value* EmitSlotLoad(IRBuilder<>& b, LLVMContext& c)
{
    FunctionType* ft = FunctionType::get(Type::getInt64Ty(c), {}, false);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "movq -%u(%%rsp), $0", (unsigned)StashOffset);

    InlineAsm* a = InlineAsm::get(ft, std::string(buf), "=r,~{memory}", true, false, InlineAsm::AD_ATT);
    return b.CreateCall(ft, a, {}, "OpqSlot");
}

struct SiteRec
{
    BlockAddress* Addr;
    uint16_t InsnSize;
};

static BasicBlock* InsertSite(Function& f, uint64_t sentinel)
{
    LLVMContext& c = f.getContext();

    BasicBlock& entry = f.getEntryBlock();

    Instruction* splitpt = entry.getFirstNonPHIOrDbg();
    while (splitpt && isa<AllocaInst>(splitpt))
        splitpt = splitpt->getNextNode();

    if (!splitpt) return nullptr;

    BasicBlock* differentbb = entry.splitBasicBlock(splitpt, "OpqDifferent");
    entry.getTerminator()->eraseFromParent();

    BasicBlock* triggerbb = BasicBlock::Create(c, "OpqTrigger", &f, differentbb);
    BasicBlock* checkbb = BasicBlock::Create(c, "OpqCheck", &f, differentbb);
    BasicBlock* realbb = BasicBlock::Create(c, "OpqReal", &f, differentbb);

    {
        IRBuilder<> b(&entry);
        EmitSentinelWrite(b, c, sentinel);
        b.CreateBr(triggerbb);
    }

    {
        IRBuilder<> b(triggerbb);
        EmitTriggerAV(b, c);
        b.CreateBr(checkbb);
    }

    {
        IRBuilder<> b(checkbb);
        Value* slot = EmitSlotLoad(b, c);
        Value* s = ConstantInt::get(Type::getInt64Ty(c), sentinel);
        Value* eq = b.CreateICmpEQ(slot, s, "OpqEq");
        b.CreateCondBr(eq, realbb, differentbb);
    }

    {
        IRBuilder<> b(realbb);
        Type* retty = f.getReturnType();
        if (retty->isVoidTy())
            b.CreateRetVoid();
        else
            b.CreateRet(Constant::getNullValue(retty));
    }

    return triggerbb;
}

static Function* BuildVEHCallback(Module& m, GlobalVariable* addrgv, GlobalVariable* sizegv, GlobalVariable* countgv, Type* addrarrty, Type* sizearrty)
{
    LLVMContext& c = m.getContext();

    Type* i8 = Type::getInt8Ty(c);
    Type* i16 = Type::getInt16Ty(c);
    Type* i32 = Type::getInt32Ty(c);
    Type* i64 = Type::getInt64Ty(c);
    Type* i8p = PointerType::get(i8, 0);
    Type* i64p = PointerType::get(i64, 0);
    Type* i8pp = PointerType::get(i8p, 0);

    FunctionType* ft = FunctionType::get(i32, {i8p}, false);
    Function* f = Function::Create(ft, GlobalValue::InternalLinkage, "__opqexc_veh", &m);
    f->setCallingConv(CallingConv::Win64);
    f->addFnAttr(Attribute::NoInline);
    f->addFnAttr(Attribute::OptimizeNone);

    Argument* eparg = f->getArg(0);
    eparg->setName("Ep");

    BasicBlock* entrybb = BasicBlock::Create(c, "Entry", f);
    BasicBlock* loopbb = BasicBlock::Create(c, "Loop", f);
    BasicBlock* addrchkbb = BasicBlock::Create(c, "AddrCheck", f);
    BasicBlock* matchbb = BasicBlock::Create(c, "Match", f);
    BasicBlock* nextbb = BasicBlock::Create(c, "Next", f);
    BasicBlock* donebb = BasicBlock::Create(c, "Done", f);

    IRBuilder<> b(entrybb);

    Value* erpp = b.CreateBitCast(eparg, i8pp, "ErPP");
    Value* er = b.CreateLoad(i8p, erpp, "Er");

    Value* crppi8 = b.CreateGEP(i8, eparg, ConstantInt::get(i64, 8), "CrPPI8");
    Value* crpp = b.CreateBitCast(crppi8, i8pp, "CrPP");
    Value* cr = b.CreateLoad(i8p, crpp, "Cr");

    Value* addrppi8 = b.CreateGEP(i8, er, ConstantInt::get(i64, 16), "AddrPPI8");
    Value* addrpp = b.CreateBitCast(addrppi8, i8pp, "AddrPP");
    Value* faultaddr = b.CreateLoad(i8p, addrpp, "FaultAddr");

    Value* n = b.CreateLoad(i32, countgv, "N");
    b.CreateBr(loopbb);

    b.SetInsertPoint(loopbb);
    PHINode* iphi = b.CreatePHI(i32, 2, "I");
    iphi->addIncoming(ConstantInt::get(i32, 0), entrybb);
    Value* inrange = b.CreateICmpULT(iphi, n, "InRange");
    b.CreateCondBr(inrange, addrchkbb, donebb);

    b.SetInsertPoint(addrchkbb);
    Value* addrslotp = b.CreateGEP(addrarrty, addrgv, {ConstantInt::get(i64, 0), b.CreateZExt(iphi, i64)}, "AddrSlotP");
    Value* addrslot = b.CreateLoad(i8p, addrslotp, "AddrSlot");
    Value* addreq = b.CreateICmpEQ(addrslot, faultaddr, "AddrEq");
    b.CreateCondBr(addreq, matchbb, nextbb);

    b.SetInsertPoint(matchbb);
    Value* sizeslotp = b.CreateGEP(sizearrty, sizegv, {ConstantInt::get(i64, 0), b.CreateZExt(iphi, i64)}, "SizeSlotP");
    Value* sizeslot = b.CreateLoad(i16, sizeslotp, "SizeSlot");
    Value* sizeext = b.CreateZExt(sizeslot, i64, "SizeExt");

    Value* rippi8 = b.CreateGEP(i8, cr, ConstantInt::get(i64, 0xF8), "RipPI8");
    Value* ripp = b.CreateBitCast(rippi8, i64p, "RipP");
    Value* rip = b.CreateLoad(i64, ripp, "Rip");
    Value* ripnew = b.CreateAdd(rip, sizeext, "RipNew");
    b.CreateStore(ripnew, ripp);
  b.CreateRet(ConstantInt::get(i32, uint32_t(-1), true));

    b.SetInsertPoint(nextbb);
    Value* iplus = b.CreateAdd(iphi, ConstantInt::get(i32, 1), "IPlus");
    iphi->addIncoming(iplus, nextbb);
    b.CreateBr(loopbb);

    b.SetInsertPoint(donebb);
    b.CreateRet(ConstantInt::get(i32, 0));

    return f;
}

static Function* BuildInitFn(Module& m, Function* vehfn)
{
    LLVMContext& c = m.getContext();
    Type* i8 = Type::getInt8Ty(c);
    Type* i32 = Type::getInt32Ty(c);
    Type* i8p = PointerType::get(i8, 0);
    Type* voidty = Type::getVoidTy(c);

    FunctionType* avehty = FunctionType::get(i8p, {i32, i8p}, false);
    FunctionCallee aveh = m.getOrInsertFunction("AddVectoredExceptionHandler", avehty);
    if (auto* af = dyn_cast<Function>(aveh.getCallee()))
    {
        af->setCallingConv(CallingConv::Win64);
        af->setDLLStorageClass(GlobalValue::DLLImportStorageClass);
    }

    FunctionType* ft = FunctionType::get(voidty, {}, false);
    Function* f = Function::Create(ft, GlobalValue::InternalLinkage, "__opqexc_init", &m);
    f->setCallingConv(CallingConv::Win64);
    f->addFnAttr(Attribute::NoInline);
    f->addFnAttr(Attribute::OptimizeNone);

    BasicBlock* entrybb = BasicBlock::Create(c, "Entry", f);
    IRBuilder<> b(entrybb);

    Value* vehptr = b.CreateBitCast(vehfn, i8p);
    b.CreateCall(aveh, {ConstantInt::get(i32, 1), vehptr});
    b.CreateRetVoid();

    return f;
}

static void EmitCRTInitSlot(Module& m, Function* initfn)
{
    LLVMContext& c = m.getContext();
    Type* voidty = Type::getVoidTy(c);
    FunctionType* initfnty = FunctionType::get(voidty, {}, false);
    PointerType* initptrty = PointerType::get(initfnty, 0);

    auto* slot = new GlobalVariable(m, initptrty, true, GlobalValue::InternalLinkage, initfn, "__opqexc_crt_entry");
    slot->setSection(".CRT$XCU");
    appendToUsed(m, {slot});
}

static void EmitRuntime(Module& m, const std::vector<SiteRec>& sites)
{
    LLVMContext& c = m.getContext();
    Type* i8 = Type::getInt8Ty(c);
    Type* i16 = Type::getInt16Ty(c);
    Type* i32 = Type::getInt32Ty(c);
    Type* i8p = PointerType::get(i8, 0);

    unsigned n = (unsigned)sites.size();

    std::vector<Constant*> addrconsts, sizeconsts;
    addrconsts.reserve(n);
    sizeconsts.reserve(n);
    for (const auto& s : sites)
    {
        addrconsts.push_back(ConstantExpr::getBitCast(s.Addr, i8p));
        sizeconsts.push_back(ConstantInt::get(i16, s.InsnSize));
    }

    ArrayType* addrarrty = ArrayType::get(i8p, n);
    ArrayType* sizearrty = ArrayType::get(i16, n);

    auto* addrgv = new GlobalVariable(m, addrarrty, true, GlobalValue::InternalLinkage, ConstantArray::get(addrarrty, addrconsts), "__opqexc_addrs");
    auto* sizegv = new GlobalVariable(m, sizearrty, true, GlobalValue::InternalLinkage, ConstantArray::get(sizearrty, sizeconsts), "__opqexc_sizes");
    auto* countgv = new GlobalVariable(m, i32, true, GlobalValue::InternalLinkage, ConstantInt::get(i32, n), "__opqexc_count");

    Function* vehfn = BuildVEHCallback(m, addrgv, sizegv, countgv, addrarrty, sizearrty);
    Function* initfn = BuildInitFn(m, vehfn);
    EmitCRTInitSlot(m, initfn);

    appendToUsed(m, {addrgv, sizegv, countgv, vehfn, initfn});
}

bool RunOpaqueException(Module& m)
{
    if (!OpaqueExceptionEnable) return false;

    std::vector<SiteRec> sites;

    for (Function& f : m)
    {
        if (!IsEligible(f)) continue;

        uint64_t sentinel = RandomSentinel();
        BasicBlock* trig = InsertSite(f, sentinel);
        if (!trig) continue;

        BlockAddress* ba = BlockAddress::get(&f, trig);
     
        sites.push_back({ba, 8});
    }

    if (sites.empty()) return false;

    EmitRuntime(m, sites);
    return true;
}

PreservedAnalyses OpaqueExceptionPass::run(Module& m, ModuleAnalysisManager&)
{
    if (!RunOpaqueException(m)) return PreservedAnalyses::all();
    return PreservedAnalyses::none();
}