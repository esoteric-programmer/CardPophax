/* Minimal stub used only to syntax-check gbcpop.c off-device. */
#ifndef FAKE_PIGPIO_H
#define FAKE_PIGPIO_H
#include <stdint.h>
#include <unistd.h>
#define PI_OUTPUT 1
#define PI_INPUT  0
#define PI_PUD_OFF 0
#define PI_PUD_DOWN 1
#define PI_PUD_UP 2
#define PI_CLOCK_PCM 1
#define PI_WAVE_MODE_ONE_SHOT 0
typedef struct { uint32_t gpioOn, gpioOff, usDelay; } gpioPulse_t;
static inline int gpioCfgClock(unsigned a,unsigned b,unsigned c){(void)a;(void)b;(void)c;return 0;}
static inline int gpioInitialise(void){return 0;}
static inline void gpioTerminate(void){}
static inline int gpioSetMode(unsigned g,unsigned m){(void)g;(void)m;return 0;}
static inline int gpioWrite(unsigned g,unsigned l){(void)g;(void)l;return 0;}
static inline int gpioRead(unsigned g){(void)g;return 0;}
static inline int gpioSetPullUpDown(unsigned g,unsigned p){(void)g;(void)p;return 0;}
typedef void (*gpioAlertFuncEx_t)(int,int,uint32_t,void*);
static inline int gpioSetAlertFuncEx(unsigned g,gpioAlertFuncEx_t f,void*u){(void)g;(void)f;(void)u;return 0;}
static inline int gpioWaveAddNew(void){return 0;}
static inline int gpioWaveAddGeneric(unsigned n,gpioPulse_t*p){(void)n;(void)p;return 0;}
static inline int gpioWaveCreate(void){return 0;}
static inline int gpioWaveDelete(unsigned i){(void)i;return 0;}
static inline int gpioWaveTxSend(unsigned i,unsigned m){(void)i;(void)m;return 0;}
static inline int gpioWaveTxBusy(void){return 0;}
static inline uint32_t gpioTick(void){return 0;}
static inline int gpioDelay(uint32_t u){(void)u;return 0;}
#endif
