/* Minimal dynamically linked executable used by ELF interpreter smoke tests. */
#include <stdio.h>

int main(void)
{
	puts("DYNAMIC_HELLO_OK");
	return 0;
}
