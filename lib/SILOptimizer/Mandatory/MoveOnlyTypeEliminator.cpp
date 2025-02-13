//===--- SILMoveOnlyTypeEliminator.cpp ------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// This file contains an optimizer pass that lowers away move only types from
/// SIL. It can run on all types or just trivial types. It works by Walking all
/// values in the IR and unsafely converting their type to be without move
/// only. If a change is made, we add the defining instruction to a set list for
/// post-processing. Once we have updated all types in the function, we revisit
/// the instructions that we touched and update/delete them as appropriate.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-move-only-type-eliminator"

#include "swift/AST/DiagnosticsSIL.h"
#include "swift/Basic/Defer.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Analysis/ClosureScope.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/NonLocalAccessBlockAnalysis.h"
#include "swift/SILOptimizer/Analysis/PostOrderAnalysis.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CanonicalOSSALifetime.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                                  Visitor
//===----------------------------------------------------------------------===//

namespace {

struct SILMoveOnlyTypeEliminatorVisitor
    : SILInstructionVisitor<SILMoveOnlyTypeEliminatorVisitor, bool> {
  const SmallSetVector<SILArgument *, 8> &touchedArgs;

  SILMoveOnlyTypeEliminatorVisitor(
      const SmallSetVector<SILArgument *, 8> &touchedArgs)
      : touchedArgs(touchedArgs) {}

  bool visitSILInstruction(SILInstruction *inst) {
    llvm::errs() << "Unhandled SIL Instruction: " << *inst;
    llvm_unreachable("error");
  }

  bool eraseFromParent(SILInstruction *i) {
    LLVM_DEBUG(llvm::dbgs() << "Erasing Inst: " << *i);
    i->eraseFromParent();
    return true;
  }

  bool visitLoadInst(LoadInst *li) {
    if (!li->getType().isTrivial(*li->getFunction()))
      return false;
    li->setOwnershipQualifier(LoadOwnershipQualifier::Trivial);
    return true;
  }

  bool visitStoreInst(StoreInst *si) {
    if (!si->getSrc()->getType().isTrivial(*si->getFunction()))
      return false;
    si->setOwnershipQualifier(StoreOwnershipQualifier::Trivial);
    return true;
  }

  bool visitStoreBorrowInst(StoreBorrowInst *si) {
    if (!si->getSrc()->getType().isTrivial(*si->getFunction()))
      return false;
    SILBuilderWithScope b(si);
    b.emitStoreValueOperation(si->getLoc(), si->getSrc(), si->getDest(),
                              StoreOwnershipQualifier::Trivial);
    return eraseFromParent(si);
  }

  bool visitLoadBorrowInst(LoadBorrowInst *li) {
    if (!li->getType().isTrivial(*li->getFunction()))
      return false;
    SILBuilderWithScope b(li);
    auto newVal = b.emitLoadValueOperation(li->getLoc(), li->getOperand(),
                                           LoadOwnershipQualifier::Trivial);
    li->replaceAllUsesWith(newVal);
    return eraseFromParent(li);
  }

#define RAUW_IF_TRIVIAL_RESULT(CLS)                                            \
  bool visit##CLS##Inst(CLS##Inst *inst) {                                     \
    if (!inst->getType().isTrivial(*inst->getFunction())) {                    \
      return false;                                                            \
    }                                                                          \
    inst->replaceAllUsesWith(inst->getOperand());                              \
    return eraseFromParent(inst);                                              \
  }
  RAUW_IF_TRIVIAL_RESULT(CopyValue)
  RAUW_IF_TRIVIAL_RESULT(ExplicitCopyValue)
  RAUW_IF_TRIVIAL_RESULT(BeginBorrow)
#undef RAUW_IF_TRIVIAL_RESULT

#define RAUW_ALWAYS(CLS)                                                       \
  bool visit##CLS##Inst(CLS##Inst *inst) {                                     \
    inst->replaceAllUsesWith(inst->getOperand());                              \
    return eraseFromParent(inst);                                              \
  }
  RAUW_ALWAYS(MoveOnlyWrapperToCopyableValue)
  RAUW_ALWAYS(CopyableToMoveOnlyWrapperValue)
#undef RAUW_ALWAYS

#define DELETE_IF_TRIVIAL_OP(CLS)                                              \
  bool visit##CLS##Inst(CLS##Inst *inst) {                                     \
    if (!inst->getOperand()->getType().isTrivial(*inst->getFunction())) {      \
      return false;                                                            \
    }                                                                          \
    return eraseFromParent(inst);                                              \
  }
  DELETE_IF_TRIVIAL_OP(DestroyValue)
  DELETE_IF_TRIVIAL_OP(EndBorrow)
#undef DELETE_IF_TRIVIAL_OP

#define NEED_TO_CONVERT_FORWARDING_TO_NONE_IF_TRIVIAL_OP(CLS)                  \
  bool visit##CLS##Inst(CLS##Inst *inst) {                                     \
    if (!inst->getOperand()->getType().isTrivial(*inst->getFunction()))        \
      return false;                                                            \
    inst->setForwardingOwnershipKind(OwnershipKind::None);                     \
    return true;                                                               \
  }
  NEED_TO_CONVERT_FORWARDING_TO_NONE_IF_TRIVIAL_OP(StructExtract)
  NEED_TO_CONVERT_FORWARDING_TO_NONE_IF_TRIVIAL_OP(TupleExtract)
  NEED_TO_CONVERT_FORWARDING_TO_NONE_IF_TRIVIAL_OP(UncheckedEnumData)
  NEED_TO_CONVERT_FORWARDING_TO_NONE_IF_TRIVIAL_OP(SwitchEnum)
#undef NEED_TO_CONVERT_FORWARDING_TO_NONE_IF_TRIVIAL_OP

#define NEED_TO_CONVERT_FORWARDING_TO_NONE_IF_TRIVIAL_RESULT(CLS)              \
  bool visit##CLS##Inst(CLS##Inst *inst) {                                     \
    if (!inst->getType().isTrivial(*inst->getFunction()))                      \
      return false;                                                            \
    inst->setForwardingOwnershipKind(OwnershipKind::None);                     \
    return true;                                                               \
  }
  NEED_TO_CONVERT_FORWARDING_TO_NONE_IF_TRIVIAL_RESULT(Enum)
  NEED_TO_CONVERT_FORWARDING_TO_NONE_IF_TRIVIAL_RESULT(Struct)
  NEED_TO_CONVERT_FORWARDING_TO_NONE_IF_TRIVIAL_RESULT(Tuple)
#undef NEED_TO_CONVERT_FORWARDING_TO_NONE_IF_TRIVIAL_RESULT

#define NO_UPDATE_NEEDED(CLS)                                                  \
  bool visit##CLS##Inst(CLS##Inst *inst) { return false; }
  NO_UPDATE_NEEDED(AllocStack)
  NO_UPDATE_NEEDED(DebugValue)
  NO_UPDATE_NEEDED(StructElementAddr)
  NO_UPDATE_NEEDED(TupleElementAddr)
  NO_UPDATE_NEEDED(UncheckedTakeEnumDataAddr)
  NO_UPDATE_NEEDED(DestructureTuple)
  NO_UPDATE_NEEDED(DestructureStruct)
  NO_UPDATE_NEEDED(SelectEnum)
  NO_UPDATE_NEEDED(SelectValue)
  NO_UPDATE_NEEDED(MarkDependence)
  NO_UPDATE_NEEDED(DestroyAddr)
  NO_UPDATE_NEEDED(DeallocStack)
  NO_UPDATE_NEEDED(Branch)
  NO_UPDATE_NEEDED(UncheckedAddrCast)
  NO_UPDATE_NEEDED(RefElementAddr)
  NO_UPDATE_NEEDED(Upcast)
  NO_UPDATE_NEEDED(CheckedCastBranch)
  NO_UPDATE_NEEDED(Object)
  NO_UPDATE_NEEDED(OpenExistentialRef)
  NO_UPDATE_NEEDED(ConvertFunction)
  NO_UPDATE_NEEDED(RefToBridgeObject)
  NO_UPDATE_NEEDED(BridgeObjectToRef)
  NO_UPDATE_NEEDED(UnconditionalCheckedCast)
  NO_UPDATE_NEEDED(ClassMethod)
#undef NO_UPDATE_NEEDED
};

} // namespace

//===----------------------------------------------------------------------===//
//                             Top Levelish Code?
//===----------------------------------------------------------------------===//

namespace {

struct SILMoveOnlyTypeEliminator {
  SILFunction *fn;
  bool trivialOnly;

  SILMoveOnlyTypeEliminator(SILFunction *fn, bool trivialOnly)
      : fn(fn), trivialOnly(trivialOnly) {}

  bool process();
};

} // namespace

bool SILMoveOnlyTypeEliminator::process() {
  bool madeChange = true;

  SmallSetVector<SILArgument *, 8> touchedArgs;
  SmallSetVector<SILInstruction *, 8> touchedInsts;

  for (auto &bb : *fn) {
    // We should (today) never have move only function arguments. Instead we
    // convert them in the prologue block.
    if (&bb != &fn->front()) {
      for (auto *arg : bb.getArguments()) {
        if (!arg->getType().isMoveOnlyWrapped())
          continue;

        // If we are looking at trivial only, skip non-trivial function args.
        if (trivialOnly &&
            !arg->getType().removingMoveOnlyWrapper().isTrivial(*fn))
          continue;

        arg->unsafelyEliminateMoveOnlyWrapper();

        // If our new type is trivial, convert the arguments ownership to
        // None. Otherwise, preserve the ownership kind of the argument.
        if (arg->getType().isTrivial(*fn))
          arg->setOwnershipKind(OwnershipKind::None);
        touchedArgs.insert(arg);
        for (auto *use : arg->getNonTypeDependentUses())
          touchedInsts.insert(use->getUser());
      }
    }

    for (auto &ii : bb) {
      for (SILValue v : ii.getResults()) {
        if (!v->getType().isMoveOnlyWrapped())
          continue;

        if (trivialOnly &&
            !v->getType().removingMoveOnlyWrapper().isTrivial(*fn))
          continue;

        v->unsafelyEliminateMoveOnlyWrapper();
        touchedInsts.insert(&ii);

        // Add all users as well. This ensures we visit things like
        // destroy_value and end_borrow.
        for (auto *use : v->getNonTypeDependentUses())
          touchedInsts.insert(use->getUser());
        madeChange = true;
      }
    }
  }

  SILMoveOnlyTypeEliminatorVisitor visitor(touchedArgs);
  while (!touchedInsts.empty()) {
    visitor.visit(touchedInsts.pop_back_val());
  }

  return madeChange;
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

struct SILMoveOnlyTypeEliminatorPass : SILFunctionTransform {
  bool trivialOnly;

  SILMoveOnlyTypeEliminatorPass(bool trivialOnly)
      : SILFunctionTransform(), trivialOnly(trivialOnly) {}

  void run() override {
    auto *fn = getFunction();

    // Don't rerun on deserialized functions. We lower trivial things earlier
    // during Raw SIL.
    if (getFunction()->wasDeserializedCanonical())
      return;

    assert(fn->getModule().getStage() == SILStage::Raw &&
           "Should only run on Raw SIL");

    if (SILMoveOnlyTypeEliminator(getFunction(), trivialOnly).process()) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
    }
  }
};

} // anonymous namespace

SILTransform *swift::createTrivialMoveOnlyTypeEliminator() {
  return new SILMoveOnlyTypeEliminatorPass(true /*trivial only*/);
}

SILTransform *swift::createMoveOnlyTypeEliminator() {
  return new SILMoveOnlyTypeEliminatorPass(false /*trivial only*/);
}
