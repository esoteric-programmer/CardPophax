/* Minimal stub used only to compile-check the wiringPi backend off-device. */
#ifndef FAKE_WIRINGPI_H
#define FAKE_WIRINGPI_H
#define INPUT 0
#define OUTPUT 1
#define LOW 0
#define HIGH 1
#define PUD_OFF 0
#define PUD_DOWN 1
#define PUD_UP 2
static inline int  wiringPiSetupGpio(void){return 0;}
static inline void pinMode(int p,int m){(void)p;(void)m;}
static inline void pullUpDnControl(int p,int m){(void)p;(void)m;}
static inline void digitalWrite(int p,int v){(void)p;(void)v;}
static inline int  digitalRead(int p){(void)p;return 0;}
static inline unsigned int micros(void){return 0;}
#endif
