#ifndef _AJ_TARGET_NVRAM_H_
#define _AJ_TARGET_NVRAM_H_

/**
 * @file
 */
/******************************************************************************
 *  *    Copyright (c) Open Connectivity Foundation (OCF) and AllJoyn Open
 *    Source Project (AJOSP) Contributors and others.
 *
 *    SPDX-License-Identifier: Apache-2.0
 *
 *    All rights reserved. This program and the accompanying materials are
 *    made available under the terms of the Apache License, Version 2.0
 *    which accompanies this distribution, and is available at
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Copyright (c) Open Connectivity Foundation and Contributors to AllSeen
 *    Alliance. All rights reserved.
 *
 *    Permission to use, copy, modify, and/or distribute this software for
 *    any purpose with or without fee is hereby granted, provided that the
 *    above copyright notice and this permission notice appear in all
 *    copies.
 *
 *     THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 *     WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 *     WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 *     AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 *     DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 *     PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 *     TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 *     PERFORMANCE OF THIS SOFTWARE.
 ******************************************************************************/
#include "alljoyn.h"

/*
 * Identifies an AJ NVRAM block
 */
#define AJ_NV_SENTINEL ('A' | ('J' << 8) | ('N' << 16) | ('V' << 24))
#define INVALID_ID (0)
#define INVALID_DATA (0xFFFF)
#define INVALID_DATA_BYTE (0xFF)
#define SENTINEL_OFFSET (4)
#define WORD_ALIGN(x) ((x & 0x3) ? ((x >> 2) + 1) << 2 : x)
#define AJ_NVRAM_SIZE (2024)

typedef struct _NV_EntryHeader {
    uint16_t id;           /**< The unique id */
    uint16_t capacity;     /**< The data set size */
} NV_EntryHeader;

#define ENTRY_HEADER_SIZE (sizeof(NV_EntryHeader))
#define AJ_NVRAM_END_ADDRESS (AJ_NVRAM_BASE_ADDRESS + AJ_NVRAM_SIZE)

/**
 * Write a block of data to NVRAM
 *
 * @param dest  Pointer a location of NVRAM
 * @param buf   Pointer to data to be written
 * @param size  The number of bytes to be written
 */
void _AJ_NV_Write(void* dest, void* buf, uint16_t size);

/**
 * Read a block of data from NVRAM
 *
 * @param src   Pointer a location of NVRAM
 * @param buf   Pointer to data to be written
 * @param size  The number of bytes to be written
 */
void _AJ_NV_Read(void* src, void* buf, uint16_t size);

/**
 * Erase the whole NVRAM sector and write the sentinel data
 */
void _AJ_NVRAM_Clear();

/**
 * Load NVRAM data from a file
 */
AJ_Status _AJ_LoadNVFromFile();

/**
 * Write NVRAM data to a file for persistent storage
 */
AJ_Status _AJ_StoreNVToFile();

#endif