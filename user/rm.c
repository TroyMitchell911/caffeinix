#include "user.h"

#define TAG                     "rm: "

int main(int argc, char **argv)
{
        int i;

        if(argc < 2) {
                printf(TAG"missing operand\n");
                return -1;
        }

        for(i = 1; i < argc; i++) {
                if(unlink(argv[i]) < 0) {
                        fprintf(2, TAG"cannot remove '%s'\n", argv[i]);
                        break;
                }
        }

        if(i != argc)
                return -1;

        return 0;
}