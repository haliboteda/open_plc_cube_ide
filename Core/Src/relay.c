
// relay.c
#include "relay.h"

// Interne Zuordnung von Ports und Pins zu jedem HSFET
static GPIO_TypeDef* relay_ports[RELAY_COUNT] = {GPIOI, GPIOI, GPIOI, GPIOG, GPIOG, GPIOD};
static const uint16_t relay_pins[RELAY_COUNT] = {RY1_Pin, RY2_Pin, RY3_Pin, RY4_Pin, RY5_Pin, RY6_Pin};


void Relay_Init(void) {
    // Die Initialisierung der Ports und Pins erfolgt bereits in MX_GPIO_Init
}

void Relay_On(RELAY_Name relay) {
    if(relay < RELAY_COUNT) {
        HAL_GPIO_WritePin(relay_ports[relay], relay_pins[relay], GPIO_PIN_SET);
    }
}

void Relay_Off(RELAY_Name relay) {
    if(relay < RELAY_COUNT) {
        HAL_GPIO_WritePin(relay_ports[relay], relay_pins[relay], GPIO_PIN_RESET);
    }
}

void Relay_Toggle(RELAY_Name relay) {
    if(relay < RELAY_COUNT) {
    	HAL_GPIO_TogglePin(relay_ports[relay], relay_pins[relay]);
    }
}
