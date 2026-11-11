#ifndef GROVE_SD_DEMO_H
#define GROVE_SD_DEMO_H

#ifndef GROVE_DEMO
#define GROVE_DEMO 1
#endif

#define GROVE_STR_(x) #x
#define GROVE_STR(x) GROVE_STR_(x)
#define GROVE_DEMO_STR GROVE_STR(GROVE_DEMO)

int app_main(void);

#endif
