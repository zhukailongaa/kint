#define DEBUG_TYPE "cmp-range"
#include <llvm/Instructions.h>
#include <llvm/Pass.h>
#include <llvm/ADT/OwningPtr.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/Support/InstIterator.h>
#include <llvm/Support/raw_ostream.h>
#include "Diagnostic.h"

using namespace llvm;

namespace {

typedef const char *CmpStatus;
static CmpStatus CMP_FALSE = "comparison always false";
static CmpStatus CMP_TRUE = "comparison always true";

struct CmpRange : FunctionPass {
	static char ID;
	CmpRange() : FunctionPass(ID) {
		PassRegistry &Registry = *llvm::PassRegistry::getPassRegistry();
		initializeScalarEvolutionPass(Registry);
	}

	virtual void getAnalysisUsage(AnalysisUsage &AU) const {
		AU.setPreservesAll();
		AU.addRequired<ScalarEvolution>();
	}

	virtual bool runOnFunction(Function &F) {
		SE = &getAnalysis<ScalarEvolution>();
		inst_iterator i = inst_begin(F), e = inst_end(F);
		for (; i != e; ++i) {
			if (ICmpInst *ICI = dyn_cast<ICmpInst>(&*i)) {
				check(ICI);
			}
		}
		return false;
	}

private:
	Diagnostic Diag;
	ScalarEvolution *SE;

	void check(ICmpInst *);
};

} // anonymous namespace

void CmpRange::check(ICmpInst *I) {
	if (!SE->isSCEVable(I->getOperand(0)->getType()))
		return;
	const SCEV *L = SE->getSCEV(I->getOperand(0));
	const SCEV *R = SE->getSCEV(I->getOperand(1));
	// Ignore constant comparison.
	if (isa<SCEVConstant>(L) && isa<SCEVConstant>(R))
		return;
	// Ignore loop variables.
	if (isa<llvm::SCEVAddRecExpr>(L) || isa<llvm::SCEVAddRecExpr>(R))
		return;
	CmpStatus Reason;
	if (SE->isKnownPredicate(I->getPredicate(), L, R))
		Reason = CMP_TRUE;
	else if (SE->isKnownPredicate(I->getInversePredicate(), L, R))
		Reason = CMP_FALSE;
	else
		return;
	Diag << "---";
	Diag << "bug:   " << Reason << "\n";
	Diag << "lhs:   " << *L << '\n';
	Diag << "rhs:   " << *R << '\n';
	Diag << "stack: \n";
	Diag.backtrace(I, "  - ");
}

char CmpRange::ID;

static RegisterPass<CmpRange>
X("cmp-range", "Detecting bogus comparisons via ranges", false, true);
