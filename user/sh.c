/*
 * @Author: TroyMitchell
 * @Date: 2024-05-20
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-22
 * @FilePath: /caffeinix/user/sh.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include "user.h"
#include "fcntl.h"

#define SH_BUF_MAX                      128

#define MAXARGS                         10

typedef enum type{
        EXEC = 1,
        REDIR,
        PIPE,
        LIST,
        BACK
}type_t;

typedef struct cmd {
        type_t type;
}*cmd_t;

typedef struct execcmd {
        type_t type;
        char *argv[MAXARGS];
        char *eargv[MAXARGS];
}*execcmd_t;

static const char whitespace[] = " \t\r\n\v";
static const char symbols[] = "<|>&;()";

static void panic(char *s)
{
        fprintf(2, "%s\n", s);
        exit(1);
}

static cmd_t nulterminate(cmd_t cmd)
{
        int i;
        execcmd_t execcmd;
        if(cmd == 0)
                return 0;
        switch((int)cmd->type) {
                case EXEC:
                        execcmd = (execcmd_t)cmd;
                        for(i = 0; execcmd->eargv[i]; i++)
                                *execcmd->eargv[i] = 0;
                        break;
        }
                
        return cmd;
}

/**
 * @description: Alloc a structure of execcmd
 * @return {*} a structure of execcmd
 */
static cmd_t execcmd(void)
{
        execcmd_t cmd;

        cmd = malloc(sizeof(struct execcmd));
        if(!cmd)
                return 0;
        memset(cmd, 0, sizeof(struct execcmd));
        cmd->type = EXEC;
        return (cmd_t)cmd;
}

int gettoken(char **ps, char *es, char **q, char **eq)
{
        char *s;
        int ret;

        /* Skip the white space */
        s = *ps;
        while(s < es && strchr(whitespace, *s)) s++;
        /* Save the beginning address of token */
        if(q)
                *q = s;
        /* Save the first character */
        ret = *s;

        switch(*s) {
                /* ending of string */
                case 0:
                        break;
                /* Special character */
                case '|':
                case '(':
                case ')':
                case ';':
                case '&':
                /* Redirection character */
                case '<':
                        s++;
                        break;
                /* Redirection character */
                case '>':
                        s++;
                        if(*s == '>') {
                                ret = '+';
                                s++;
                        }
                        break;
                /* ASCII character */
                default:
                        ret = 'a';
                        while(s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
                                s++;
                        break;
        }
        /* Save the ending address of token */
        if(eq)
                *eq = s;
        
        /* Skip white space */
        while(s < es && strchr(whitespace, *s)) s++;

        /* Save the where current pointer 's' points */
        *ps = s;

        return ret;
}

static cmd_t parseexec(char **ps, char *es)
{
        char *q, *eq;
        execcmd_t cmd;
        int tok, argc = 0;
        cmd = (execcmd_t)execcmd();
        if(!cmd)
                return 0;

        /* TODO: redirection */

        /* TODO: parse other character */
        while(1) {
                tok = gettoken(ps, es, &q, &eq);
                if(tok == 0) {
                        break;
                }

                if(tok != 'a') {
                        panic("syntax");
                }

                cmd->argv[argc] = q;
                cmd->eargv[argc++] = eq;
                /* Reserve the position of zero */
                if(argc >= MAXARGS - 1) 
                        panic("too many args");

                /* TODO: redirection */
        }

        cmd->argv[argc] = 0;
        cmd->eargv[argc] = 0;

        return (cmd_t)cmd;
}

static cmd_t parsepipe(char **ps, char *es)
{
        cmd_t cmd;
        cmd = parseexec(ps, es);
        /* TODO: parse pipe */
        return cmd;
}

static cmd_t parseline(char **ps, char *es)
{
        cmd_t cmd;
        cmd = parsepipe(ps, es);
        /* TODO: parse && and ; */

        return cmd;
}

static cmd_t parsecmd(char* s)
{
        char *es;
        cmd_t cmd;
        es = s + strlen(s);
        cmd = parseline(&s, es);
        /* TODO: Judge the 's' points ending of string */

        nulterminate(cmd);
        return cmd;
}

static void runcmd(cmd_t cmd)
{
        execcmd_t ecmd;

        if(cmd == 0)
                exit(1);

        switch((int)cmd->type) {
                case EXEC:
                        ecmd = (execcmd_t)cmd;
                        if(ecmd->argv[0] == 0)
                                exit(1);
                        exec(ecmd->argv[0], ecmd->argv);
                        fprintf(2, "exec %s failed\n", ecmd->argv[0]);
                        break;
        }
}
static int getcmd(char* buf, int max)
{
        char cwd[1024];
        if(getcwd(cwd, 1024)) {
                return -1;
        }
        printf("%s$ ", cwd);
        gets(buf, max);
        if(buf[0] == 0)
                return -1;
        return 0;
}

int main(void)
{
        static char buf[SH_BUF_MAX];
        int fd, ret;

        while((fd = open("console", O_RDWR)) >= 0) {
                if(fd == 3) {
                        close(fd);
                        break;
                }
        }
        if(fd != 3) {
                printf("sh failed: fd->%d\n", fd);
                exit(-1);
        }

        while(getcmd(buf, SH_BUF_MAX) >= 0) {
                if(buf[0] == 'c' && buf[1] == 'd' && buf[2] == ' ') {
                        /* Delete '\n' */
                        buf[strlen(buf) - 1] = '\0';

                        if(chdir(buf + 3) < 0)
                                fprintf(2, "cd: %s: No such file or directory\n", buf + 3);

                        continue;
                }
                ret = fork();
                if(ret == 0) {
                        runcmd(parsecmd(buf));
                } else if(ret == -1) {
                        panic("fork failed\n");
                } else {
                        wait(0);
                }   
        }

        exit(-1);
        return 0;
}