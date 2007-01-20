#include <stdio.h>
#include <stdlib.h>
#include "yy.h"


void yyerror(const char *str) {
  fprintf(stderr, "%s:%d ERROR: %s\n", yyfilename, chplLineno, str);
  fprintf(stderr, "yytext is: %s\n", yytext);
  exit(1);
}

int yywrap() {
  return 1;
}
