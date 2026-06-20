/*
 * irx_imports.h — master import header for the musicstream IRX.
 *
 * The build prepends `#include "irx_imports.h"` to build-imports.c and then
 * appends imports.lst, so every module whose import-table tokens (the
 * NAME_IMPORTS_start / I_func / NAME_IMPORTS_end macros) appear in imports.lst
 * must have its header included here.
 */
#ifndef IRX_IMPORTS_H
#define IRX_IMPORTS_H

#include <irx.h>

#include <sysclib.h>   /* memcpy / memset (IOP is -nostdlib -fno-builtin) */
#include <sifcmd.h>    /* SIF RPC server */
#include <thbase.h>    /* threads */
#include <thsemap.h>   /* semaphores */
#include <libsd.h>     /* SPU2 voices (same driver the SFX use) */
#include <iomanX.h>    /* mass0: file reads */
#include <stdio.h>     /* printf (bring-up logging over ps2link) */

#endif /* IRX_IMPORTS_H */
