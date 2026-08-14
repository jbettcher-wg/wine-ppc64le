/* Reproducer for the clang>=20 canonical-sugar spelling that wine_sig.py works around.
   A function TYPE mentioning size_t prints as the internal name __size_t, which is not
   spellable in source, so a reconstructed prototype fails to compile.  ParmVarDecl nodes
   still print size_t, which is why only return types were affected.  Compile with:
       clang -fsyntax-only toolchain-notes/clang_sugar_spelling.c   */
#include <stddef.h>
static __size_t a;
static __ptrdiff_t b;
static __signed_size_t c;
void u(void){(void)a;(void)b;(void)c;}
