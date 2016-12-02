/**
 * @file
 */
/******************************************************************************
 *  * 
 *    Copyright (c) 2016 Open Connectivity Foundation and AllJoyn Open
 *    Source Project Contributors and others.
 *    
 *    All rights reserved. This program and the accompanying materials are
 *    made available under the terms of the Apache License, Version 2.0
 *    which accompanies this distribution, and is available at
 *    http://www.apache.org/licenses/LICENSE-2.0

 ******************************************************************************/

/**
 * Per-module definition of the current module for debug logging.  Must be defined
 * prior to first inclusion of aj_debug.h
 */
#define AJ_MODULE INIT

#include "aj_target.h"
#include "aj_init.h"
#include "aj_nvram.h"
#include "aj_creds.h"
#include "aj_guid.h"
#include "aj_crypto.h"
#include "aj_debug.h"
#include "aj_connect.h"

/**
 * Turn on per-module debug printing by setting this variable to non-zero value
 * (usually in debugger).
 */
#ifndef NDEBUG
uint8_t dbgINIT = 0;
#endif

static uint8_t initialized = FALSE;

void AJ_Initialize(void)
{
    AJ_GUID localGuid;
    if (!initialized) {
        initialized = TRUE;
        AJ_NVRAM_Init();
        /*
         * This will seed the random number generator
         */
        AJ_RandBytes(NULL, 0);
        /*
         * This will initialize credentials if needed
         */
        AJ_GetLocalGUID(&localGuid);

        /*
         * Clear the Routing Node black list
         */
        AJ_InitRoutingNodeBlacklist();

        AJ_InitRoutingNodeResponselist();
    }
}