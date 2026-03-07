/*---------------------------------------------------------------------------/
/  FatFs Configuration — wili8jam SD card
/---------------------------------------------------------------------------*/

#define FFCONF_DEF	80286	/* Revision ID (must match ff.h) */

/*---------------------------------------------------------------------------/
/ Function Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_READONLY	0
#define FF_FS_MINIMIZE	0
#define FF_USE_FIND		0
#define FF_USE_MKFS		0
#define FF_USE_FASTSEEK	0
#define FF_USE_EXPAND	0
#define FF_USE_CHMOD	0
#define FF_USE_LABEL	0
#define FF_USE_FORWARD	0
#define FF_USE_STRFUNC	0
#define FF_PRINT_LLI	0
#define FF_PRINT_FLOAT	0
#define FF_STRF_ENCODE	0

/*---------------------------------------------------------------------------/
/ Locale and Namespace Configurations
/---------------------------------------------------------------------------*/

#define FF_CODE_PAGE	437

#define FF_USE_LFN		1
#define FF_MAX_LFN		255

#define FF_LFN_UNICODE	0
/* 0: ANSI/OEM in current CP (TCHAR = char) — simplest for our use */

#define FF_LFN_BUF		255
#define FF_SFN_BUF		12

#define FF_FS_RPATH		2
/* 2: Enable relative path with f_getcwd() */

/*---------------------------------------------------------------------------/
/ Drive/Volume Configurations
/---------------------------------------------------------------------------*/

#define FF_VOLUMES		1

#define FF_STR_VOLUME_ID	0
#define FF_VOLUME_STRS		"SD"

#define FF_MULTI_PARTITION	0

#define FF_MIN_SS		512
#define FF_MAX_SS		512
/* SD cards use 512-byte sectors */

#define FF_LBA64		0

#define FF_MIN_GPT		0x10000000

#define FF_USE_TRIM		0

/*---------------------------------------------------------------------------/
/ System Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_TINY		0

#define FF_FS_EXFAT		0

#define FF_FS_NORTC		1
#define FF_NORTC_MON	1
#define FF_NORTC_MDAY	1
#define FF_NORTC_YEAR	2026
/* No RTC on Fruit Jam — use fixed timestamp */

#define FF_FS_NOFSINFO	0

#define FF_FS_LOCK		0

#define FF_FS_REENTRANT	0
#define FF_FS_TIMEOUT	1000

/*--- End of configuration options ---*/
