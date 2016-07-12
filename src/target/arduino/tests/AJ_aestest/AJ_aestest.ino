/**
 * @file
 */
/******************************************************************************
 * Copyright AllSeen Alliance. All rights reserved.
 *
 * SPDX-License-Identifier: ISC
 ******************************************************************************/

#include <stdint.h>
#include <stdarg.h>
//#include <stdio.h>

#include <SPI.h>
#ifdef WIFI_UDP_WORKING
#include <WiFi.h>
#else
#include <Ethernet.h>
#endif
#include <alljoyn.h>

int AJ_Main(void);

void setup() {
    //Initialize serial and wait for port to open:
    Serial.begin(115200);
    while (!Serial) {
        ; // wait for serial port to connect. Needed for Leonardo only
    }
}


void loop() {
    AJ_Main();
}

