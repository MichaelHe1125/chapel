/*
 * Copyright 2004-2017 Cray Inc.
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

#include "errorHandling.h"

#include "AstVisitorTraverse.h"
#include "CatchStmt.h"
#include "driver.h"
#include "stmt.h"
#include "symbol.h"
#include "TryStmt.h"

#include <stack>

/*
This is a pseudo-code example of what this pass is supposed to do with a
throwing function, transforming error handling constructs.

If using try in functions that do not propagate, a catch-all is required
(catch or catch err). This can be avoided by using try!

Note that errors are not yet automatically deallocated when the error
is consumed by a catch.

// given code
proc propagate() throws {
  try {
    a(); // throws
    b(); // does not throw
    c(); // throws
  } catch e: SubError {
    f();
  } catch e: AnotherSubError {
    g();
  }
}

// after this pass
proc propagate(out error_out: Error) {
  var error: Error;
  a(error);
  if error then
    goto handler;
  b();
  c(error);
  if error then
    goto handler;

  label handler:
  if error {
    var e = error: SubError;
    if _cast {
      f();
      delete e;
    } else {
      var e = error: AnotherSubError;
      if e {
        g();
        delete e;
      } else {
        // set and return
        error_out = error;
        goto epilogue_label;
      }
    }
  }
}

nested try:
try {
  try {
    throwingCall();
  } catch e: SpecificError {
    handleGracefully();
  }
  otherThrowingCall();
} catch {
  handleSomehow();
}

{
  var _e1: Error;
  {
    var _e2: Error;
    throwingCall(_e2);
    if _e2 then
      goto handler2;

    label handler2:
    if _e2 {
      var _cast = _e2: SpecificError;
      if _cast {
        handleGracefully();
        delete _cast;
      } else {
        _e1 = _e2;
        goto handler1;
      }
    }
  }
  otherThrowingCall(_e1);
  if _e1 then
    goto handler1;

  label handler1:
  if _e1 {
    handleSomehow();
    delete _e1;
  }
}
*/


// Static functions
static bool canBlockThrow(BlockStmt* blk);
static void checkErrorHandling(FnSymbol* fn);


namespace {

class ErrorHandlingVisitor : public AstVisitorTraverse {

public:
  ErrorHandlingVisitor       (ArgSymbol* _outFormal, LabelSymbol* _epilogue);

  virtual bool enterTryStmt (TryStmt*   node);
  virtual void exitTryStmt  (TryStmt*   node);
  virtual void exitCatchStmt(CatchStmt* node);
  virtual bool enterCallExpr(CallExpr*  node);

private:
  struct TryInfo {
    VarSymbol*   errorVar;
    LabelSymbol* handlerLabel;
    TryStmt*     tryStmt;
    BlockStmt*   tryBody;
  };

  std::stack<TryInfo> tryStack;
  std::stack<TryInfo> catchesStack;
  ArgSymbol*          outError;
  LabelSymbol*        epilogue;

  void   lowerCatches      (const TryInfo& info);
  AList  setOutGotoEpilogue(VarSymbol*     error);
  AList  errorCond         (VarSymbol*     errorVar,
                            BlockStmt*     thenBlock,
                            BlockStmt*     elseBlock = NULL);
  CallExpr* haltExpr       ();

  ErrorHandlingVisitor();
};

ErrorHandlingVisitor::ErrorHandlingVisitor(ArgSymbol*   _outError,
                                           LabelSymbol* _epilogue) {
  outError = _outError;
  epilogue = _epilogue;
}

bool ErrorHandlingVisitor::enterTryStmt(TryStmt* node) {
  SET_LINENO(node);

  VarSymbol*   errorVar     = newTemp("error", dtError);
  LabelSymbol* handlerLabel = new LabelSymbol("handler");
  TryInfo      info         = {errorVar, handlerLabel, node, node->body()};
  tryStack.push(info);

  return true;
}

void ErrorHandlingVisitor::exitTryStmt(TryStmt* node) {
  SET_LINENO(node);

  TryInfo info = tryStack.top();
  tryStack.pop();

  BlockStmt* tryBody = info.tryBody;

  tryBody->insertAtHead(new DefExpr(info.errorVar));
  tryBody->insertAtTail(new DefExpr(info.handlerLabel));

  if (node->_catches.empty()) {
    lowerCatches(info); // no exitCatchStmt, so called here
  } else {
    catchesStack.push(info);
  }

  // may be NULL due to replacement of an enclosing try
  if (tryBody->parentExpr)
    tryBody->remove();

  node->replace(tryBody);
}

void ErrorHandlingVisitor::exitCatchStmt(CatchStmt* node) {
  // last CatchStmt to have its contents lowered; lower catches structure
  if (node->next == NULL) {
    TryInfo info = catchesStack.top();
    catchesStack.pop();
    lowerCatches(info);
  }
}

void ErrorHandlingVisitor::lowerCatches(const TryInfo& info) {
  TryStmt*   tryStmt  = info.tryStmt;
  VarSymbol* errorVar = info.errorVar;

  SET_LINENO(tryStmt);

  BlockStmt* handlers    = new BlockStmt();
  BlockStmt* currHandler = handlers;
  bool       hasCatchAll = false;

  for_alist(c, tryStmt->_catches) {
    if (hasCatchAll)
      INT_FATAL(c->prev, "catchall placed before the end of a catch list");

    SET_LINENO(c);

    CatchStmt* catchStmt = toCatchStmt(c);
    BlockStmt* catchBody = catchStmt->body();
    DefExpr*   catchDef  = catchStmt->expr();

    catchBody->insertAtTail(new CallExpr(gChplDeleteError, errorVar));
    catchBody->remove();

    // catchall
    if (catchDef == NULL) {
      hasCatchAll = true;
      currHandler->insertAtTail(catchBody);
    } else {
      VarSymbol* errSym  = toVarSymbol(catchDef->sym);
      Type*      errType = errSym->type;

      catchDef->remove();
      currHandler->insertAtTail(catchDef);

      // named catchall
      if (errType == dtError) {
        hasCatchAll = true;
        currHandler->insertAtTail(new CallExpr(PRIM_MOVE, errSym, errorVar));
        currHandler->insertAtTail(errorCond(errSym, catchBody));

      // specified catch
      } else {
        CallExpr*  castError   = new CallExpr(PRIM_DYNAMIC_CAST,
                                              new SymExpr(errType->symbol),
                                              errorVar);
        BlockStmt* nextHandler = new BlockStmt();

        currHandler->insertAtTail(new CallExpr(PRIM_MOVE, errSym, castError));
        currHandler->insertAtTail(errorCond(errSym, catchBody, nextHandler));

        currHandler = nextHandler;
      }
    }
  }

  if (!hasCatchAll) {
    if (tryStmt->tryBang()) {
      currHandler->insertAtTail(haltExpr());
    } else if (!tryStack.empty()) {
      TryInfo* outerTry = & tryStack.top();
      currHandler->insertAtTail(new CallExpr(PRIM_MOVE, outerTry->errorVar,
                                             errorVar));
      currHandler->insertAtTail(new GotoStmt(GOTO_ERROR_HANDLING,
                                             outerTry->handlerLabel));
    } else if (outError != NULL) {
      currHandler->insertAtTail(setOutGotoEpilogue(errorVar));
    } else {
      INT_FATAL(tryStmt, "try without a catchall in a non-throwing function");
    }
  }

  info.tryBody->insertAtTail(errorCond(errorVar, handlers));
}

bool ErrorHandlingVisitor::enterCallExpr(CallExpr* node) {
  bool insideTry = !tryStack.empty();

  if (FnSymbol* calledFn = node->resolvedFunction()) {
    if (calledFn->throwsError()) {
      SET_LINENO(node);

      VarSymbol* errorVar    = NULL;
      BlockStmt* errorPolicy = new BlockStmt();
      Expr*      insert      = node->getStmtExpr();
      if (insert == NULL)
        insert = node;

      if (insideTry) {
        TryInfo info = tryStack.top();
        errorVar = info.errorVar;

        errorPolicy->insertAtTail(new GotoStmt(GOTO_ERROR_HANDLING,
                                               info.handlerLabel));
      } else if (fStrictErrorHandling) {
        INT_FATAL(node, "throwing call without try or try! (strict mode)");
      } else {
        // without try, need an error variable
        errorVar = newTemp("error", dtError);
        insert->insertBefore(new DefExpr(errorVar));

        if (outError != NULL)
          errorPolicy->insertAtTail(setOutGotoEpilogue(errorVar));
        else
          errorPolicy->insertAtTail(haltExpr());
      }

      node->insertAtTail(errorVar); // adding error argument to call
      insert->insertAfter(errorCond(errorVar, errorPolicy));
    }
  } else if (node->isPrimitive(PRIM_THROW)) {
    SET_LINENO(node);

    BlockStmt* throwBlock = new BlockStmt();
    node->replace(throwBlock);

    SymExpr*   thrownExpr  = toSymExpr(node->get(1)->remove());
    VarSymbol* thrownError = toVarSymbol(thrownExpr->symbol());

    if (insideTry) {
      TryInfo   info      = tryStack.top();
      CallExpr* castError = new CallExpr(PRIM_CAST, dtError->symbol,
                                         thrownError);
      throwBlock->insertAtTail(new CallExpr(PRIM_MOVE, info.errorVar,
                                            castError));
      throwBlock->insertAtTail(new GotoStmt(GOTO_ERROR_HANDLING,
                                            info.handlerLabel));
    } else if (outError != NULL) {
      throwBlock->insertAtTail(setOutGotoEpilogue(thrownError));
    } else {
      INT_FATAL(node, "cannot throw in a non-throwing function");
    }
  }
  return true;
}

// Sets the fn out variable with the given error, then goes to the fn epilogue.
AList ErrorHandlingVisitor::setOutGotoEpilogue(VarSymbol* error) {
  CallExpr* castError = new CallExpr(PRIM_CAST, dtError->symbol, error);

  AList ret;
  // Using PRIM_ASSIGN instead of PRIM_MOVE here to work around
  // errors that come up in C compilation.
  ret.insertAtTail(new CallExpr(PRIM_ASSIGN, outError, castError));
  ret.insertAtTail(new GotoStmt(GOTO_RETURN, epilogue));

  return ret;
}

AList ErrorHandlingVisitor::errorCond(VarSymbol* errorVar,
                                      BlockStmt* thenBlock,
                                      BlockStmt* elseBlock) {
  VarSymbol* errorExistsVar = newTemp("errorExists", dtBool);
  CallExpr*  errorExists    = new CallExpr(PRIM_NOTEQUAL, errorVar, gNil);

  AList ret;
  ret.insertAtTail(new DefExpr(errorExistsVar));
  ret.insertAtTail(new CallExpr(PRIM_MOVE, errorExistsVar, errorExists));
  ret.insertAtTail(new CondStmt(new SymExpr(errorExistsVar),
                                thenBlock, elseBlock));
  return ret;
}

// TODO: take in a halt message from the error
CallExpr* ErrorHandlingVisitor::haltExpr() {
  return new CallExpr(PRIM_RT_ERROR, new_CStringSymbol("uncaught error"));
}

} /* end anon namespace */

void lowerErrorHandling() {
  if (!fMinimalModules)
    INT_ASSERT(dtError->inTree());

  forv_Vec(FnSymbol, fn, gFnSymbols) {
    // Determine if compiler-generated fns should be marked 'throws'
    if (fn->hasFlag(FLAG_ON)) {
      if (canBlockThrow(fn->body))
        fn->throwsErrorInit();
    } else {
      // Otherwise, just check for error-handling errors.
      checkErrorHandling(fn);
    }
  }

  // Quit if fatal errors were encountered by checkErrorHandling above.
  USR_STOP();

  forv_Vec(FnSymbol, fn, gFnSymbols) {
    ArgSymbol*   outError = NULL;
    LabelSymbol* epilogue = NULL;

    if (fn->throwsError()) {
      SET_LINENO(fn);

      outError = new ArgSymbol(INTENT_REF, "error_out", dtError);
      fn->insertFormalAtTail(outError);

      epilogue = fn->getOrCreateEpilogueLabel();
      INT_ASSERT(epilogue); // throws requires an epilogue
    }

    ErrorHandlingVisitor visitor = ErrorHandlingVisitor(outError, epilogue);
    fn->accept(&visitor);
  }
}

namespace {

class CanThrowVisitor : public AstVisitorTraverse {

public:
  CanThrowVisitor       (bool inThrowingFn, bool makeCompileErrors);

  virtual bool enterTryStmt  (TryStmt*   node);
  virtual void exitTryStmt   (TryStmt*   node);
  virtual bool enterCallExpr (CallExpr*  node);

  bool throws() { return canThrow; }

private:
  int  tryDepth;
  bool canThrow;
  bool errors;
  bool fnCanThrow; // only used for error checking

  bool   catchesNotExhaustive(TryStmt* tryStmt);
};

CanThrowVisitor::CanThrowVisitor(bool inThrowingFn, bool makeCompileErrors) {
  tryDepth = 0;
  canThrow = false;
  errors = makeCompileErrors;
  fnCanThrow = inThrowingFn;
}

bool CanThrowVisitor::enterTryStmt(TryStmt* node) {
  tryDepth++;

  return true;
}

void CanThrowVisitor::exitTryStmt(TryStmt* node) {
  tryDepth--;

  // is it an exhaustive catch?

  bool nonExhaustive = catchesNotExhaustive(node);

  if (node->tryBang()) {
    canThrow = false;
  } else {
    canThrow = nonExhaustive;
    if (errors && tryDepth==0 && nonExhaustive && !fnCanThrow) {
      USR_FATAL_CONT(node, "try without a catchall in a non-throwing function");
    }
  }
}

bool CanThrowVisitor::catchesNotExhaustive(TryStmt* tryStmt) {

  bool hasCatchAll = false;

  for_alist(c, tryStmt->_catches) {
    if (errors && hasCatchAll)
      USR_FATAL_CONT(c->prev, "catchall placed before the end of a catch list");

    SET_LINENO(c);

    CatchStmt* catchStmt = toCatchStmt(c);
    DefExpr*   catchDef  = catchStmt->expr();

    // catchall
    if (catchDef == NULL) {
      hasCatchAll = true;
    } else {
      VarSymbol* errSym  = toVarSymbol(catchDef->sym);
      Type*      errType = errSym->type;

      // named catchall
      if (errType == dtError) {
        hasCatchAll = true;
      }
    }
  }

  return !hasCatchAll;
}

bool CanThrowVisitor::enterCallExpr(CallExpr* node) {
  bool insideTry = (tryDepth > 0);

  if (FnSymbol* calledFn = node->resolvedFunction()) {
    if (calledFn->throwsError()) {
      if (insideTry) {
        // OK
      } else {
        if (errors && fStrictErrorHandling) {
          USR_FATAL_CONT(node, "throwing call without try or try! (strict mode)");
        }
        // not in a try
        canThrow = true;
      }
    }
  } else if (node->isPrimitive(PRIM_THROW)) {
    canThrow = true;

    if (insideTry) {
      // OK, error checking for this case done in try handling
    } else if (fnCanThrow == true) {
      // OK, fn can throw
    } else if (errors == true) {
      USR_FATAL_CONT(node, "cannot throw in a non-throwing function");
    }
  }
  return true;
}

} /* end anon namespace */


// Returns `true` if a block can exit with an error
//  (e.g. by calling 'throw' or a throwing function,
//   when these are not handled by try! or catch).
// This function is useful to infer 'throws' for
// certain compiler-introduced functions.

static bool canBlockThrow(BlockStmt* block)
{
  CanThrowVisitor visit(false, false);

  block->accept(&visit);

  return visit.throws();
}

static void checkErrorHandling(FnSymbol* fn)
{
  CanThrowVisitor visit(fn->throwsError(), true);

  fn->body->accept(&visit);
}
