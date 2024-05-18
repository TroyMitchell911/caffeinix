/*
 * @Author: TroyMitchell
 * @Date: 2024-05-08
 * @LastEditors: GoKo-Son626
 * @LastEditTime: 2024-05-18
 * @FilePath: /caffeinix/user/init.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include "user.h"
#include "fcntl.h"
#include "stat.h"

#define CONSOLE                 1  
#define NULL (void*)            0


int _test_thread(void* arg);

int main(void){
        int ret, fd;
        char buf[128];

        if((fd = open("console", O_RDWR)) == -1) {
                if((mknod("console", 1, 0)) == 0) {
                        fd = open("console", O_RDWR);       
                }
        }
        if(fd != -1)
                fd = dup(fd);

        char str1[] = "Hello, world!";
        char c1 = 'o';  
        char c2 = 'z';  
        
        /* Test for "strchr" */
        // Test to find a character
        char* result1 = strchr(str1, c1);  
        if (result1 != NULL ) {  
                printf("Found '%c' in '%s'\n", c1, str1);  
        } else {  
                printf("'%c' not found in '%s'\n", c1, str1);  
        }
        // Test to find a character that not exit
        char* result2 = strchr(str1, c2);  
        if (result2 != NULL) {  
                printf("Found '%c' in '%s' \n", c2, str1);  
        } else {  
                printf("'%c' not found in '%s'\n", c2, str1);  
        }

        /* Test for atoi */
        char* Num = "66";
        int number = atoi(Num);
        printf("The number value of char \" %s \" is %d \n", Num, number);

        /* Test for memcmp*/
        char* str2 = "Hello";
        char* str3 = "Hello world";
        int result3 = memcmp(str2, str3, strlen(str2));  
        if (result3 == 0) {  
                printf("The first %d bits of str1 and str2 are equal\n", strlen(str2));  
        } else {  
                printf("The first %d bits of str1 and str2 are not equal\n", strlen(str2));  
        }
        int result4 = memcmp(str2, str3, strlen(str3));  
        if (result4 == 0) {  
                printf("The first %d bits of str1 and str2 are equal\n", strlen(str3));  
        } else {  
                printf("The first %d bits of str1 and str2 are not equal\n", strlen(str3));  
        }


        clone(_test_thread, 0, 0, "_test_thread1");
        clone(_test_thread, 0, 0, "_test_thread2");
        clone(_test_thread, 0, 0, "_test_thread3");
        clone(_test_thread, 0, 0, "_test_thread4");
        clone(_test_thread, 0, 0, "_test_thread5");

        ret = fork();
        if(ret == 0)
                printf("child\n");
        else
                printf("parent\n");

        for(;;) {
                if(fd != -1) {
                        ret = read(fd, buf, 128);
                        buf[ret] = '\0';
                        if(ret != 0) {
                                fprintf(fd, "From user: %s", buf);
                        }
                }  
        }

        return 0;
}

int _test_thread(void* arg)
{
        
        while(1) {
              printf("%s\n", (char*)arg);
              sleep(1);  
        }
        return 0;
}