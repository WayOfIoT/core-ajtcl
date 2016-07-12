/**
 * @file
 */
/******************************************************************************
 * Copyright AllSeen Alliance. All rights reserved.
 *
 * SPDX-License-Identifier: ISC
 ******************************************************************************/

#include <ajtcl/alljoyn.h>
#include <ajtcl/aj_debug.h>

static const char ServiceName[] = "org.alljoyn.ajlite";

#define CONNECT_TIMEOUT    (1000 * 60)
#define UNMARSHAL_TIMEOUT  (1000 * 5)

int AJ_Main()
{
    AJ_Status status;
    AJ_BusAttachment bus;

    AJ_Initialize();

    status = AJ_FindBusAndConnect(&bus, NULL, CONNECT_TIMEOUT);
    if (status == AJ_OK) {
        status = AJ_BusRequestName(&bus, ServiceName, AJ_NAME_REQ_DO_NOT_QUEUE);
    }
    while (status == AJ_OK) {
        AJ_Message msg;
        status = AJ_UnmarshalMsg(&bus, &msg, UNMARSHAL_TIMEOUT);
        if (status == AJ_OK) {
            status = AJ_BusHandleBusMessage(&msg);
        }
        AJ_CloseMsg(&msg);
    }

    return 0;
}

#ifdef AJ_MAIN
int main()
{
    return AJ_Main();
}
#endif
