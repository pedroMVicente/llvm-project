//===- InstCombineBitInsert.cpp - combine BitInsert ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InstCombineInternal.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"

namespace llvm {

namespace {
// Helper to determine integer bitwidth of a type. Returns 0 if not integer-like.
static unsigned getBitWidth(Type *Ty) {
  if (auto *IT = dyn_cast<IntegerType>(Ty))
    return IT->getBitWidth();
  return 0;
}

struct BitInsertInfo {
  ConstantInt *InsertVal;
  uint64_t Offset;
  unsigned BitWidth;
};
} // end anonymous namespace

Instruction *InstCombinerImpl::visitBitInsertInst(BitInsertInst &BI) {
  // Collect a backward chain of BitInsertInsts with constant offsets.
  SmallVector<BitInsertInfo, 8> Inserts;

  Value *Cur = &BI;
  Value *RootBase = nullptr;

  while (BitInsertInst *BII = dyn_cast<BitInsertInst>(Cur)) {
    Value *OffV = BII->getOperand(2);
    auto *OffC = dyn_cast<ConstantInt>(OffV);
    if (!OffC)
      break;

    auto *ValC = dyn_cast<ConstantInt>(BII->getOperand(1));
    unsigned ValBits = getBitWidth(ValC ? ValC->getType() : nullptr);
    if (!ValBits || !ValC)
      break;

    Inserts.push_back({ValC, OffC->getZExtValue(), ValBits});
    Cur = BII->getOperand(0);
    RootBase = Cur;
  }

  // Need at least one insert collected and a constant integer base to fold
  if (Inserts.empty())
    return nullptr;

  auto *BaseC = dyn_cast_or_null<ConstantInt>(RootBase);
  if (!BaseC)
    return nullptr;

  unsigned BaseBits = getBitWidth(RootBase->getType());
  if (!BaseBits)
    return nullptr;

  // Ensure all inserts fit within base and do not overlap. Then fold to constant.
  // We collected newest->oldest, reverse to apply oldest->newest so later
  // inserts (closer to the original instruction) overwrite earlier ones.
  std::reverse(Inserts.begin(), Inserts.end());
  APInt Result = BaseC->getValue();

  for (const BitInsertInfo &Insertion : Inserts) {
    if (Insertion.Offset + Insertion.BitWidth > BaseBits)
      return nullptr; // would produce poison

    // Off is MSB-0 (offset from most-significant bit). Convert to LSB-0
    // bit index for masking/shifting: lsbOff = BaseBits - VB - Off.
    unsigned lsbOff = BaseBits - Insertion.BitWidth -
                      static_cast<unsigned>(Insertion.Offset);

    // Mask for the field (LSB-based shift)
    APInt FieldMask = APInt::getLowBitsSet(BaseBits, Insertion.BitWidth)
                          .shl(lsbOff);

    APInt InsertVal = Insertion.InsertVal->getValue().zextOrTrunc(BaseBits);

    // Clear the field in result and insert the new bits.
    Result &= ~FieldMask;
    APInt ToOr = (InsertVal & APInt::getLowBitsSet(BaseBits, Insertion.BitWidth)).shl(lsbOff);
    Result |= ToOr;
  }

  Constant *NewC = ConstantInt::get(RootBase->getType(), Result);
  return replaceInstUsesWith(BI, NewC);
}

} // end namespace llvm