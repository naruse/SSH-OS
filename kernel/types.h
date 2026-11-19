/* Fixed-size types for the 68000 (int and long are both 32 bits here). */
#ifndef SSHOS_TYPES_H
#define SSHOS_TYPES_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;
typedef signed char    s8;
typedef signed short   s16;
typedef signed long    s32;
typedef unsigned long  size_t;

#define NULL ((void *)0)

#endif
