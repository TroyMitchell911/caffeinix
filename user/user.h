/*
 * @Author: TroyMitchell
 * @Date: 2024-05-08
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-14
 * @FilePath: /caffeinix/user/user.h
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
/* system calls from usys.S */
int fork(void);
int exit(int) __attribute__((noreturn));
int wait(int*);
int pipe(int*);
int write(int, const void*, int);
int read(int, void*, int);
int close(int);
int kill(int);
int exec(const char*, char**);
int open(const char*, int);
int mknod(const char*, short, short);
int unlink(const char*);
int link(const char*, const char*);
int mkdir(const char*);
int chdir(const char*);
int dup(int);
int getpid(void);
char* sbrk(int);
int sleep(int);
int uptime(void);

/* From ulib.c */
char* strcpy(char*, const char*);
char* strncpy(char*, const char*, unsigned short);
void *memmove(void*, const void*, int);
char* strchr(const char*, char c);
int strcmp(const char*, const char*);
int strncmp(const char *, const char *, unsigned int n);
char* gets(char*, int max);
unsigned int strlen(const char*);
void* memset(void*, int, unsigned int);
int atoi(const char*);
int memcmp(const void *, const void *, unsigned int);
void *memcpy(void *, const void *, unsigned int);

/* From printf.c */
void fprintf(int, const char*, ...);
void printf(const char*, ...);

/* From umalloc.c */
void* malloc(unsigned int);
void free(void*);