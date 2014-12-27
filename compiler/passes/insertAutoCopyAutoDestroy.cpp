/*
 * Copyright 2004-2014 Cray Inc.
 * Other additional copyright holders may be indicated within.
 *
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// TODO: This should be moved to symbol.cpp
bool fWarnOwnership = false;

// insertAutoCopyAutoDestroy
//
// Inserts copy-constructor and destructor calls as needed, by tracking the
// "ownership" of record objects.
//
//#############################################################################
//#
//# Ownership represents the "liveness" of a record object in the sense that it
//# may contain heap-allocated data.  After a record object is constructed, any
//# of its fields of class type may refer to a class object.  In that state, any
//# heap-allocated fields in the record are "owned" by the record.  Calling the
//# destructor for that record type gives the record author the opportunity to
//# clean up any contained class fields and thereby avoid leaking memory.  After
//# the destructor is called, the record variable no longer owns any
//# heap-allocated data, so that data is "unowned".
//#
//# In the simplest view, ownership is a binary state: construction changes the
//# state from false to true; destruction changes the state from true to false.
//#
//# Ownership of a record can also be transferred by a bit-wise copy.  When a
//# bit-wise copy of a record is made, class fields (which are implemented as
//# pointers) are copied verbatim.  That means that both record objects point to
//# the same class data.  Ownership is then shared between them.  Some time
//# before both copies go out of scope, one of them needs to call its
//# destructor, the other should not (to avoid double-deletion errors).
//#
//# When ownership is shared in this way, we can perform a sort of internal
//# reference-counting, maintaining a list of aliases, and ensuring that the
//# destructor is called on one of them before the last member of that alias set
//# goes out of scope. 
//#
//# For these purposes, we assume that none of the class fields of an alias is
//# updated, so they remain equivalent throughout.  If that is not true, then
//# the two variables represent independent variables.  In that case, a
//# copy-constructor call should have been inserted because each variable should
//# have its own initialization.  So its absence would indicate an error
//# somewhere upstream.
//#
//# We assume that the input AST contains the minimum number of
//# copy-constructor calls required to provide the specified semantics.  That
//# is, record values passed by in intent must be copied.  We do not yet
//# attempt to pass ownership of an object into a called routine.  Two kinds of
//# variable whose ownersihp must be true before exiting a routine are:
//#  - The return value variable
//#  - A record field in a record.
//# If ownership of any of these is false when they (or their container) is
//# about to go out of scope, a copy must be made to bring ownership to the
//# expected state.  Otherwise, ownership can be shared and a copy potentially saved.
//#
//# After copy-constructor calls have been added, the routine has its full
//# complement of construcotr calls.  Correct AMM is then just a matter of
//# inserting the minimum number of destructor calls to drive the ownership of
//# all local variables to false before the routine ends.  Dataflow analysis is
//# used to propagate this constraint backwards, in case ownership is
//# transferred out of a variable along one path but not another.
//#
//#############################################################################

// The sets used for flow analysis are:
//  GEN --  The set of symbols whose ownership transitions to true by the end of
//          the block.
//  KILL -- The set of symbols whose ownership transitions to false by the end
//          of the block.
//  IN --   The set of symbols that are owned at the beginning of the block.
//  OUT --  The set of symbols that are owned at the end of the block.
//
// GEN[i] contains a true bit corresponding to each symbol that is constructed
// in that block.  We expect that symbol is constructed only once, so if IN[i]
// and GEN[i] have a true bit in the same position, we can flag an internal
// error.  This condition is unlikely, so at best it should be added as a
// verify check.
//
// KILL[i] contains a true bit corresponding to each symbol that is destroyed
// or whose ownership is usurped in that block.  We keep a separate list of
// aliases.  When a bitwise copy is made, we treat the new copy as if it has
// been constructed without changing the state of the original.  When any
// member of that set of aliases is destroyed or usurped, all members of the
// set are added to KILL[i].
//
// We start by performing forward flow analysis to establish the value of IN.  
// IN[i] is the intersection of OUT[pred(i)] for all predecessors of block i.
// It is desirable to allow ownership to flow through to the end of the
// function (to a final block with many predecessors).  That way, we can
// consume liveness with a minimum of destructor calls.  However, if we cannot
// depend on a given symbol being owned at the beginning of a block, then its
// destructor must be pushed back up the flow graph.  For example, we might
// pass ownership to a field or global variable on one path; then we need a
// destructor call to get rid of ownership on the other path(s).
//
// For a block that contains a constructor followed by a destructor, both
// GEN[i] and KILL[i] will be zero.  If they are both true, that represents a
// symbol being destroyed and then reused, so GEN takes precedence over KILL
// in forward flow: OUT[i] = IN[i] - KILL[i] + GEN[i].
//
// For backward flow, we compute OUT[i] as the intersection of IN[succ(i)] for
// all successors of block i.  (No information is propagated backward through
// the blocks, so this can be done in one iteration.)  Then, in the local
// traversal the follows, for any symbol that remains owned at the end of block
// i where its OUT[i] is false, we add a destructor call to make that condition
// true.
//

#include "passes.h" // The main routine is declared here.

#include "bb.h"
#include "expr.h"
#include "symbol.h"
#include "bitVec.h"
#include "stlUtil.h"
#include "astutil.h"
#include "resolution.h" // For AutoDestroyMap.

#include <map>
#include <vector>

typedef std::vector<BitVec*> FlowSet;
typedef std::vector<Symbol*> SymbolVector;
typedef std::map<Symbol*, size_t> SymbolIndexMap;
typedef SymbolIndexMap::value_type SymbolIndexElement;
typedef std::map<Symbol*, SymbolVector*> AliasVectorMap;
typedef AliasVectorMap::value_type AliasMapElement;
typedef std::vector<SymExpr*> SymExprVector;
typedef std::vector<DefExpr*> DefExprVector;


static void createFlowSet(std::vector<BitVec*>& set,
                          size_t                nbbs,
                          size_t                size)
{
  // Create a BitVec of length size for each block.
  for (size_t i = 0; i < nbbs; ++i)
    set.push_back(new BitVec(size));
}


static void destroyFlowSet(std::vector<BitVec*> set)
{
  for_vector(BitVec, vec, set)
    delete vec, vec = 0;
}


// An alias vector map is map from symbols to lists of symbols, but all of the
// symbols in the same clique share the same list.  So, to destroy all the
// lists in the map, we find the list through the first member of the clique,
// then set the map element for each member in the list to NULL.  (The list
// contains the element though which the list is accessed, so this detaches the
// list from all map elements.)  Then we delete the list.  (The symbols
// contained in the list are owned elsewhere.)
static void destroyAliasVectorMap(AliasVectorMap& aliases)
{
  for (AliasVectorMap::iterator it = aliases.begin();
       it != aliases.end(); ++it)
  {
    if (it->second == NULL)
      continue;

    SymbolVector* aliasList = it->second;
    for_vector(Symbol, sym, *aliasList)
      aliases[sym] = NULL;
      
    delete aliasList;
  }
}


// Scans the body of the given function and inserts all of the variable and
// argument symbols defined in it into the vector of symbols.  Bits in the flow
// analysis bit-vectors correspond to the entries in this vector.
// Also constructs an index map, to make it easier to find the index of a
// symbol in the vector.  (Otherwise, a linear search is required.)
// The alias map can also be populated at the same time.  
// TODO: Split the alias list off as a separate class, mostly to make it easy
// to document the data structure it implements.
static void
extractSymbols(FnSymbol* fn,
               SymbolVector& symbols,
               SymbolIndexMap& symbolIndex,
               AliasVectorMap& aliases)
{
  DefExprVector defExprs;
  collectDefExprsSTL(fn, defExprs);

  for_vector(DefExpr, def, defExprs)
  {
    Symbol* sym = def->sym;

    // We are interested only in arguments and variables.
    if (! (toArgSymbol(sym) || toVarSymbol(sym)))
      continue;

    Type* type = sym->type;

    // TODO: Extern record types also do not have constructors and
    // destructors.  To treat them uniformly, we would have to enforce that
    // extern types supply constructors and destructors and/or supply them
    // internally.
    TypeSymbol* ts = type->symbol;
    if (ts->hasFlag(FLAG_EXTERN))
      continue;

    // We are concerned only with record types.
    // TODO: This is too bad, because it would be nice to be able to treat all
    // value types uniformly.  But for that to work, arguments of fundamental
    // type must be constructed by having their values piped through a copy
    // constructor.  Currently, that is not the case.

    // We are interested only in records passed by value.  Records passed by
    // ref appear be a class in the current AST because _ref(T) is a class type.
    AggregateType* at = toAggregateType(type);
    if (at == NULL)
      // Not an aggregate type, so not a record.
      continue;
    if (!at->isRecord())
      // Not a record.
      continue;

    symbolIndex.insert(SymbolIndexElement(sym, symbols.size()));
    symbols.push_back(sym);

    // We expect the symbolIndex to return the index of that symbol in the
    // symbols vector.
    INT_ASSERT(symbols[symbolIndex[sym]] == sym);

    // Initialize each entry in the alias map with a list of symbols
    // containingh the symbol itself.
    SymbolVector* aliasList = new SymbolVector();
    aliasList->push_back(sym);
    aliases.insert(AliasMapElement(sym, aliasList));
  }
}


// Returns true if this function returns a fully-constructed value; false otherwise.
static bool isConstructor(CallExpr* call)
{
  if (FnSymbol* fn = call->isResolved())
  {
    // A "normal" function.

    // Return values of class type are ruled out.
    Type* retType = fn->retType;
    if (AggregateType* at = toAggregateType(retType))
      if (at->isClass())
        return false;
  }
  else
  {
    // A primitive.
    if (AggregateType* at = toAggregateType(call->typeInfo()))
      if (at->isClass())
        return false;
  }

  return true;
}


static void processConstructor(CallExpr* call, SymExpr* se, 
                               BitVec* gen,
                               const SymbolIndexMap& symbolIndex)
{
  // In the current incarnation, we expect construction to look like:
  //  ('move' lhs (construct <args>))
  // When constructors turn into methods, this will look a bit different.
  if (call->isPrimitive(PRIM_MOVE) ||
      call->isPrimitive(PRIM_ASSIGN))
  {
    if (CallExpr* rhsCall = toCallExpr(call->get(2)))
    {
      if (isConstructor(rhsCall))
      {
        // Any function returning a value is considered to be a constructor.
        Symbol* sym = se->var;
        size_t index = symbolIndex.at(sym);
        // We expect that each symbol gets constructed only once, so if we are
        // about to set a bit in the gen set, it cannot already be true.
        INT_ASSERT(gen->get(index) == false);
        // If this assumption turns out to be false, it means we are reusing
        // symbols.  That case can be accommodated, but it means we have to
        // insert a destructor call ahead of the symbol's reinitialization.
        gen->set(index);
      }
    }
  }
}


// Merge the alias lists of two symbols that have become aliases.
static void mergeAliases(Symbol* orig, Symbol* alias,
                         AliasVectorMap& aliases)
{
  SymbolVector* origList = aliases[orig];
  SymbolVector* aliasList = aliases[alias];
  for (size_t i = 0; i < aliasList->size(); ++i)
  {
    Symbol* sym = aliasList->at(i);
    origList->push_back(sym);
  }
  delete aliasList;
  aliases[alias] = origList;
}


static void processMove(CallExpr* call, SymExpr* se, BitVec* gen, AliasVectorMap& aliases, 
                        const SymbolIndexMap& symbolIndex)
{
  // We only care about bitwise copies here.
  if (call->isPrimitive(PRIM_MOVE) ||
      call->isPrimitive(PRIM_ASSIGN))
  {
    // We'll key off the LHS.
    // Meaning that we will only pay attention to the expression if the SymExpr
    // is in the LHS position in the primitive.
    if (SymExpr* lhs = toSymExpr(call->get(1)))
    {
      if (lhs == se)
      {
        // We only care about bitwise copies from one symbol to another.
        if (SymExpr* rhs = toSymExpr(call->get(2)))
        {
          Symbol* lsym = lhs->var;
          Symbol* rsym = rhs->var;
          size_t lindex = symbolIndex.at(lsym);
          size_t rindex = symbolIndex.at(rsym);
          // Copy ownership state from RHS.
          INT_ASSERT(gen->get(lindex) == false);
          if (!gen->get(rindex))
          {
            if (fWarnOwnership)
              USR_WARN(rsym, "Uninitialized symbol is copied here");
          }
          else
          {
            gen->set(lindex);
          }
          // Merge aliases whether or not they are live.
          mergeAliases(rsym, lsym, aliases);
        }
      }
    }
  }
}
  

static void processDestructor(SymExpr* se,
                              BitVec* kill, AliasVectorMap& aliases,
                              const SymbolIndexMap& symbolIndex)
{
  // Add all members of an alias clique to the kill set.
  // All members of an alias clique point to the same SymbolVector, so we only
  // need to look up one arbitrarily and then run the list.
  Symbol* sym = se->var;
  SymbolVector* aliasList = aliases[sym];

  for_vector(Symbol, alias, *aliasList)
  {
    size_t index = symbolIndex.at(alias);
    // We expect a symbol to be live when it is killed.
    INT_ASSERT(kill->get(index) == false);
    kill->set(index);
  }

  // We don't bother updating the alias list under the assumption that
  // symbols are not reused.  If that turns out to be false, we have to
  // remove the alias clique from aliases here.
}


// If this call acts like a destructor, then add the symbols it affects to the
// kill set.
static void processDestructor(CallExpr* call, SymExpr* se, BitVec* kill,
                               AliasVectorMap& aliases,
                               const SymbolIndexMap& symbolIndex)
{
  if (FnSymbol* fn = call->isResolved())
  {
    // This is a function call.

    // The only one we're interested in right now is a destructor call.
    if (fn->hasFlag(FLAG_DESTRUCTOR))
    {
      // Paranoid check: This SymExpr is the thing being destroyed, right?
      INT_ASSERT(call->get(1) == se);

      processDestructor(se, kill, aliases, symbolIndex);
    }
  }
  else
  {
    // This is a primitive.
    if (call->isPrimitive(PRIM_RETURN))
    {
      // Returns act like destructors.
      processDestructor(se, kill, aliases, symbolIndex);
    }
  }      
}


static void computeTransitions(SymExprVector& symExprs,
                               BitVec* gen, BitVec* kill,
                               AliasVectorMap& aliases,
                               const SymbolIndexMap& symbolIndex)
{
  for_vector(SymExpr, se, symExprs)
  {
    // We are only interested in local symbols, so if this one does not appear
    // in our map, move on.
    Symbol* sym = se->var;
    if (symbolIndex.find(sym) == symbolIndex.end())
      continue;

    // We are only interested in call expressions involving the SymExpr.
    if (CallExpr* call = toCallExpr(se->parentExpr))
    {
      processConstructor(call, se, gen, symbolIndex);
      processMove(call, se, gen, aliases, symbolIndex);
      processDestructor(call, se, kill, aliases, symbolIndex);
    }
  }
}


// Look for expressions that create, destroy or transfer ownership of a record
// object.
// Right now, we can get away with just looking for MOVE or ASSIGN primitives
// to determine where ownership is created or transferred.  When constructors
// become methods, we'll key off the CONSTRUCTOR flag and modify the state of
// the first (receiver) argument.
static void computeTransitions(BasicBlock& bb, BitVec* gen, BitVec* kill,
                               AliasVectorMap& aliases,
                               const SymbolIndexMap& symbolIndex)
{
  for_vector(Expr, expr, bb.exprs)
  {
    // Are all expressions in this list mutually exclusive?
    // Can we speed things up by processing only statement expressions?

    SymExprVector symExprs;
    collectSymExprsSTL(expr, symExprs);

    computeTransitions(symExprs, gen, kill, aliases, symbolIndex);
  }
}


static void computeTransitions(FnSymbol* fn,
                               FlowSet& GEN, FlowSet& KILL,
                               AliasVectorMap& aliases,
                               const SymbolIndexMap& symbolIndex)
{
  size_t nbbs = fn->basicBlocks->size();
  for (size_t i = 0; i < nbbs; ++i)
  {
    computeTransitions(*(*fn->basicBlocks)[i], GEN[i], KILL[i], aliases, symbolIndex);
  }
}


// This predicate determines if the given statement is a jump.
static bool isJump(Expr* stmt)
{
  // A goto is definitely a jump.
  if (isGotoStmt(stmt))
    return true;

  // A return primitive works like a jump. (Nothing should appear after it.)
  if (CallExpr* call = toCallExpr(stmt))
    if (call->isPrimitive(PRIM_RETURN))
      return true;

  return false;
}


// At the end of this basic block, insert an autodestroy for each symbol
// specified by the given bit-vector.
static void insertAutoDestroy(BasicBlock& bb, BitVec* to_kill, 
                              const SymbolVector& symbols)
{
  // Skip degenerate basic blocks.
  if (bb.exprs.size() == 0)
    return;

  // Find the last statement in the block.
  Expr*& last = bb.exprs.back();
  Expr* stmt = last->getStmtExpr();
  bool isjump = isJump(stmt);

  // For each true bit in the bit vector, add an autodestroy call.
  for (size_t j = 0; j < to_kill->size(); ++j)
  {
    if (to_kill->get(j))
    {
      Symbol* sym = symbols[j];
      CallExpr* autoDestroyCall = new CallExpr(autoDestroyMap.get(sym->type), sym);
      if (isjump)
        stmt->insertBefore(autoDestroyCall);
      else
        stmt->insertAfter(autoDestroyCall);
    }
  }
}

static void insertAutoDestroy(FnSymbol* fn,
                              FlowSet& GEN, FlowSet& KILL,
                              FlowSet& IN, FlowSet& OUT,
                              const SymbolVector& symbols)
{
  size_t nbbs = fn->basicBlocks->size();
  for (size_t i = 0; i < nbbs; i++)
  {
    // We need to insert an autodestroy call for each symbol that is owned
    // (live) at the end of the block but is unowned (dead) in the OUT set.
    BasicBlock& bb = *(*fn->basicBlocks)[i];
    BitVec* to_kill = *IN[i] + *GEN[i] - *KILL[i] - *OUT[i];
    insertAutoDestroy(bb, to_kill, symbols);
    delete to_kill; to_kill = 0;
  }
}

static void insertAutoDestroy(FnSymbol* fn)
{
  BasicBlock::buildBasicBlocks(fn);
  size_t nbbs = fn->basicBlocks->size();

  SymbolVector symbols;
  SymbolIndexMap symbolIndex;
  AliasVectorMap aliases;
  extractSymbols(fn, symbols, symbolIndex, aliases);

  size_t size = symbols.size();

#if DEBUG_AMM
  if (debug > 0)
  {
    printf("\n");
    list_view(fn);

    BasicBlock::printBasicBlocks(fn);
  }
#endif

  FlowSet GEN;
  FlowSet KILL;
  FlowSet IN;
  FlowSet OUT;

  createFlowSet(GEN, nbbs, size);
  createFlowSet(KILL, nbbs, size);
  createFlowSet(IN,   nbbs, size);
  createFlowSet(OUT,  nbbs, size);

  computeTransitions(fn, GEN, KILL, aliases, symbolIndex);

  BasicBlock::forwardFlowAnalysis(fn, GEN, KILL, IN, OUT, true);

  insertAutoDestroy(fn, GEN, KILL, IN, OUT, symbols);

  destroyAliasVectorMap(aliases);

  destroyFlowSet(GEN);
  destroyFlowSet(KILL);
  destroyFlowSet(IN);
  destroyFlowSet(OUT);
}


static void insertAutoDestroy()
{
  forv_Vec(FnSymbol, fn, gFnSymbols)
  {
    // Function prototypes have no body, so we skip them.
    if (fn->hasFlag(FLAG_FUNCTION_PROTOTYPE))
      continue;

    insertAutoDestroy(fn);
  }
}


void insertAutoCopyAutoDestroy()
{
  // First of all, just insert autoDestroy calls.
  // We assume that all necessary autoCopy calls have been added.
  // As an enhancement/optimization, we'll add a separate pass to compute and
  // add the minimum number of autoCopy calls required.
  insertAutoDestroy();
}
