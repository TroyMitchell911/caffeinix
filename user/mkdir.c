#include "user.h"

int main(int argc, char *argv[])
{
        int ret;
        if(argc < 2) {
                fprintf(2, "mkdir: missing operand\n");
                return -1;
        }

        ret = mkdir(argv[1]);
        if(ret < 0) {
                fprintf(2, "mkdir: %s failed\n", argv[1]);
                return -1;
        }
        return 0;
}