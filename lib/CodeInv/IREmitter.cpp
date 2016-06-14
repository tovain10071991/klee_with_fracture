//===--- IREmitter - Emits IR from SDnodes ----------------------*- C++ -*-===//
//
//              Fracture: The Draper Decompiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This class uses SDNodes and emits IR. It is intended to be extended by Target
// implementations who have special ISD legalization nodes.
//
// Author: Richard Carback (rtc1032) <rcarback@draper.com>
// Date: October 16, 2013
//===----------------------------------------------------------------------===//

#include "CodeInv/IREmitter.h"
#include "CodeInv/Decompiler.h"

#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/IR/Intrinsics.h"
#define GET_REGINFO_ENUM
#include "../lib/Target/X86/X86GenRegisterInfo.inc"
#define GET_INSTRINFO_ENUM
#include "../lib/Target/X86/X86GenInstrInfo.inc"

#include <err.h>
#include <string>

#include "Helper/LLDBHelper.h"

using namespace llvm;

namespace fracture {

IREmitter::IREmitter(Decompiler *TheDec, raw_ostream &InfoOut,
  raw_ostream &ErrOut) : Infos(InfoOut), Errs(ErrOut) {
  Dec = TheDec;
  IRB = new IRBuilder<>(getGlobalContext());
  initDispatcher();
}

IREmitter::~IREmitter() {
  delete IRB;
}

void IREmitter::initDispatcher()
{
  #include "MOV/MOV_initDispatcher.inc"
  #include "LEA/LEA_initDispatcher.inc"
    
  visitDispatchers[X86::PUSH64r] = &IREmitter::visitPUSH64r;
  visitDispatchers[X86::POP64r] = &IREmitter::visitPOP64r;
  visitDispatchers[X86::LEAVE64] = &IREmitter::visitLEAVE64;
  
  #include "ADD/ADD_initDispatcher.inc"
  #include "SUB/SUB_initDispatcher.inc"
  
  visitDispatchers[X86::SAR64r1] = &IREmitter::visitSAR64r1;
  visitDispatchers[X86::SAR64ri] = &IREmitter::visitSAR64ri;
  visitDispatchers[X86::SHR64ri] = &IREmitter::visitSHR64ri;
  
  visitDispatchers[X86::AND64ri8] = &IREmitter::visitAND64ri8;
  visitDispatchers[X86::AND32i32] = &IREmitter::visitAND32i32;
  visitDispatchers[X86::OR64ri8] = &IREmitter::visitOR64ri8;
  visitDispatchers[X86::XOR32rr] = &IREmitter::visitXOR32r;
  
  visitDispatchers[X86::NEG32r] = &IREmitter::visitNEG32r;
  
  visitDispatchers[X86::CMP32ri8] = &IREmitter::visitCMP32ri8;
  visitDispatchers[X86::CMP64ri8] = &IREmitter::visitCMP64ri8;
  visitDispatchers[X86::CMP64i32] = &IREmitter::visitCMP64i32;
  visitDispatchers[X86::CMP64rr] = &IREmitter::visitCMP64r;
  visitDispatchers[X86::CMP32mi8] = &IREmitter::visitCMP32mi8;
  visitDispatchers[X86::CMP64mi8] = &IREmitter::visitCMP64mi8;
  visitDispatchers[X86::CMP8mi] = &IREmitter::visitCMP8mi;
  visitDispatchers[X86::CMP64rm] = &IREmitter::visitCMP64rm;
  
  #include "TEST/TEST_initDispatcher.inc"
  
  visitDispatchers[X86::JMP64r] = &IREmitter::visitJMP64r;
  visitDispatchers[X86::JMP_1] = &IREmitter::visitJMP;
  visitDispatchers[X86::JMP64pcrel32] = &IREmitter::visitJMP;
  
  #include "Jcc/Jcc_initDispatcher.inc"
  
  visitDispatchers[X86::CALL64pcrel32] = &IREmitter::visitCALL64pcrel32;
  visitDispatchers[X86::CALL64r] = &IREmitter::visitCALL64r;
  visitDispatchers[X86::CALL64m] = &IREmitter::visitCALL64m;
  visitDispatchers[X86::RET] = &IREmitter::visitRET;
  
  visitDispatchers[X86::NOOP] = &IREmitter::visitNOOP;
  visitDispatchers[X86::NOOPL] = &IREmitter::visitNOOP;
  visitDispatchers[X86::NOOPW] = &IREmitter::visitNOOP;
  visitDispatchers[X86::REP_PREFIX] = &IREmitter::visitNOOP;
  visitDispatchers[X86::HLT] = &IREmitter::visitNOOP;
  
  visitDispatchers[X86::SYSCALL] = &IREmitter::visitSYSCALL;
  
}

void IREmitter::EmitIR(BasicBlock *BB, MachineInstr* CurInst) {
  visit(BB, CurInst);
}

StringRef IREmitter::getIndexedValueName(StringRef BaseName) {
  const ValueSymbolTable &ST = Dec->getModule()->getValueSymbolTable();

  // In the common case, the name is not already in the symbol table.
  Value *V = ST.lookup(BaseName);
  if (V == NULL) {
    return BaseName;
  }

  // Otherwise, there is a naming conflict.  Rename this value.
  // FIXME: AFAIK this is never deallocated (memory leak). It should be free'd
  // after it gets added to the symbol table (which appears to do a copy as
  // indicated by the original code that stack allocated this variable).
  SmallString<256> *UniqueName =
    new SmallString<256>(BaseName.begin(), BaseName.end());
  unsigned Size = BaseName.size();

  // Add '_' as the last character when BaseName ends in a number
  if (BaseName[Size-1] <= '9' && BaseName[Size-1] >= '0') {
    UniqueName->resize(Size+1);
    (*UniqueName)[Size] = '_';
    Size++;
  }

  unsigned LastUnique = 0;
  while (1) {
    // Trim any suffix off and append the next number.
    UniqueName->resize(Size);
    raw_svector_ostream(*UniqueName) << ++LastUnique;

    // Try insert the vmap entry with this suffix.
    V = ST.lookup(*UniqueName);
    // FIXME: ^^ this lookup does not appear to be working on non-globals...
    // Temporary Fix: check if it has a basenames entry
    if (V == NULL && BaseNames[*UniqueName].empty()) {
      BaseNames[*UniqueName] = BaseName;
      return *UniqueName;
    }
  }
}

StringRef IREmitter::getBaseValueName(StringRef BaseName) {
  // Note: An alternate approach would be to pull the Symbol table and
  // do a string search, but this is much easier to implement.
  StringRef Res = BaseNames.lookup(BaseName);
  if (Res.empty()) {
    return BaseName;
  }
  return Res;
}

StringRef IREmitter::getInstructionName(MachineInstr *I) {
  return StringRef();
}

void IREmitter::visit(BasicBlock *BB, MachineInstr* CurInst) {
  CurInst->dump();
  for(unsigned i = 0; i < CurInst->getNumOperands(); ++i)
  {
    errs() << "\n\t" << i << ": ";
    CurInst->getOperand(i).print(errs());
  }
  errs() << "\n";

  IRB->SetCurrentDebugLocation(CurInst->getDebugLoc());
  
  IRB->SetInsertPoint(BB);
  LLVMContext* context = Dec->getContext();
  ConstantInt* next_rip = ConstantInt::get(Type::getInt64Ty(*context), get_load_addr(Dec->getDisassembler()->getDebugOffset(CurInst->getDebugLoc()), Dec->getDisassembler()->getExecutable()->getFileName(), Dec->getDisassembler()->getCurrentSectionName()) + CurInst->getDesc().getSize());
  store_reg_val(X86::RIP, next_rip);
  
  assert(visitDispatchers.find(CurInst->getOpcode()) != visitDispatchers.end() && "unknown opcode when decompileBasicBlock");
  void(IREmitter::*dispatcher)(BasicBlock *, MachineInstr*) = visitDispatchers[CurInst->getOpcode()];
  (this->*dispatcher)(BB, CurInst);

  BB->dump();
}

#include "IREmitter_common.inc"

#include "MOV/MOV_define.inc"
#include "LEA/LEA_define.inc"

#include "ADD/ADD_define.inc"
#include "SUB/SUB_define.inc"

define_visit(SAR64r1)
{
  assert(I->getNumOperands()==3);
  MachineOperand& lhs_opr = I->getOperand(1);
  assert(lhs_opr.isReg());
  assert(I->getOperand(2).isReg() && I->getOperand(2).getReg()==X86::EFLAGS);
  MachineOperand& des_opr = I->getOperand(0);
  assert(des_opr.isReg() && des_opr.getReg()==lhs_opr.getReg());
  
  IRB->SetInsertPoint(BB);

  //read lhs
  Value* lhs_val = get_reg_val(lhs_opr.getReg());

  //read rhs
  Constant* rhs_val = get_imm_val(1, 64, 64);

  // compute
  Value* result = IRB->CreateAShr(lhs_val, rhs_val);

  // writeback
  store_reg_val(des_opr.getReg(), result);

  store_PF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_ZF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_SF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_CF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_OF_val(I->getOpcode(), lhs_val, rhs_val, result);
}

define_visit(SAR64ri)
{
  assert(I->getNumOperands()==4);
  MachineOperand& lhs_opr = I->getOperand(1);
  assert(lhs_opr.isReg());
  MachineOperand& rhs_opr = I->getOperand(2);
  assert(rhs_opr.isImm());
  assert(I->getOperand(3).isReg() && I->getOperand(3).getReg()==X86::EFLAGS);
  MachineOperand& des_opr = I->getOperand(0);
  assert(des_opr.isReg() && des_opr.getReg()==lhs_opr.getReg());
  
  IRB->SetInsertPoint(BB);

  //read lhs
  Value* lhs_val = get_reg_val(lhs_opr.getReg());

  //read rhs
  Constant* rhs_val = get_imm_val(rhs_opr.getImm(), 64, 64);

  // compute
  Value* result = IRB->CreateAShr(lhs_val, rhs_val);

  // writeback
  store_reg_val(des_opr.getReg(), result);

  store_PF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_ZF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_SF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_CF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_OF_val(I->getOpcode(), lhs_val, rhs_val, result);
}

define_visit(SHR64ri)
{
  assert(I->getNumOperands()==4);
  MachineOperand& lhs_opr = I->getOperand(1);
  assert(lhs_opr.isReg());
  MachineOperand& rhs_opr = I->getOperand(2);
  assert(rhs_opr.isImm());
  assert(I->getOperand(3).isReg() && I->getOperand(3).getReg()==X86::EFLAGS);
  MachineOperand& des_opr = I->getOperand(0);
  assert(des_opr.isReg() && des_opr.getReg()==lhs_opr.getReg());
  
  IRB->SetInsertPoint(BB);

  //read lhs
  Value* lhs_val = get_reg_val(lhs_opr.getReg());

  //read rhs
  Constant* rhs_val = get_imm_val(rhs_opr.getImm(), 64, 64);

  // compute
  Value* result = IRB->CreateLShr(lhs_val, rhs_val);

  // writeback
  store_reg_val(des_opr.getReg(), result);

  store_PF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_ZF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_SF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_CF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_OF_val(I->getOpcode(), lhs_val, rhs_val, result);
}

define_visit(AND64ri8)
{
  assert(I->getNumOperands()==4);
  MachineOperand& lhs_opr = I->getOperand(1);
  assert(lhs_opr.isReg());
  MachineOperand& rhs_opr = I->getOperand(2);
  assert(rhs_opr.isImm());
  assert(I->getOperand(3).isReg() && I->getOperand(3).getReg()==X86::EFLAGS);
  MachineOperand& des_opr = I->getOperand(0);
  assert(des_opr.isReg() && des_opr.getReg()==lhs_opr.getReg());
  
  IRB->SetInsertPoint(BB);
  LLVMContext* context = Dec->getContext();

  //read lhs
  Value* lhs_val = get_reg_val(lhs_opr.getReg());

  //read rhs
  Constant* rhs_val = get_imm_val(rhs_opr.getImm(), 8, 64);

  // compute
  Value* result = IRB->CreateAnd(lhs_val, rhs_val);

  // writeback
  store_reg_val(des_opr.getReg(), result);

  store_PF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_ZF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_SF_val(I->getOpcode(), lhs_val, rhs_val, result);
  IRB->CreateStore(ConstantInt::getFalse(*context), Dec->getModule()->getGlobalVariable("CF"));
  IRB->CreateStore(ConstantInt::getFalse(*context), Dec->getModule()->getGlobalVariable("OF"));
}

define_visit(AND32i32)
{
  assert(I->getNumOperands()==4);
  MachineOperand& lhs_opr = I->getOperand(3);
  assert(lhs_opr.isReg());
  MachineOperand& rhs_opr = I->getOperand(0);
  assert(rhs_opr.isImm());
  MachineOperand& des_opr = I->getOperand(1);
  assert(des_opr.isReg() && des_opr.getReg()==lhs_opr.getReg());
  assert(I->getOperand(2).isReg() && I->getOperand(2).getReg()==X86::EFLAGS);
  
  IRB->SetInsertPoint(BB);
  LLVMContext* context = Dec->getContext();

  //read lhs
  Value* lhs_val = get_reg_val(lhs_opr.getReg());

  //read rhs
  Constant* rhs_val = get_imm_val(rhs_opr.getImm(), 32, 32);

  // compute
  Value* result = IRB->CreateAnd(lhs_val, rhs_val);

  // writeback
  store_reg_val(des_opr.getReg(), result);

  store_PF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_ZF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_SF_val(I->getOpcode(), lhs_val, rhs_val, result);
  IRB->CreateStore(ConstantInt::getFalse(*context), Dec->getModule()->getGlobalVariable("CF"));
  IRB->CreateStore(ConstantInt::getFalse(*context), Dec->getModule()->getGlobalVariable("OF"));
}

define_visit(OR64ri8)
{
  assert(I->getNumOperands()==4);
  MachineOperand& lhs_opr = I->getOperand(1);
  assert(lhs_opr.isReg());
  MachineOperand& rhs_opr = I->getOperand(2);
  assert(rhs_opr.isImm());
  assert(I->getOperand(3).isReg() && I->getOperand(3).getReg()==X86::EFLAGS);
  MachineOperand& des_opr = I->getOperand(0);
  assert(des_opr.isReg() && des_opr.getReg()==lhs_opr.getReg());
  
  IRB->SetInsertPoint(BB);
  LLVMContext* context = Dec->getContext();

  //read lhs
  Value* lhs_val = get_reg_val(lhs_opr.getReg());

  //read rhs
  Constant* rhs_val = get_imm_val(rhs_opr.getImm(), 8, 64);

  // compute
  Value* result = IRB->CreateOr(lhs_val, rhs_val);

  // writeback
  store_reg_val(des_opr.getReg(), result);

  store_PF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_ZF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_SF_val(I->getOpcode(), lhs_val, rhs_val, result);
  IRB->CreateStore(ConstantInt::getFalse(*context), Dec->getModule()->getGlobalVariable("CF"));
  IRB->CreateStore(ConstantInt::getFalse(*context), Dec->getModule()->getGlobalVariable("OF"));
}

define_visit(XOR32r)
{
  assert(I->getNumOperands()==4);
  MachineOperand& lhs_opr = I->getOperand(1);
  assert(lhs_opr.isReg());
  MachineOperand& rhs_opr = I->getOperand(2);
  assert(I->getOperand(3).isReg() && I->getOperand(3).getReg()==X86::EFLAGS);
  MachineOperand& des_opr = I->getOperand(0);
  assert(des_opr.isReg() && des_opr.getReg()==lhs_opr.getReg());
  
  IRB->SetInsertPoint(BB);
  LLVMContext* context = Dec->getContext();

  //read lhs
  Value* lhs_val = get_reg_val(lhs_opr.getReg());

  //read rhs
  //read rhs
  Value* rhs_val;
  if(rhs_opr.isImm())
  {
    rhs_val = get_imm_val(rhs_opr.getImm(), 32, 32);
  }
  else
  {
    rhs_val = get_reg_val(rhs_opr.getReg());
  }

  // compute
  Value* result = IRB->CreateXor(lhs_val, rhs_val);

  // writeback
  store_reg_val(des_opr.getReg(), result);

  store_PF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_ZF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_SF_val(I->getOpcode(), lhs_val, rhs_val, result);
  IRB->CreateStore(ConstantInt::getFalse(*context), Dec->getModule()->getGlobalVariable("CF"));
  IRB->CreateStore(ConstantInt::getFalse(*context), Dec->getModule()->getGlobalVariable("OF"));
}

define_visit(NEG32r)
{
  assert(I->getNumOperands()==3);
  MachineOperand& src_opr = I->getOperand(1);
  assert(src_opr.isReg());
  MachineOperand& des_opr = I->getOperand(0);
  assert(des_opr.isReg() && des_opr.getReg()==src_opr.getReg());
  assert(I->getOperand(2).isReg() && I->getOperand(2).getReg()==X86::EFLAGS);
  
  IRB->SetInsertPoint(BB);

  //read src
  Value* src_val = get_reg_val(src_opr.getReg());

  // compute
  Value* result = IRB->CreateNeg(src_val);

  // writeback
  store_reg_val(des_opr.getReg(), result);

  // set CF
  IRB->CreateStore(IRB->CreateICmpNE(src_val, ConstantInt::get(src_val->getType(), 0)), Dec->getModule()->getGlobalVariable("CF"));
}

define_visit(CMP32ri8)
{
  assert(I->getNumOperands()==3);
  MachineOperand& lhs_opr = I->getOperand(0);
  assert(lhs_opr.isReg());
  MachineOperand& rhs_opr = I->getOperand(1);
  assert(rhs_opr.isImm());
  assert(I->getOperand(2).isReg() && I->getOperand(2).getReg()==X86::EFLAGS);
  
  IRB->SetInsertPoint(BB);

  // read lhs
  Value* lhs_val = get_reg_val(lhs_opr.getReg());

  //read rhs
  Value* rhs_val = get_imm_val(rhs_opr.getImm(), 8, 32);

  // compute
  Value* result = IRB->CreateSub(lhs_val, rhs_val);

  store_AF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_PF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_ZF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_SF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_CF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_OF_val(I->getOpcode(), lhs_val, rhs_val, result);
}

define_visit(CMP64ri8)
{
  assert(I->getNumOperands()==3);
  MachineOperand& lhs_opr = I->getOperand(0);
  assert(lhs_opr.isReg());
  MachineOperand& rhs_opr = I->getOperand(1);
  assert(rhs_opr.isImm());
  assert(I->getOperand(2).isReg() && I->getOperand(2).getReg()==X86::EFLAGS);
  
  IRB->SetInsertPoint(BB);

  // read lhs
  Value* lhs_val = get_reg_val(lhs_opr.getReg());

  //read rhs
  Value* rhs_val = get_imm_val(rhs_opr.getImm(), 8, 64);

  // compute
  Value* result = IRB->CreateSub(lhs_val, rhs_val);

  store_AF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_PF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_ZF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_SF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_CF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_OF_val(I->getOpcode(), lhs_val, rhs_val, result);
}

define_visit(CMP64i32)
{
  assert(I->getNumOperands()==4);
  MachineOperand& lhs_opr = I->getOperand(3);
  assert(lhs_opr.isReg());
  assert(I->getOperand(1).isReg() && I->getOperand(1).getReg()==lhs_opr.getReg());  // is defed reg, why?
  MachineOperand& rhs_opr = I->getOperand(0);
  assert(rhs_opr.isImm());
  assert(I->getOperand(2).isReg() && I->getOperand(2).getReg()==X86::EFLAGS);
  
  IRB->SetInsertPoint(BB);

  // read lhs
  Value* lhs_val = get_reg_val(lhs_opr.getReg());

  //read rhs
  Value* rhs_val = get_imm_val(rhs_opr.getImm(), 32, 64);

  // compute
  Value* result = IRB->CreateSub(lhs_val, rhs_val);

  store_AF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_PF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_ZF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_SF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_CF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_OF_val(I->getOpcode(), lhs_val, rhs_val, result);
}

define_visit(CMP64r)
{
  assert(I->getNumOperands()==3);
  MachineOperand& lhs_opr = I->getOperand(0);
  assert(lhs_opr.isReg());
  MachineOperand& rhs_opr = I->getOperand(1);
  assert(I->getOperand(2).isReg() && I->getOperand(2).getReg()==X86::EFLAGS);
  
  IRB->SetInsertPoint(BB);

  // read lhs
  Value* lhs_val = get_reg_val(lhs_opr.getReg());

  //read rhs
  Value* rhs_val;
  if(rhs_opr.isImm())
  {
    rhs_val = get_imm_val(rhs_opr.getImm(), 64, 64);
  }
  else
  {
    rhs_val = get_reg_val(rhs_opr.getReg());
  }

  // compute
  Value* result = IRB->CreateSub(lhs_val, rhs_val);

  store_AF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_PF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_ZF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_SF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_CF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_OF_val(I->getOpcode(), lhs_val, rhs_val, result);
}

define_visit(CMP32mi8)
{
  assert(I->getNumOperands()==7 && "CMP32mi8's opr's num is not 7");
  MachineOperand& base_opr = I->getOperand(0);
  assert(base_opr.isReg() && "opr 0(base) is not reg in IREmitter::visitCMP32mi8");
  MachineOperand& scale_opr = I->getOperand(1);
  assert(scale_opr.isImm() && "opr 1(scale) is not imm in IREmitter::visitCMP32mi8");
  MachineOperand& idx_opr = I->getOperand(2);
  assert(idx_opr.isReg() && "opr 2(idx) is not reg in IREmitter::visitCMP32mi8");
  MachineOperand& off_opr = I->getOperand(3);
  assert(off_opr.isImm() && "opr 3(off) is not imm in IREmitter::visitCMP32mi8");
  MachineOperand& seg_opr = I->getOperand(4);
  assert(seg_opr.isReg());
  MachineOperand& rhs_opr = I->getOperand(5);
  assert(I->getOperand(6).isReg() && I->getOperand(6).getReg()==X86::EFLAGS && "opr 2(efalgs) is not eflags in IREmitter::visitCMP32mi8");

  IRB->SetInsertPoint(BB);

  // read lhs
  Value* lhs_val = get_mem_val(BB, base_opr.getReg(), scale_opr.getImm(), idx_opr.getReg(), off_opr.getImm(), seg_opr.getReg(), 32);

  //read rhs
  Value* rhs_val = get_imm_val(rhs_opr.getImm(), 8, 32);

  // compute
  Value* result = IRB->CreateSub(lhs_val, rhs_val);

  store_AF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_PF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_ZF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_SF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_CF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_OF_val(I->getOpcode(), lhs_val, rhs_val, result);
}

define_visit(CMP64mi8)
{
  assert(I->getNumOperands()==7);
  MachineOperand& base_opr = I->getOperand(0);
  assert(base_opr.isReg());
  MachineOperand& scale_opr = I->getOperand(1);
  assert(scale_opr.isImm());
  MachineOperand& idx_opr = I->getOperand(2);
  assert(idx_opr.isReg());
  MachineOperand& off_opr = I->getOperand(3);
  assert(off_opr.isImm());
  MachineOperand& seg_opr = I->getOperand(4);
  assert(seg_opr.isReg());
  MachineOperand& rhs_opr = I->getOperand(5);
  assert(I->getOperand(6).isReg() && I->getOperand(6).getReg()==X86::EFLAGS);

  IRB->SetInsertPoint(BB);

  // read lhs
  Value* lhs_val = get_mem_val(BB, base_opr.getReg(), scale_opr.getImm(), idx_opr.getReg(), off_opr.getImm(), seg_opr.getReg(), 64);

  //read rhs
  Value* rhs_val = get_imm_val(rhs_opr.getImm(), 8, 64);

  // compute
  Value* result = IRB->CreateSub(lhs_val, rhs_val);

  store_AF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_PF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_ZF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_SF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_CF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_OF_val(I->getOpcode(), lhs_val, rhs_val, result);
}

define_visit(CMP8mi)
{
  assert(I->getNumOperands()==7);
  MachineOperand& base_opr = I->getOperand(0);
  assert(base_opr.isReg());
  MachineOperand& scale_opr = I->getOperand(1);
  assert(scale_opr.isImm());
  MachineOperand& idx_opr = I->getOperand(2);
  assert(idx_opr.isReg());
  MachineOperand& off_opr = I->getOperand(3);
  assert(off_opr.isImm());
  MachineOperand& seg_opr = I->getOperand(4);
  assert(seg_opr.isReg());
  MachineOperand& rhs_opr = I->getOperand(5);
  assert(I->getOperand(6).isReg() && I->getOperand(6).getReg()==X86::EFLAGS);

  IRB->SetInsertPoint(BB);

  // read lhs
  Value* lhs_val = get_mem_val(BB, base_opr.getReg(), scale_opr.getImm(), idx_opr.getReg(), off_opr.getImm(), seg_opr.getReg(), 8);

  //read rhs
  Value* rhs_val = get_imm_val(rhs_opr.getImm(), 8, 8);

  // compute
  Value* result = IRB->CreateSub(lhs_val, rhs_val);

  store_AF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_PF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_ZF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_SF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_CF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_OF_val(I->getOpcode(), lhs_val, rhs_val, result);
}

define_visit(CMP64rm)
{
  assert(I->getNumOperands()==7);
  MachineOperand& lhs_opr = I->getOperand(0);
  assert(lhs_opr.isReg());
  MachineOperand& base_opr = I->getOperand(1);
  assert(base_opr.isReg());
  MachineOperand& scale_opr = I->getOperand(2);
  assert(scale_opr.isImm());
  MachineOperand& idx_opr = I->getOperand(3);
  assert(idx_opr.isReg());
  MachineOperand& off_opr = I->getOperand(4);
  assert(off_opr.isImm());
  MachineOperand& seg_opr = I->getOperand(5);
  assert(seg_opr.isReg());
  assert(I->getOperand(6).isReg() && I->getOperand(6).getReg()==X86::EFLAGS);

  IRB->SetInsertPoint(BB);

  // read lhs
  Value* lhs_val = get_reg_val(lhs_opr.getReg());

  //read rhs
  Value* rhs_val = get_mem_val(BB, base_opr.getReg(), scale_opr.getImm(), idx_opr.getReg(), off_opr.getImm(), seg_opr.getReg(), 64);

  // compute
  Value* result = IRB->CreateSub(lhs_val, rhs_val);

  store_AF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_PF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_ZF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_SF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_CF_val(I->getOpcode(), lhs_val, rhs_val, result);
  store_OF_val(I->getOpcode(), lhs_val, rhs_val, result);
}

#include "TEST/TEST_define.inc"
#include "Jcc/Jcc_define.inc"

define_visit(PUSH64r)
{
  assert(I->getNumOperands()==3 && "PUSH64r's opr's num is not 3");
  MachineOperand& src_opr = I->getOperand(0);

  IRB->SetInsertPoint(BB);

  // rsp = rsp -8
  Value* lhs_val = get_reg_val(X86::RSP);
  Constant* rhs_val = get_imm_val(8, 64, 64);
  store_reg_val(X86::RSP, IRB->CreateSub(lhs_val, rhs_val));

  // mov src, (%rsp)
  // read src val
  Value* src_val;
  if(src_opr.isImm())
  {
    src_val = get_imm_val(src_opr.getImm(), 64, 64);
  }
  else
  {
    src_val = get_reg_val(src_opr.getReg());
  }

  // compute
  Value* result = src_val;

  store_mem_val(BB, X86::RSP, 0, X86::NoRegister, 0, X86::NoRegister, result);
}

define_visit(POP64r)
{
  assert(I->getNumOperands()==3 && "POP64r's opr's num is not 3");
  MachineOperand& des_opr = I->getOperand(0);
  assert(des_opr.isReg() && "opr 0(des reg) is not reg in IREmitter::visitPOP64r");

  IRB->SetInsertPoint(BB);

  // mov (%rsp), des
  // read src
  Value* src_val = get_mem_val(BB, X86::RSP, 0, X86::NoRegister, 0, X86::NoRegister, 64);

  // compute
  Value* result = src_val;

  // mask and store result
  store_reg_val(des_opr.getReg(), result);

  // rsp = rsp + 8
  Value* lhs_val = get_reg_val(X86::RSP);
  Constant* rhs_val = get_imm_val(8, 64, 64);
  store_reg_val(X86::RSP, IRB->CreateAdd(lhs_val, rhs_val));
}

define_visit(LEAVE64)
{
  assert(I->getNumOperands()==4 && "LEAVE64's opr's num is not 4");
  assert(I->getOperand(0).isReg() && I->getOperand(0).getReg()==X86::RBP && "opr 0(rbp reg) is not rbp reg in IREmitter::visitLEAVE64");
  assert(I->getOperand(1).isReg() && I->getOperand(1).getReg()==X86::RSP && "opr 1(rsp reg) is not rsp reg in IREmitter::visitLEAVE64");
  assert(I->getOperand(2).isReg() && I->getOperand(2).getReg()==X86::RBP && "opr 2(rbp reg) is not rbp reg in IREmitter::visitLEAVE64");
  assert(I->getOperand(3).isReg() && I->getOperand(3).getReg()==X86::RSP && "opr 3(rsp reg) is not rsp reg in IREmitter::visitLEAVE64");

  IRB->SetInsertPoint(BB);

  // mov %rbp, %rsp
  store_reg_val(X86::RSP, get_reg_val(X86::RBP));

  // mov (%rsp), %rbp
  store_reg_val(X86::RBP, get_mem_val(BB, X86::RSP, 0, X86::NoRegister, 0, X86::NoRegister, 64));

  // rsp = rsp + 8
  Value* lhs_val = get_reg_val(X86::RSP);
  Constant* rhs_val = get_imm_val(8, 64, 64);
  store_reg_val(X86::RSP, IRB->CreateAdd(lhs_val, rhs_val));
}

define_visit(JMP64r)
{
  assert(I->getNumOperands()==1);
  MachineOperand& target_opr = I->getOperand(0);
  assert(target_opr.isReg());

  IRB->SetInsertPoint(BB);

  // jmp target
  IRB->CreateUnreachable();
}

define_visit(JMP)
{
  assert(I->getNumOperands()==1);
  MachineOperand& off_opr = I->getOperand(0);
  assert(off_opr.isImm());

  IRB->SetInsertPoint(BB);

  int64_t off = off_opr.getImm();

  std::stringstream bb_name;
  bb_name << "bb_" << (Dec->getDisassembler()->getDebugOffset(I->getDebugLoc())+ I->getDesc().getSize() + off);
  BasicBlock* bb  = Dec->getOrCreateBasicBlock(bb_name.str(), BB->getParent());
  IRB->CreateBr(bb);
}

define_visit(CALL64pcrel32)
{
  assert(I->getNumOperands()==2);
  MachineOperand& off_opr = I->getOperand(0);
  assert(off_opr.isImm());
  assert(I->getOperand(1).isReg() && I->getOperand(1).getReg()==X86::RSP);

  IRB->SetInsertPoint(BB);
  LLVMContext* context = Dec->getContext();  

 // rsp = rsp -8
  Value* lhs_val = get_reg_val(X86::RSP);
  Constant* rhs_val = get_imm_val(8, 64, 64);
  store_reg_val(X86::RSP, IRB->CreateSub(lhs_val, rhs_val));

  // mov %rip, (%rsp)
  // read %rip val
  Value* src_val = get_reg_val(X86::RIP);

  // compute
  Value* result = src_val;

  store_mem_val(BB, X86::RSP, 0, X86::NoRegister, 0, X86::NoRegister, result);

  // jmp target
  int64_t off = off_opr.getImm();
  int64_t target = get_load_addr(Dec->getDisassembler()->getDebugOffset(I->getDebugLoc()), Dec->getDisassembler()->getExecutable()->getFileName(), Dec->getDisassembler()->getCurrentSectionName()) + I->getDesc().getSize() + off;
  Function* target_func = Dec->getFunctionByAddr(target);
  if(target_func)
    IRB->CreateCall(target_func);
  else
  {
    const object::SectionRef sec = Dec->getDisassembler()->getSectionByAddress(target);
    StringRef sec_name;
    if(error_code ec = sec.getName(sec_name))
      errx(-1, "get sec's name failed: %s", ec.message().c_str());
    if(sec_name.equals(".plt"))
    {
      std::string func_name = get_func_name_in_plt(target);
      target_func = dyn_cast<Function>(Dec->getModule()->getOrInsertFunction(func_name, FunctionType::get(Type::getVoidTy(*context), false)));
      IRB->CreateCall(target_func);
    }
    else
      IRB->CreateUnreachable();    
  }
}

define_visit(CALL64r)
{
  assert(I->getNumOperands()==2 && "CALL64r's opr's num is not 2");
  MachineOperand& target_opr = I->getOperand(0);
  assert(target_opr.isReg() && "opr 0(target) is not Reg in IREmitter::visitCALL64r");
  assert(I->getOperand(1).isReg() && I->getOperand(1).getReg()==X86::RSP && "opr 1(rsp) is not rsp in IREmitter::visitCALL64r");

  IRB->SetInsertPoint(BB);

 // rsp = rsp -8
  Value* lhs_val = get_reg_val(X86::RSP);
  Constant* rhs_val = get_imm_val(8, 64, 64);
  store_reg_val(X86::RSP, IRB->CreateSub(lhs_val, rhs_val));

  // mov %rip, (%rsp)
  // read %rip val
  Value* src_val = get_reg_val(X86::RIP);

  // compute
  Value* result = src_val;

  store_mem_val(BB, X86::RSP, 0, X86::NoRegister, 0, X86::NoRegister, result);

 // jmp target
  Value* target_val = get_reg_val(target_opr.getReg());
  IRB->CreateCall(BB->getParent()->getParent()->getFunction("saib_collect_indirect"), target_val);
}

define_visit(CALL64m)
{
  assert(I->getNumOperands()==6);
  MachineOperand& base_opr = I->getOperand(0);
  assert(base_opr.isReg());
  MachineOperand& scale_opr = I->getOperand(1);
  assert(scale_opr.isImm());
  MachineOperand& idx_opr = I->getOperand(2);
  assert(idx_opr.isReg());
  MachineOperand& off_opr = I->getOperand(3);
  assert(off_opr.isImm());
  MachineOperand& seg_opr = I->getOperand(4);
  assert(seg_opr.isReg());
  assert(I->getOperand(5).isReg() && I->getOperand(5).getReg()==X86::RSP);

  IRB->SetInsertPoint(BB);

 // rsp = rsp -8
  Value* lhs_val = get_reg_val(X86::RSP);
  Constant* rhs_val = get_imm_val(8, 64, 64);
  store_reg_val(X86::RSP, IRB->CreateSub(lhs_val, rhs_val));

  // mov %rip, (%rsp)
  // read %rip val
  Value* src_val = get_reg_val(X86::RIP);

  // compute
  Value* result = src_val;

  store_mem_val(BB, X86::RSP, 0, X86::NoRegister, 0, X86::NoRegister, result);

 // jmp target
//  Value* target_val = get_reg_val(target_opr.getReg());

  IRB->CreateUnreachable();
}

define_visit(RET)
{
  assert(I->getNumOperands()==0 && "RETQ's opr's num is not 0");

  IRB->SetInsertPoint(BB);

  // pop rip
  // mov (%rsp), rip
  // read src
  Value* src_val = get_mem_val(BB, X86::RSP, 0, X86::NoRegister, 0, X86::NoRegister, 64);

  // compute
  Value* result = src_val;

  // mask and store result
  store_reg_val(X86::RIP, result);

  // rsp = rsp + 8
  Value* lhs_val = get_reg_val(X86::RSP);
  Constant* rhs_val = get_imm_val(8, 64, 64);
  store_reg_val(X86::RSP, IRB->CreateAdd(lhs_val, rhs_val));

  // jmp target
  IRB->CreateRetVoid();
}

define_visit(NOOP)
{
  // IRB->SetInsertPoint(BB);
  // LLVMContext* context = Dec->getContext();

  // IRB->CreateCall(Intrinsic::getDeclaration(Dec->getModule(), Intrinsic::donothing, {Type::getVoidTy(*context)}));
}

define_visit(SYSCALL)
{
  IRB->SetInsertPoint(BB);

  Value* sys_num = get_reg_val(X86::RAX);
  Value* arg1 = get_reg_val(X86::RDI);
  Value* arg2 = get_reg_val(X86::RSI);
  Value* arg3 = get_reg_val(X86::RDX);
  Value* arg4 = get_reg_val(X86::R10);
  Value* arg5 = get_reg_val(X86::R8);
  Value* arg6 = get_reg_val(X86::R9);
  std::vector<Value*> args = {sys_num, arg1, arg2, arg3, arg4, arg5, arg6};

  Value* result = IRB->CreateCall(BB->getParent()->getParent()->getFunction("saib_syscall"), args);
  store_reg_val(X86::RAX, result);
}

} // End namespace fracture
