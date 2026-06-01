#ifndef	_UTILS_H
#define	_UTILS_H

extern void delay ( unsigned long);
extern void put32 ( unsigned long, unsigned int );
extern unsigned int get32 ( unsigned long );
extern int get_el ( void );
extern unsigned long get_sp ( void );
extern void set_simd_marker ( unsigned long );
extern unsigned long get_simd_marker ( void );

#endif  /*_UTILS_H */
