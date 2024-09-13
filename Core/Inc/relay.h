
// relay.h
#ifndef RELAY_H
#define RELAY_H

#include "main.h"

typedef enum {
  RELAY_1 = 0,
  RELAY_2,
  RELAY_3,
  RELAY_4,
  RELAY_5,
  RELAY_6,
  RELAY_COUNT
} RELAY_Name;

void Relay_Init(void);
void Relay_On(RELAY_Name relay);
void Relay_Off(RELAY_Name relay);
void Relay_Toggle(RELAY_Name relay);

#endif // RELAY_H

