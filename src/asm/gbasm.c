#include <stdio.h>

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    fprintf(stderr,
            "gbasm — SM83 assembler\n"
            "Usage: gbasm <input.asm> -o <output.gb> [--sym <output.sym>]\n");
    return 1;
}
