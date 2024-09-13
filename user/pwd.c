/*
 * @Date: 2024-09-13
 * @LastEditors: GoKo-Son626
 * @LastEditTime: 2024-09-13
 * @FilePath: /caffeinix/user/pwd.c
 * @Description: Words are cheap so I do.
 */
#include "user.h"

#define MAXPATH                 128

int main(int argc, char *argv[]) {
        char cwd[MAXPATH];

        if (getcwd(cwd, MAXPATH)) {
                return -1;
        }

        printf("%s\n", cwd);

        return 0;
}