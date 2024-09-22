/*
 * @Date: 2024-09-12
 * @LastEditors: GoKo-Son626
 * @LastEditTime: 2024-09-22
 * @FilePath: /caffeinix/user/cp.c
 * @Description: 
 */
#include "user.h"
#include "fcntl.h"
#include "stat.h"
#include "dirent.h"

int cp_file(char *src, char *dst);
#define TAG                     "cp: "

static int cp_dir(char *src, char *dst) {
        int sfd, dfd;
        struct dirent entry;
        char src_path[1024], dst_path[1024];
        struct stat st;
        size_t bytes_read;

        dfd = open(dst, O_RDONLY);
        if(dfd < 0) {
                printf("cannot open '%s'\n", dst);
                if(mkdir(dst) < 0 ) {
                        printf(TAG"cannot create '%s'\n", dst);
                        return -1;
                }
        }
        dfd = open(dst, O_RDONLY);

        sfd = open(src, O_RDONLY);
        if(!sfd) {
                printf(TAG"cannot open directory '%s'\n", src);
                return -1;
        }

        while((bytes_read = read(sfd, &entry, sizeof(entry))) > 0) {
                if(strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0)
                        continue;
                printf("continue\n");

                strcpy(src_path, src);
                strcat(src_path, "/");
                strcat(src_path, entry.name);
                printf("%s\n", src_path);
                strcpy(dst_path, dst);
                strcat(dst_path, "/");
                strcat(dst_path, entry.name);
                printf("%s\n", dst_path);

                if(stat(src_path, &st) < 0) {
                        printf(TAG"cannot stat '%s'\n", src_path);
                        close(sfd);
                        return -1;
                }
                printf("abc\n");

                if(st.type == T_DIR) {
                        printf("folder-judge\n");
                        if(mkdir(dst_path) < 0) {
                                printf("cannot create directory '%s'\n", dst_path);
                                return -1;
                        } else {
                                dfd = open(dst_path, O_RDONLY);
                                if (dfd == -1) {
                                        printf("%s create fail\n", dst_path);
                                        return -1;
                                }
                                else {
                                        printf("%s create succeed\n", dst_path);
                                        close(dfd);
                                } 
                        }
                        printf("abc1\n");
                        if(cp_dir(src_path, dst_path) < 0) {
                                printf("%s fail", src_path);
                                close(sfd);
                                return -1;
                        }
                } else {
                        if(cp_file(src_path, dst_path) < 0) {
                                close(sfd);
                                printf("%s fail", dst_path);
                                return -1;
                        }
                }
                printf("while\n");
    }

        printf("return");
        close(sfd);
        return 0;
}

int cp_file(char *src, char *dst) {
        int sfd, dfd, n;
        char buf[1024];

        sfd = open(src, O_RDONLY);
        if(!sfd) {
                printf(TAG"cannot open '%s': No such file or directory\n", src);
                return -1;
        }

        dfd = open(dst, O_WRONLY | O_CREATE | O_TRUNC);
        if(!dfd) {
                printf(TAG"cannot create '%s'\n", dst);
                close(sfd);
                return -1;
        }

        while((n = read(sfd, buf, sizeof(buf))) > 0) {
                if((n = write(dfd, buf, n)) < 0) {
                        printf(TAG"write '%s' failed\n", dst);
                        close(sfd);
                        close(dfd);
                        return -1;
                }
        }

        return 0;
}

static int cp(char *src, char *dst, int flag)
{
        struct stat st;

        if(stat(src, &st) < 0) {
                printf(TAG"cannot stat '%s': No such file\n", src);
                return -1;
        }

        if(st.type == T_DIR) {
                if(flag) {
                        return cp_dir(src, dst);
                } else {
                        printf(TAG"cannot copy '%s': Is a directory\n", src);
                        return -1;;
                } 
        } else {
                return cp_file(src, dst);
        }
}

int main(int argc, char **argv)
{
        int flag = 0;

        if(argc < 3) {
                printf(TAG"missing operand\n");
                return -1;
        }

        if (strcmp(argv[1], "-r") == 0) {
                flag = 1;
                argv++;
        }

        if(cp(argv[1], argv[2], flag) < 0)
                return -1;
        
        return 0;
}