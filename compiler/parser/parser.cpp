#include <stdlib.h>
#include "build.h"
#include "countTokens.h"
#include "files.h"
#include "parser.h"
#include "stringutil.h"
#include "symbol.h"
#include "chapel.tab.h"
#include "yy.h"

AList* yystmtlist = NULL;
char* yyfilename;
int chplLineno;
int yystartlineno;
extern YYLTYPE yylloc;

static char* filenameToModulename(char* filename) {
  char* modulename = stringcpy(filename);
  char* lastslash = strrchr(modulename, '/');
  if (lastslash) {
    modulename = lastslash+1;
  }
  char* dot = strchr(modulename, '.');
  if (dot) {
    *dot = '\0';
  }

  return modulename;
}

static void
clearModulesDefPoints(AList* stmts) {
  for_alist(Expr, stmt, stmts) {
    if (BlockStmt* block = dynamic_cast<BlockStmt*>(stmt))
      stmt = block->body->first();
    if (DefExpr* defExpr = dynamic_cast<DefExpr*>(stmt))
      if (ModuleSymbol* mod = dynamic_cast<ModuleSymbol*>(defExpr->sym))
        mod->defPoint = NULL;
  }
}

bool containsOnlyModules(AList* stmts) {
  for_alist(Expr, stmt, stmts) {
    bool isModuleDef = false;
    if (BlockStmt* block = dynamic_cast<BlockStmt*>(stmt))
      stmt = block->body->first();
    if (DefExpr* defExpr = dynamic_cast<DefExpr*>(stmt))
      if (dynamic_cast<ModuleSymbol*>(defExpr->sym))
        isModuleDef = true;
    if (!isModuleDef)
      return false;
  }
  return true;
}

ModuleSymbol* ParseFile(char* filename, modType moduletype) {
  ModuleSymbol* newModule = NULL;
  yyfilename = filename;
  yylloc.first_column = yylloc.last_column = yylloc.first_line = yylloc.last_line = yystartlineno = chplLineno = 0;

  yylloc.first_column = yylloc.last_column = 0;
  yylloc.first_line = yylloc.last_line = yystartlineno = chplLineno = 1;
  yyin = openInputFile(filename);
  
  yystmtlist = NULL;
  if (moduletype == MOD_USER) {
    startCountingFileTokens(filename);
  }
  yyparse();
  if (moduletype == MOD_USER) {
    stopCountingFileTokens();
  }

  closeInputFile(yyin);

  if (!containsOnlyModules(yystmtlist)) {
    char* modulename = filenameToModulename(filename);
    newModule = build_module(modulename, moduletype, yystmtlist);
  } else {
    clearModulesDefPoints(yystmtlist);
  }

  yyfilename = "<internal>";

  yylloc.first_column = yylloc.last_column = 0;
  yylloc.first_line = yylloc.last_line = yystartlineno = chplLineno = -1;

  return newModule;
}

