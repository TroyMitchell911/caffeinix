/*
 * @Date: 2024-09-12
 * @LastEditors: GoKo-Son626
 * @LastEditTime: 2024-09-16
 * @FilePath: /caffeinix/user/rm.c
 * @Description: 
 */
#include "user.h"
#include "stat.h"

#define TAG                     "rm: "

int main(int argc, char **argv)
{
        int i;
        struct stat st;

        if(argc < 2) {
                printf(TAG"missing operand\n");
                return -1;
        }

        for(i = 1; i < argc; i++) {
                if(stat(argv[i], &st) < 0) {
                        printf(TAG"cannot stat '%s'\n", argv[i]);
                        continue;
                }

                if(st.type == T_DIR) {
                        printf(TAG"cannot remove '%s': Is a directory\n", argv[i]);
                        continue;
                }

                if(unlink(argv[i]) < 0) {
                        fprintf(2, TAG"cannot remove '%s'\n", argv[i]);
                        break;
                }
        }

        if(i != argc)
                return -1;

        return 0;
}