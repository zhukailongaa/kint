#define DEBUG_TYPE "ranges"
#include <llvm/Pass.h>
#include <llvm/Instructions.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/InstIterator.h>
#include <llvm/Module.h>
#include <llvm/Constants.h>
#include <llvm/ADT/OwningPtr.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/DebugInfo.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include "llvm/Support/CommandLine.h"

#include "Annotation.h"
#include "IntGlobal.h"

using namespace llvm;

static cl::opt<std::string>
WatchID("w", cl::desc("Watch sID"), 
			   cl::value_desc("sID"));


void conv_and_warn_if_unmatch(ConstantRange &V1, ConstantRange &V2)
{
	if (V1.getBitWidth() != V2.getBitWidth()) {
		dbgs() << "warning: range " << V1 << " " << V1.getBitWidth()
			<< " and " << V2 << " " << V2.getBitWidth() << " unmatch\n";
		V2 = V2.zextOrTrunc(V1.getBitWidth());
	}
}

bool RangePass::safeUnion(ConstantRange &CR, const ConstantRange &R)
{
	ConstantRange V = R, Old = CR;
	conv_and_warn_if_unmatch(CR, V);
	CR = CR.unionWith(V);
	return Old != CR;
}

bool RangePass::unionRange(StringRef sID, const ConstantRange &R,
						   Value *V = NULL)
{
	if (R.isEmptySet())
		return false;
	
	if (WatchID == sID && V) {
		if (Instruction *I = dyn_cast<Instruction>(V))
			dbgs() << I->getParent()->getParent()->getName() << "(): ";
		V->print(dbgs());
		dbgs() << "\n";
	}
	
	bool changed = true;
	RangeMap::iterator it = Ctx->IntRanges.find(sID);
	if (it != Ctx->IntRanges.end()) {
		changed = safeUnion(it->second, R);
		if (changed && sID == WatchID)
			dbgs() << sID << " + " << R << " = " << it->second << "\n";
	} else {
		Ctx->IntRanges.insert(std::make_pair(sID, R));
		if (sID == WatchID)
			dbgs() << sID << " = " << R << "\n";
	}
	if (changed)
		Changes.insert(sID);
	return changed;
}

bool RangePass::unionRange(BasicBlock *BB, Value *V,
						   const ConstantRange &R)
{
	if (R.isEmptySet())
		return false;
	
	bool changed = true;
	ValueRangeMap &VRM = FuncVRMs[BB];
	ValueRangeMap::iterator it = VRM.find(V);
	if (it != VRM.end())
		changed = safeUnion(it->second, R);
	else
		VRM.insert(std::make_pair(V, R));
	return changed;
}

ConstantRange RangePass::getRange(BasicBlock *BB, Value *V)
{
	// constants
	if (ConstantInt *C = dyn_cast<ConstantInt>(V))
		return ConstantRange(C->getValue());
	
	ValueRangeMap &VRM = FuncVRMs[BB];
	ValueRangeMap::iterator invrm = VRM.find(V);
	
	if (invrm != VRM.end())
		return invrm->second;
	
	// V must be integer or pointer to integer
	IntegerType *Ty = dyn_cast<IntegerType>(V->getType());
	if (PointerType *PTy = dyn_cast<PointerType>(V->getType()))
		Ty = dyn_cast<IntegerType>(PTy->getElementType());
	assert(Ty != NULL);
	
	// not found in VRM, lookup global range, return empty set by default
	ConstantRange CR(Ty->getBitWidth(), false);
	ConstantRange Fullset(Ty->getBitWidth(), true);
	
	RangeMap &IRM = Ctx->IntRanges;
	TaintPass TI(Ctx);
	
	if (CallInst *CI = dyn_cast<CallInst>(V)) {
		// calculate union of values ranges returned by all possible callees
		if (!CI->isInlineAsm() && Ctx->Callees.count(CI)) {
			FuncSet &CEEs = Ctx->Callees[CI];
			for (FuncSet::iterator i = CEEs.begin(), e = CEEs.end();
				 i != e; ++i) {
				std::string sID = getRetId(*i);
				if (sID != "" && TI.isTaintSource(sID)) {
					CR = Fullset;
					break;
				}
				RangeMap::iterator it;
				if ((it = IRM.find(sID)) != IRM.end())
					safeUnion(CR, it->second);
			}
		}
	} else {
		// arguments & loads
		std::string sID = getValueId(V);
		if (sID != "") {
			RangeMap::iterator it;
			if (TI.isTaintSource(sID))
				CR = Fullset;
			else if ((it = IRM.find(sID)) != IRM.end())
				CR = it->second;
		}
		// might load part of a struct field
		CR = CR.zextOrTrunc(Ty->getBitWidth());
	}
	if (!CR.isEmptySet())
		VRM.insert(std::make_pair(V, CR));
	return CR;
}

void RangePass::collectInitializers(GlobalVariable *GV, Constant *I)
{	
	// global var
	if (ConstantInt *CI = dyn_cast<ConstantInt>(I)) {
		unionRange(getVarId(GV), CI->getValue(), GV);
	}
	
	// structs
	if (ConstantStruct *CS = dyn_cast<ConstantStruct>(I)) {
		// Find integer fields in the struct
		StructType *ST = CS->getType();
		// Skip anonymous structs
		if (!ST->hasName() || ST->getName() == "struct.anon" 
			|| ST->getName().startswith("struct.anon."))
			return;
		
		for (unsigned i = 0; i != ST->getNumElements(); ++i) {
			Type *Ty = ST->getElementType(i);
			if (Ty->isStructTy()) {
				// nested struct
				// TODO: handle nested arrays
				collectInitializers(GV, CS->getOperand(i));
			} else if (Ty->isIntegerTy()) {
				ConstantInt *CI = 
					dyn_cast<ConstantInt>(I->getOperand(i));
				StringRef sID = getStructId(ST, GV->getParent(), i);
				if (!sID.empty() && CI)
					unionRange(sID, CI->getValue(), GV);
			}
		}
	}

	// arrays
	if (ConstantArray *CA = dyn_cast<ConstantArray>(I)) {
		Type *Ty = CA->getType()->getElementType();
		if (Ty->isStructTy() || Ty->isIntegerTy()) {
			for (unsigned i = 0; i != CA->getNumOperands(); ++i)
				collectInitializers(GV, CA->getOperand(i));
		}
	}
}

//
// Handle integer assignments in global initializers
//
bool RangePass::doInitialization(Module *M)
{	
	// Looking for global variables
	for (Module::global_iterator i = M->global_begin(), 
		 e = M->global_end(); i != e; ++i) {

		// skip strings literals
		if (i->hasInitializer() && !i->getName().startswith("."))
			collectInitializers(&*i, i->getInitializer());
	}
	return true;
}


ConstantRange RangePass::visitBinaryOp(BinaryOperator *BO)
{
	ConstantRange L = getRange(BO->getParent(), BO->getOperand(0));
	ConstantRange R = getRange(BO->getParent(), BO->getOperand(1));
	conv_and_warn_if_unmatch(L, R);
	switch (BO->getOpcode()) {
		default: BO->dump(); llvm_unreachable("Unknown binary operator!");
		case Instruction::Add:  return L.add(R);
		case Instruction::Sub:  return L.sub(R);
		case Instruction::Mul:  return L.multiply(R);
		case Instruction::UDiv: return L.udiv(R);
		case Instruction::SDiv: return L; // FIXME
		case Instruction::URem: return R; // FIXME
		case Instruction::SRem: return R; // FIXME
		case Instruction::Shl:  return L.shl(R);
		case Instruction::LShr: return L.lshr(R);
		case Instruction::AShr: return L; // FIXME
		case Instruction::And:  return L.binaryAnd(R);
		case Instruction::Or:   return L.binaryOr(R);
		case Instruction::Xor:  return L; // FIXME
	}
}


ConstantRange RangePass::visitCastInst(CastInst *CI)
{	
	unsigned bits = dyn_cast<IntegerType>(
								CI->getDestTy())->getBitWidth();
	
	// pointer to int could be any value
	if (CI->getOpcode() == CastInst::PtrToInt)
		return ConstantRange(bits, true);
	
	ConstantRange CR = getRange(CI->getParent(), CI->getOperand(0));
	switch (CI->getOpcode()) {
		default: CI->dump(); llvm_unreachable("unknown cast inst");
		case CastInst::Trunc:    return CR.zextOrTrunc(bits);
		case CastInst::ZExt:     return CR.zextOrTrunc(bits);
		case CastInst::SExt:     return CR.signExtend(bits);
		case CastInst::BitCast:  return CR;
	}
}

ConstantRange RangePass::visitSelectInst(SelectInst *SI)
{
	ConstantRange T = getRange(SI->getParent(), SI->getTrueValue());
	ConstantRange F = getRange(SI->getParent(), SI->getFalseValue());
	safeUnion(T, F);
	return T;
}

ConstantRange RangePass::visitPHINode(PHINode *PHI)
{
	IntegerType *Ty = dyn_cast<IntegerType>(PHI->getType());
	assert(Ty);
	ConstantRange CR(Ty->getBitWidth(), false);
	
	for (unsigned i = 0, n = PHI->getNumIncomingValues(); i < n; ++i) {
		BasicBlock *Pred = PHI->getIncomingBlock(i);
		// skip back edges
		if (isBackEdge(Edge(Pred, PHI->getParent())))
			continue;
		safeUnion(CR, getRange(Pred, PHI->getIncomingValue(i)));
	}
	return CR;
}

bool RangePass::visitCallInst(CallInst *CI)
{
	bool changed = false;
	if (CI->isInlineAsm() || Ctx->Callees.count(CI) == 0)
		return false;

	// update arguments of all possible callees
	FuncSet &CEEs = Ctx->Callees[CI];
	for (FuncSet::iterator i = CEEs.begin(), e = CEEs.end(); i != e; ++i) {
		// skip vaarg and builtin functions
		if ((*i)->isVarArg() 
			|| (*i)->getName().find('.') != StringRef::npos)
			continue;
		
		for (unsigned j = 0; j < CI->getNumArgOperands(); ++j) {
			Value *V = CI->getArgOperand(j);
			// skip non-integer arguments
			if (!V->getType()->isIntegerTy())
				continue;
			std::string sID = getArgId(*i, j);
			changed |= unionRange(sID, getRange(CI->getParent(), V), CI);
		}
	}
	// range for the return value of this call site
	if (CI->getType()->isIntegerTy())
		changed |= unionRange(getRetId(CI), getRange(CI->getParent(), CI), CI);
	return changed;
}

bool RangePass::visitStoreInst(StoreInst *SI)
{
	std::string sID = getValueId(SI);
	Value *V = SI->getValueOperand();
	if (V->getType()->isIntegerTy() && sID != "") {
		ConstantRange CR = getRange(SI->getParent(), V);
		unionRange(SI->getParent(), SI->getPointerOperand(), CR);
		return unionRange(sID, CR, SI);
	}
	return false;
}

bool RangePass::visitReturnInst(ReturnInst *RI)
{
	Value *V = RI->getReturnValue();
	if (!V || !V->getType()->isIntegerTy())
		return false;
	
	std::string sID = getRetId(RI->getParent()->getParent());
	return unionRange(sID, getRange(RI->getParent(), V), RI);
}

bool RangePass::updateRangeFor(Instruction *I)
{
	bool changed = false;
	
	// store, return and call might update global range
	if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
		changed |= visitStoreInst(SI);
	} else if (ReturnInst *RI = dyn_cast<ReturnInst>(I)) {
		changed |= visitReturnInst(RI);
	} else if (CallInst *CI = dyn_cast<CallInst>(I)) {
		changed |= visitCallInst(CI);
	}
	
	IntegerType *Ty = dyn_cast<IntegerType>(I->getType());
	if (!Ty)
		return changed;
	
	ConstantRange CR(Ty->getBitWidth(), true);
	if (BinaryOperator *BO = dyn_cast<BinaryOperator>(I)) {
		CR = visitBinaryOp(BO);
	} else if (CastInst *CI = dyn_cast<CastInst>(I)) {
		CR = visitCastInst(CI);
	} else if (SelectInst *SI = dyn_cast<SelectInst>(I)) {
		CR = visitSelectInst(SI);
	} else if (PHINode *PHI = dyn_cast<PHINode>(I)) {
		CR = visitPHINode(PHI);
	} else if (LoadInst *LI = dyn_cast<LoadInst>(I)) {
		CR = getRange(LI->getParent(), LI);
	} else if (CallInst *CI = dyn_cast<CallInst>(I)) {
		CR = getRange(CI->getParent(), CI);
	}
	unionRange(I->getParent(), I, CR);
	
	return changed;
}

bool RangePass::isBackEdge(const Edge &E)
{
	return std::find(BackEdges.begin(), BackEdges.end(), E)	!= BackEdges.end();
}

void RangePass::visitBranchInst(BranchInst *BI, BasicBlock *BB, 
								ValueRangeMap &VRM)
{
	if (!BI->isConditional())
		return;
	
	ICmpInst *ICI = dyn_cast<ICmpInst>(BI->getCondition());
	if (ICI == NULL)
		return;
	
	Value *LHS = ICI->getOperand(0);
	Value *RHS = ICI->getOperand(1);
	
	if (!LHS->getType()->isIntegerTy() || !RHS->getType()->isIntegerTy())
		return;
	
	ConstantRange LCR = getRange(ICI->getParent(), LHS);
	ConstantRange RCR = getRange(ICI->getParent(), RHS);
	conv_and_warn_if_unmatch(LCR, RCR);

	if (BI->getSuccessor(0) == BB) {
		// true target
		ConstantRange PLCR = ConstantRange::makeICmpRegion(
									ICI->getSwappedPredicate(), LCR);
		ConstantRange PRCR = ConstantRange::makeICmpRegion(
									ICI->getPredicate(), RCR);
		VRM.insert(std::make_pair(LHS, LCR.intersectWith(PRCR)));
		VRM.insert(std::make_pair(RHS, LCR.intersectWith(PLCR)));
	} else {
		// false target, use inverse predicate
		// N.B. why there's no getSwappedInversePredicate()...
		ICI->swapOperands();
		ConstantRange PLCR = ConstantRange::makeICmpRegion(
									ICI->getInversePredicate(), RCR);
		ICI->swapOperands();
		ConstantRange PRCR = ConstantRange::makeICmpRegion(
									ICI->getInversePredicate(), RCR);
		VRM.insert(std::make_pair(LHS, LCR.intersectWith(PRCR)));
		VRM.insert(std::make_pair(RHS, LCR.intersectWith(PLCR)));
	}
}

void RangePass::visitSwitchInst(SwitchInst *SI, BasicBlock *BB, 
								ValueRangeMap &VRM)
{
	Value *V = SI->getCondition();
	IntegerType *Ty = dyn_cast<IntegerType>(V->getType());
	if (!Ty)
		return;
	
	ConstantRange VCR = getRange(SI->getParent(), V);
	ConstantRange CR(Ty->getBitWidth(), false);
						   
	if (SI->getDefaultDest() != BB) {
		// union all values that goes to BB
		for (SwitchInst::CaseIt i = SI->case_begin(), e = SI->case_end();
			 i != e; ++i) {
			if (i.getCaseSuccessor() == BB)
				safeUnion(CR, i.getCaseValue()->getValue());
		}
	} else {
		// default case
		for (SwitchInst::CaseIt i = SI->case_begin(), e = SI->case_end();
			 i != e; ++i)
			safeUnion(CR, i.getCaseValue()->getValue());
		CR = CR.inverse();
	}
	VRM.insert(std::make_pair(V, VCR.intersectWith(CR)));
}

void RangePass::visitTerminator(TerminatorInst *I, BasicBlock *BB,
								 ValueRangeMap &VRM) {
	if (BranchInst *BI = dyn_cast<BranchInst>(I))
		visitBranchInst(BI, BB, VRM);
	else if (SwitchInst *SI = dyn_cast<SwitchInst>(I))
		visitSwitchInst(SI, BB, VRM);
	else {
		I->dump(); llvm_unreachable("Unknown terminator!");
	}
}


bool RangePass::updateRangeFor(BasicBlock *BB)
{
	bool changed = false;

	// propagate value ranges from pred BBs, ranges in BB are union of ranges
	// in pred BBs, constrained by each terminator.
	for (pred_iterator i = pred_begin(BB), e = pred_end(BB);
			i != e; ++i) {
		BasicBlock *Pred = *i;
		if (isBackEdge(Edge(Pred, BB)))
			continue;
		
		ValueRangeMap &PredVRM = FuncVRMs[Pred];
		ValueRangeMap &BBVRM = FuncVRMs[BB];
		
		// Copy from its predecessor
		ValueRangeMap VRM(PredVRM.begin(), PredVRM.end());
		// Refine according to the terminator
		visitTerminator(Pred->getTerminator(), BB, VRM);
		
		// union with other predecessors
		for (ValueRangeMap::iterator j = VRM.begin(), je = VRM.end();
			 j != je; ++j) {
			ValueRangeMap::iterator it = BBVRM.find(j->first);
			if (it != BBVRM.end())
				safeUnion(it->second, j->second);
			else
				BBVRM.insert(*j);
		}
	}
	
	// Now run through instructions
	for (BasicBlock::iterator i = BB->begin(), e = BB->end(); 
		 i != e; ++i) {
		changed |= updateRangeFor(&*i);
	}
	
	return changed;
}

bool RangePass::updateRangeFor(Function *F)
{
	bool changed = false;
	
	FuncVRMs.clear();
	BackEdges.clear();
	FindFunctionBackedges(*F, BackEdges);
	
	for (Function::iterator b = F->begin(), be = F->end(); b != be; ++b)
		changed |= updateRangeFor(&*b);
	
	return changed;
}

bool RangePass::doModulePass(Module *M)
{
	unsigned itr = 0;
	bool changed = true, ret = false;

	while (changed) {
		// if some values converge too slowly, expand them to full-set
		if (++itr > MaxIterations) {
			for (ChangeSet::iterator it = Changes.begin(), ie = Changes.end();
				 it != ie; ++it) {
				RangeMap::iterator i = Ctx->IntRanges.find(*it);
				i->second = ConstantRange(i->second.getBitWidth());
			}
		}
		changed = false;
		Changes.clear();
		for (Module::iterator i = M->begin(), e = M->end(); i != e; ++i)
			if (!i->empty())
				changed |= updateRangeFor(&*i);
		ret |= changed;
	}
	return ret;
}


void RangePass::dumpRange()
{
	raw_ostream &OS = dbgs();
	for (RangeMap::iterator i = Ctx->IntRanges.begin(), 
		e = Ctx->IntRanges.end(); i != e; ++i) {
		OS << i->first << " " << i->second << "\n";
	}
}

