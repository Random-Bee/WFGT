// included first with -include and -DTEXT_SHIFT=n: puts n bytes in .text
// ahead of every function the file defines (GCC emits top-level asm before
// any function), so with n a multiple of 64 each instruction keeps its
// offset into its 64-byte block while its address moves by n
#ifndef TEXT_SHIFT
#define TEXT_SHIFT 0
#endif
#define TEXT_SHIFT_STRING2(x) #x
#define TEXT_SHIFT_STRING(x) TEXT_SHIFT_STRING2(x)
asm(".pushsection .text\n\t.skip " TEXT_SHIFT_STRING(TEXT_SHIFT) ", 0xcc\n\t.popsection");
