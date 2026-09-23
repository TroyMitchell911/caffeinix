/*
 * Freestanding byte, string, and small-array helpers. These operate on
 * kernel-accessible memory, not user pointers, and provide no allocation or
 * synchronization. Keep this subset usable before the allocator and scheduler
 * exist.
 *
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved.
 */
#include <mystring.h>

/**
 * memset() - Fill a byte range
 * @dst: Writable kernel range of at least @n bytes.
 * @c: Value converted to a byte.
 * @n: Number of bytes to write.
 *
 * Context: Any context; no sleeping or internal locking.
 * Return: @dst.
 */
void* memset(void* dst, int c, size_t n)
{
        char* d = (char*)dst;
	size_t i;
        
        for(i = 0; i < n; i++) {
                d[i] = c;
        }

        return dst;
}

/**
 * strlen() - Measure a terminated kernel string
 * @s: Readable NUL-terminated string.
 *
 * Context: Any context; @s must remain stable.
 * Return: Bytes preceding the terminator.
 */
size_t strlen(const char* s)
{
        char* p = (char*)s;
        while((*p++) != '\0');
        return (p - s - 1);
}

/**
 * strcpy() - Copy a terminated string
 * @s: Destination with room for the string and terminator.
 * @t: Stable NUL-terminated source; must not overlap @s.
 *
 * Context: Any context; caller serializes destination access.
 * Return: Original destination pointer.
 */
char* strcpy(char* s, const char* t)
{
	char *original = s;

	while ((*s++ = *t++))
		;
	return original;
}

/**
 * strncpy() - Copy and pad a string known to fit
 * @s: Writable destination of at least @n bytes.
 * @t: NUL-terminated source whose terminator is within @n bytes.
 * @n: Destination extent; must exceed the source string length.
 *
 * This legacy implementation decrements an unsigned count in both loops.
 * Exhausting the copy count before seeing NUL underflows that count before
 * padding; it is not safe for truncation or a zero limit. Use safe_strncpy()
 * for bounded kernel names.
 *
 * Context: Any context; caller serializes destination access.
 * Return: Original destination pointer.
 */
char* strncpy(char* s, const char* t, size_t n)
{
        char *os;       

        os = s;
        while(n-- > 0 && (*s++ = *t++) != 0)
                ;
        while(n-- > 0)
                *s++ = 0;
        return os;  
}

/**
 * safe_strncpy() - Copy a bounded string with termination
 * @s: Writable destination of @n bytes, unused when @n is zero.
 * @t: Readable source up to NUL or @n - 1 bytes.
 * @n: Destination capacity including the terminator.
 *
 * Writes a terminator whenever @n is nonzero. Bytes beyond that terminator
 * are not padded.
 *
 * Context: Any context; source and destination must not overlap.
 * Return: Original destination pointer.
 */
char* safe_strncpy(char* s, const char* t, size_t n)
{
        char *os;

        os = s;
        if(n <= 0)
                return os;
        while(--n > 0 && (*s++ = *t++) != 0)
                ;
        *s = 0;
        return os;  
}

/**
 * memmove() - Copy bytes allowing overlap
 * @dst: Writable kernel range of @n bytes.
 * @src: Readable kernel range of @n bytes.
 * @n: Byte count; zero leaves both ranges untouched.
 *
 * Context: Any context; caller serializes concurrent access.
 * Return: @dst.
 */
void* memmove(void *dst, const void *src, size_t n)
{
        const char *s;
        char *d;

        if(n == 0)
                return dst;

        s = src;
        d = dst;
        if(s < d && s + n > d){
                s += n;
                d += n;
                while(n-- > 0)
                *--d = *--s;
        } else
                while(n-- > 0)
                        *d++ = *s++;

        return dst;
}

/**
 * memcpy() - Copy a kernel byte range
 * @dst: Writable kernel range of @n bytes.
 * @src: Readable kernel range of @n bytes.
 * @n: Number of bytes.
 *
 * The current implementation delegates to memmove(), including its overlap
 * handling; callers should not require overlap support from the memcpy
 * interface.
 *
 * Context: Any context; no sleeping or internal locking.
 * Return: @dst.
 */
void* memcpy(void* dst, const void* src, size_t n)
{
        return memmove(dst, src, n);
}

/**
 * memchr() - Find a byte in a bounded range
 * @buffer: Readable range of @n bytes.
 * @character: Value compared after conversion to an unsigned byte.
 * @n: Maximum bytes examined.
 *
 * Context: Any context; the range must remain stable.
 * Return: Pointer to the first matching byte, or NULL.
 */
void* memchr(const void *buffer, int character, size_t n)
{
	const uint8 *bytes = buffer;

	while (n--) {
		if (*bytes == (uint8)character)
			return (void *)bytes;
		bytes++;
	}
	return 0;
}

/**
 * memcmp() - Compare two byte ranges
 * @left: First readable range of @n bytes.
 * @right: Second readable range of @n bytes.
 * @n: Byte count.
 *
 * Context: Any context; both ranges must remain stable.
 * Return: Zero if equal, otherwise the first unsigned-byte difference.
 */
int memcmp(const void *left, const void *right, size_t n)
{
	const uint8 *p = left;
	const uint8 *q = right;

	while (n--) {
		if (*p != *q)
			return *p - *q;
		p++;
		q++;
	}
	return 0;
}

/**
 * strcmp() - Compare terminated strings
 * @p: First NUL-terminated string.
 * @q: Second NUL-terminated string.
 *
 * Context: Any context; strings must remain stable.
 * Return: Zero if equal, otherwise the first unsigned-byte difference.
 */
int strcmp(const char *p, const char *q)
{
	while (*p && *p == *q) {
		p++;
		q++;
	}
	return (uint8)*p - (uint8)*q;
}

/**
 * strncmp() - Compare strings up to a byte limit
 * @p: First string, readable up to NUL or @n bytes.
 * @q: Second string, readable up to NUL or @n bytes.
 * @n: Maximum compared bytes.
 *
 * Context: Any context; strings must remain stable.
 * Return: Zero if equal within the limit, otherwise the differing-byte delta.
 */
int strncmp(const char *p, const char *q, size_t n)
{
        while(n > 0 && *p && *p == *q)
                n--, p++, q++;
        if(n == 0)
                return 0;
        return (uint8)*p - (uint8)*q;
}

/**
 * strcat() - Append a terminated string
 * @p: Terminated destination with room for both strings and NUL.
 * @q: Stable terminated source, not overlapping the destination.
 *
 * Context: Any context; caller serializes destination access.
 * Return: Original destination pointer.
 */
char* strcat(char *p, const char *q)
{
	char *original = p;

        while(*p != '\0') p++;
        while(*q != '\0') *p++ = *q++;
        *p = '\0';
	return original;
}

/**
 * strchr() - Find the first matching character
 * @p: Readable NUL-terminated string.
 * @c: Character value; zero searches for the terminator.
 *
 * Context: Any context; string must remain stable.
 * Return: Matching pointer or NULL.
 */
char* strchr(const char *p, int c)
{
	for (;; p++) {
		if (*p == c)
			return (char *)p;
		if (!*p)
			return 0;
	}
}

/**
 * strrchr() - Find the last matching character
 * @p: Readable NUL-terminated string.
 * @c: Character value; zero searches for the terminator.
 *
 * Context: Any context; string must remain stable.
 * Return: Matching pointer or NULL.
 */
char* strrchr(const char *p, int c)
{
        const char *p_start = p;

        for(; *p != '\0'; p++);
        for(; p > p_start && *p != c; p--);

	return p == p_start && *p != c ? 0 : (char*)p;
}

/**
 * strnlen() - Measure a possibly unterminated string
 * @s: Range readable up to NUL or @max bytes.
 * @max: Maximum inspected length in bytes.
 *
 * Context: Any context; range must remain stable.
 * Return: Length before NUL, or @max when none occurs within the bound.
 */
size_t strnlen(const char *s, size_t max)
{
	size_t length = 0;

	while (length < max && s[length])
		length++;
	return length;
}

/*
 * Exchange equally sized nonoverlapping element representations for the
 * insertion-sort loop; the caller owns both ranges.
 */
static void byte_swap(uint8 *left, uint8 *right, size_t size)
{
	while (size--) {
		uint8 value = *left;

		*left++ = *right;
		*right++ = value;
	}
}

/**
 * qsort() - Sort a small kernel array in place
 * @base: Array storage; NULL is treated as an empty operation.
 * @count: Number of elements.
 * @size: Bytes per element; zero is an empty operation.
 * @compare: Ordering callback returning negative, zero, or positive.
 *
 * Uses insertion sort, not quicksort: quadratic comparisons and byte swaps
 * make this suitable for small arrays. Storage and count-times-size
 * arithmetic must be valid; the callback receives pointers to elements, not
 * copies.
 *
 * Context: No internal sleeping or locking; callback must suit caller
 *          context.
 */
void qsort(void *base, size_t count, size_t size,
	   int (*compare)(const void *, const void *))
{
	uint8 *array = base;
	size_t i, j;

	if (!array || !size || count < 2)
		return;
	for (i = 1; i < count; i++) {
		for (j = i; j &&
		     compare(array + (j - 1) * size,
		             array + j * size) > 0; j--)
			byte_swap(array + (j - 1) * size,
			          array + j * size, size);
	}
}

/**
 * atoi() - Parse a small signed decimal value
 * @string: NUL-terminated input, optionally space/tab and a sign.
 *
 * Stops at the first nondigit. There is no overflow or error reporting;
 * callers must constrain the input to the int range.
 *
 * Context: Any context; input must remain stable.
 * Return: Parsed signed value, or zero if no digits are present.
 */
int atoi(const char *string)
{
	int negative = 0, value = 0;

	while (*string == ' ' || *string == '\t')
		string++;
	if (*string == '-' || *string == '+')
		negative = *string++ == '-';
	while (*string >= '0' && *string <= '9')
		value = value * 10 + *string++ - '0';
	return negative ? -value : value;
}
