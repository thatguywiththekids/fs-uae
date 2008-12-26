#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x500

#include "sysconfig.h"
#include "sysdeps.h"

#include <shellapi.h>

#include "resource"

#include "threaddep/thread.h"
#include "options.h"
#include "filesys.h"
#include "blkdev.h"
#include "registry.h"
#include "win32gui.h"
#include "zfile.h"

#define hfd_log write_log

#ifdef WINDDK
#include <devioctl.h>
#include <ntddstor.h>
#include <winioctl.h>
#include <initguid.h>   // Guid definition
#include <devguid.h>    // Device guids
#include <setupapi.h>   // for SetupDiXxx functions.
#include <cfgmgr32.h>   // for SetupDiXxx functions.
#endif
#include <stddef.h>

static int usefloppydrives = 0;

struct uae_driveinfo {
    char vendor_id[128];
    char product_id[128];
    char product_rev[128];
    char product_serial[128];
    char device_name[2048];
    char device_path[2048];
    uae_u64 size;
    uae_u64 offset;
    int bytespersector;
    int removablemedia;
    int nomedia;
    int dangerous;
    int readonly;
};

#define HDF_HANDLE_WIN32 1
#define HDF_HANDLE_ZFILE 2

#define CACHE_SIZE 16384
#define CACHE_FLUSH_TIME 5

/* safety check: only accept drives that:
 * - contain RDSK in block 0
 * - block 0 is zeroed
 */

int harddrive_dangerous, do_rdbdump;
static struct uae_driveinfo uae_drives[MAX_FILESYSTEM_UNITS];

static int isnomediaerr (DWORD err)
{
    if (err == ERROR_NOT_READY ||
	err == ERROR_MEDIA_CHANGED ||
	err == ERROR_NO_MEDIA_IN_DRIVE ||
	err == ERROR_DEV_NOT_EXIST ||
	err == ERROR_BAD_NET_NAME ||
	err == ERROR_WRONG_DISK)
	return 1;
    return 0;
}

static void rdbdump (HANDLE *h, uae_u64 offset, uae_u8 *buf, int blocksize)
{
    static int cnt = 1;
    int i, blocks;
    char name[100];
    FILE *f;

    blocks = (buf[132] << 24) | (buf[133] << 16) | (buf[134] << 8) | (buf[135] << 0);
    if (blocks < 0 || blocks > 100000)
	return;
    sprintf (name, "rdb_dump_%d.rdb", cnt);
    f = fopen (name, "wb");
    if (!f)
	return;
    for (i = 0; i <= blocks; i++) {
	DWORD outlen, high;
	high = (DWORD)(offset >> 32);
	if (SetFilePointer (h, (DWORD)offset, &high, FILE_BEGIN) == INVALID_FILE_SIZE)
	    break;
	ReadFile (h, buf, blocksize, &outlen, NULL);
	fwrite (buf, 1, blocksize, f);
	offset += blocksize;
    }
    fclose (f);
    cnt++;
}

#define CA "Commodore\0Amiga\0"
static int safetycheck (HANDLE *h, uae_u64 offset, uae_u8 *buf, int blocksize)
{
    int i, j, blocks = 63, empty = 1;
    DWORD outlen, high;

    for (j = 0; j < blocks; j++) {
	high = (DWORD)(offset >> 32);
	if (SetFilePointer (h, (DWORD)offset, &high, FILE_BEGIN) == INVALID_FILE_SIZE) {
	    write_log ("hd ignored, SetFilePointer failed, error %d\n", GetLastError());
	    return 1;
	}
	memset (buf, 0xaa, blocksize);
	ReadFile (h, buf, blocksize, &outlen, NULL);
	if (outlen != blocksize) {
	    write_log ("hd ignored, read error %d!\n", GetLastError());
	    return 2;
	}
	if (!memcmp (buf, "RDSK", 4)) {
	    if (do_rdbdump)
		rdbdump (h, offset, buf, blocksize);
	    write_log ("hd accepted (rdb detected at block %d)\n", j);
	    return -1;
	}

	if (!memcmp (buf + 2, "CIS@", 4) && !memcmp (buf + 16, CA, strlen (CA))) {
	    write_log ("hd accepted (PCMCIA RAM)\n");
	    return -2;
	}
	if (j == 0) {
	    for (i = 0; i < blocksize; i++) {
		if (buf[i])
		    empty = 0;
	    }
	}
	offset += blocksize;
    }
    if (!empty) {
	write_log ("hd ignored, not empty and no RDB detected\n");
	return 0;
    }
    write_log ("hd accepted (empty)\n");
    return -9;
}


static void trim (char *s)
{
    while(strlen(s) > 0 && s[strlen(s) - 1] == ' ')
	s[strlen(s) - 1] = 0;
}

int isharddrive (const char *name)
{
    int i;

    for (i = 0; i < hdf_getnumharddrives (); i++) {
	if (!strcmp (uae_drives[i].device_name, name))
	    return i;
    }
    return -1;
}

static char *hdz[] = { "hdz", "zip", "rar", "7z", NULL };

int hdf_open_target (struct hardfiledata *hfd, const char *pname)
{
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD flags;
    int i;
    struct uae_driveinfo *udi;
    char *name = my_strdup (pname);

    hfd->flags = 0;
    hfd->drive_empty = 0;
    hdf_close (hfd);
    hfd->cache = VirtualAlloc (NULL, CACHE_SIZE, MEM_COMMIT, PAGE_READWRITE);
    hfd->cache_valid = 0;
    hfd->virtual_size = 0;
    hfd->virtual_rdb = NULL;
    if (!hfd->cache) {
	write_log ("VirtualAlloc(%d) failed, error %d\n", CACHE_SIZE, GetLastError());
	goto end;
    }
    hfd_log ("hfd open: '%s'\n", name);
    if (strlen (name) > 4 && !memcmp (name, "HD_", 3)) {
	hdf_init_target ();
	i = isharddrive (name);
	if (i >= 0) {
	    DWORD r;
	    udi = &uae_drives[i];
	    hfd->flags = HFD_FLAGS_REALDRIVE;
	    if (udi->nomedia)
		hfd->drive_empty = -1;
	    if (udi->readonly)
		hfd->readonly = 1;
	    flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS;
	    h = CreateFile (udi->device_path,
		GENERIC_READ | (hfd->readonly ? 0 : GENERIC_WRITE),
		FILE_SHARE_READ | (hfd->readonly ? 0 : FILE_SHARE_WRITE),
		NULL, OPEN_EXISTING, flags, NULL);
	    hfd->handle = h;
	    if (h == INVALID_HANDLE_VALUE)
		goto end;
	    if (!DeviceIoControl(h, FSCTL_ALLOW_EXTENDED_DASD_IO, NULL, 0, NULL, 0, &r, NULL))
		write_log ("WARNING: '%s' FSCTL_ALLOW_EXTENDED_DASD_IO returned %d\n", name, GetLastError());
	    strncpy (hfd->vendor_id, udi->vendor_id, 8);
	    strncpy (hfd->product_id, udi->product_id, 16);
	    strncpy (hfd->product_rev, udi->product_rev, 4);
	    hfd->offset = udi->offset;
	    hfd->physsize = hfd->virtsize = udi->size;
	    hfd->blocksize = udi->bytespersector;
	    if (hfd->offset == 0 && !hfd->drive_empty) {
		int sf = safetycheck (hfd->handle, 0, hfd->cache, hfd->blocksize);
		if (sf > 0)
		    goto end;
		if (sf == 0 && hfd->warned >= 0) {
		    if (harddrive_dangerous != 0x1234dead) {
			if (!hfd->warned)
			    gui_message_id (IDS_HARDDRIVESAFETYWARNING1);
			hfd->warned = 1;
			goto end;
		    }
		    if (!hfd->warned) {
			gui_message_id (IDS_HARDDRIVESAFETYWARNING2);
			hfd->warned = 1;
		    }
		}
	    } else {
		hfd->warned = -1;
	    }
	    hfd->handle_valid = HDF_HANDLE_WIN32;
	    hfd->emptyname = my_strdup (name);
	} else {
	    hfd->flags = HFD_FLAGS_REALDRIVE;
	    hfd->drive_empty = -1;
	    hfd->emptyname = my_strdup (name);
	}
    } else {
	int zmode = 0;
	char *ext = strrchr (name, '.');
	if (ext != NULL) {
	    ext++;
	    for (i = 0; hdz[i]; i++) {
		if (!stricmp (ext, hdz[i]))
		    zmode = 1;
	    }
	}
	h = CreateFile (name, GENERIC_READ | (hfd->readonly ? 0 : GENERIC_WRITE), hfd->readonly ? FILE_SHARE_READ : 0, NULL,
	    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL);
	hfd->handle = h;
	i = strlen (name) - 1;
	while (i >= 0) {
	    if ((i > 0 && (name[i - 1] == '/' || name[i - 1] == '\\')) || i == 0) {
		strcpy (hfd->vendor_id, "UAE");
		strncpy (hfd->product_id, name + i, 15);
		strcpy (hfd->product_rev, "0.3");
		break;
	    }
	    i--;
	}
	if (h != INVALID_HANDLE_VALUE) {
	    DWORD ret, low, high;
	    high = 0;
	    ret = SetFilePointer (h, 0, &high, FILE_END);
	    if (ret == INVALID_FILE_SIZE && GetLastError() != NO_ERROR)
		goto end;
	    low = GetFileSize (h, &high);
	    if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR)
		goto end;
	    low &= ~(hfd->blocksize - 1);
	    hfd->physsize = hfd->virtsize = ((uae_u64)high << 32) | low;
	    hfd->handle_valid = HDF_HANDLE_WIN32;
	    if (hfd->physsize < 64 * 1024 * 1024 && zmode) {
		write_log ("HDF '%s' re-opened in zfile-mode\n", name);
		CloseHandle (h);
		hfd->handle = h = zfile_fopen(name, hfd->readonly ? "rb" : "r+b");
		if (!h)
		    goto end;
		zfile_fseek (h, 0, SEEK_END);
		hfd->physsize = hfd->virtsize = zfile_ftell (h);
		zfile_fseek (h, 0, SEEK_SET);
		hfd->handle_valid = HDF_HANDLE_ZFILE;
	    }
	} else {
	    write_log ("HDF '%s' failed to open. error = %d\n", name, GetLastError ());
	}
    }
    hfd->handle = h;
    if (hfd->handle_valid || hfd->drive_empty) {
	hfd_log ("HDF '%s' opened, size=%dK mode=%d empty=%d\n",
	    name, hfd->physsize / 1024, hfd->handle_valid, hfd->drive_empty);
	return 1;
    }
end:
    hdf_close (hfd);
    xfree (name);
    return 0;
}

void hdf_close_target (struct hardfiledata *hfd)
{
    if (hfd->handle_valid) {
	if (hfd->handle && hfd->handle != INVALID_HANDLE_VALUE) {
	    if (hfd->handle_valid == HDF_HANDLE_WIN32)
		CloseHandle (hfd->handle);
	    else if(hfd->handle_valid == HDF_HANDLE_ZFILE)
		zfile_fclose (hfd->handle);
	}
    }
    xfree (hfd->emptyname);
    hfd->emptyname = NULL;
    hfd->handle = 0;
    hfd->handle_valid = 0;
    if (hfd->cache)
	VirtualFree (hfd->cache, 0, MEM_RELEASE);
    xfree(hfd->virtual_rdb);
    hfd->virtual_rdb = 0;
    hfd->virtual_size = 0;
    hfd->cache = 0;
    hfd->cache_valid = 0;
    hfd->drive_empty = 0;
}

int hdf_dup_target (struct hardfiledata *dhfd, const struct hardfiledata *shfd)
{
    if (!shfd->handle_valid)
	return 0;
    if (shfd->handle_valid == HDF_HANDLE_WIN32) {
	HANDLE duphandle;
	if (!DuplicateHandle (GetCurrentProcess(), shfd->handle, GetCurrentProcess() , &duphandle, 0, FALSE, DUPLICATE_SAME_ACCESS))
	    return 0;
	dhfd->handle = duphandle;
	dhfd->handle_valid = HDF_HANDLE_WIN32;
    } else if (shfd->handle_valid == HDF_HANDLE_ZFILE) {
	struct zfile *zf;
	zf = zfile_dup (shfd->handle);
	if (!zf)
	    return 0;
	dhfd->handle = zf;
	dhfd->handle_valid = HDF_HANDLE_ZFILE;
    }
    dhfd->cache = VirtualAlloc (NULL, CACHE_SIZE, MEM_COMMIT, PAGE_READWRITE);
    dhfd->cache_valid = 0;
    if (!dhfd->cache) {
	hdf_close (dhfd);
	return 0;
    }
    return 1;
}

static int hdf_seek (struct hardfiledata *hfd, uae_u64 offset)
{
    DWORD high, ret;

    if (hfd->handle_valid == 0) {
	gui_message ("hd: hdf handle is not valid. bug.");
	abort();
    }
    if (offset >= hfd->physsize - hfd->virtual_size) {
	gui_message ("hd: tried to seek out of bounds! (%I64X >= %I64X)\n", offset, hfd->physsize);
	abort ();
    }
    offset += hfd->offset;
    if (offset & (hfd->blocksize - 1)) {
	gui_message ("hd: poscheck failed, offset not aligned to blocksize! (%I64X & %04X = %04X)\n",
	    offset, hfd->blocksize, offset & (hfd->blocksize - 1));
	abort ();
    }
    if (hfd->handle_valid == HDF_HANDLE_WIN32) {
	high = (DWORD)(offset >> 32);
	ret = SetFilePointer (hfd->handle, (DWORD)offset, &high, FILE_BEGIN);
	if (ret == INVALID_FILE_SIZE && GetLastError() != NO_ERROR)
	    return -1;
    } else if (hfd->handle_valid == HDF_HANDLE_ZFILE) {
	zfile_fseek (hfd->handle, (long)offset, SEEK_SET);
    }
    return 0;
}

static void poscheck (struct hardfiledata *hfd, int len)
{
    DWORD high, ret, err;
    uae_u64 pos;

    if (hfd->handle_valid == HDF_HANDLE_WIN32) {
	high = 0;
	ret = SetFilePointer (hfd->handle, 0, &high, FILE_CURRENT);
	err = GetLastError ();
	if (ret == INVALID_FILE_SIZE && err != NO_ERROR) {
	    gui_message ("hd: poscheck failed. seek failure, error %d", err);
	    abort ();
	}
	pos = ((uae_u64)high) << 32 | ret;
    } else if (hfd->handle_valid == HDF_HANDLE_ZFILE) {
	pos = zfile_ftell (hfd->handle);
    }
    if (len < 0) {
	gui_message ("hd: poscheck failed, negative length! (%d)", len);
	abort ();
    }
    if (pos < hfd->offset) {
	gui_message ("hd: poscheck failed, offset out of bounds! (%I64d < %I64d)", pos, hfd->offset);
	abort ();
    }
    if (pos >= hfd->offset + hfd->physsize - hfd->virtual_size || pos >= hfd->offset + hfd->physsize + len - hfd->virtual_size) {
	gui_message ("hd: poscheck failed, offset out of bounds! (%I64d >= %I64d, LEN=%d)", pos, hfd->offset + hfd->physsize, len);
	abort ();
    }
    if (pos & (hfd->blocksize - 1)) {
	gui_message ("hd: poscheck failed, offset not aligned to blocksize! (%I64X & %04X = %04X\n", pos, hfd->blocksize, pos & hfd->blocksize);
	abort ();
    }
}

static int isincache (struct hardfiledata *hfd, uae_u64 offset, int len)
{
    if (!hfd->cache_valid)
	return -1;
    if (offset >= hfd->cache_offset && offset + len <= hfd->cache_offset + CACHE_SIZE)
	return (int)(offset - hfd->cache_offset);
    return -1;
}

#if 0
void hfd_flush_cache (struct hardfiledata *hfd, int now)
{
    DWORD outlen = 0;
    if (!hfd->cache_needs_flush || !hfd->cache_valid)
	return;
    if (now || time (NULL) > hfd->cache_needs_flush + CACHE_FLUSH_TIME) {
	hdf_log ("flushed %d %d %d\n", now, time(NULL), hfd->cache_needs_flush);
	hdf_seek (hfd, hfd->cache_offset);
	poscheck (hfd, CACHE_SIZE);
	WriteFile (hfd->handle, hfd->cache, CACHE_SIZE, &outlen, NULL);
	hfd->cache_needs_flush = 0;
    }
}
#endif

#if 0

static int hdf_rw (struct hardfiledata *hfd, void *bufferp, uae_u64 offset, int len, int dowrite)
{
    DWORD outlen = 0, outlen2;
    uae_u8 *buffer = bufferp;
    int soff, size, mask, bs;

    bs = hfd->blocksize;
    mask = hfd->blocksize - 1;
    hfd->cache_valid = 0;
    if (hfd->handle_valid == HDF_HANDLE_ZFILE) {
	if (dowrite)
	    outlen = zfile_fwrite (buffer, len, 1, hfd->handle);
	else
	    outlen = zfile_fread (buffer, len, 1, hfd->handle);
    } else {
	soff = offset & mask;
	if (soff > 0) { /* offset not aligned to blocksize */
	    size = bs - soff;
	    if (size > len)
		size = len;
	    hdf_seek (hfd, offset & ~mask);
	    poscheck (hfd, len);
	    if (dowrite)
		WriteFile (hfd->handle, hfd->cache, bs, &outlen2, NULL);
	    else
		ReadFile (hfd->handle, hfd->cache, bs, &outlen2, NULL);
	    if (outlen2 != hfd->blocksize)
		goto end;
	    outlen += size;
	    memcpy (buffer, hfd->cache + soff,  size);
	    buffer += size;
	    offset += size;
	    len -= size;
	}
	while (len >= bs) { /* aligned access */
	    hdf_seek (hfd, offset);
	    poscheck (hfd, len);
	    size = len & ~mask;
	    if (size > CACHE_SIZE)
		size = CACHE_SIZE;
	    if (dowrite) {
		WriteFile (hfd->handle, hfd->cache, size, &outlen2, NULL);
	    } else {
		int coff = isincache(hfd, offset, size);
		if (coff >= 0) {
		    memcpy (buffer, hfd->cache + coff, size);
		    outlen2 = size;
		} else {
		    ReadFile (hfd->handle, hfd->cache, size, &outlen2, NULL);
		    if (outlen2 == size)
			memcpy (buffer, hfd->cache, size);
		}
	    }
	    if (outlen2 != size)
		goto end;
	    outlen += outlen2;
	    buffer += size;
	    offset += size;
	    len -= size;
	}
	if (len > 0) { /* len > 0 && len < bs */
	    hdf_seek (hfd, offset);
	    poscheck (hfd, len);
	    if (dowrite)
		WriteFile (hfd->handle, hfd->cache, bs, &outlen2, NULL);
	    else
		ReadFile (hfd->handle, hfd->cache, bs, &outlen2, NULL);
	    if (outlen2 != bs)
		goto end;
	    outlen += len;
	    memcpy (buffer, hfd->cache, len);
	}
    }
end:
    return outlen;
}

int hdf_read (struct hardfiledata *hfd, void *buffer, uae_u64 offset, int len)
{
    return hdf_rw (hfd, buffer, offset, len, 0);
}
int hdf_write (struct hardfiledata *hfd, void *buffer, uae_u64 offset, int len)
{
    return hdf_rw (hfd, buffer, offset, len, 1);
}

#else

static int hdf_read_2 (struct hardfiledata *hfd, void *buffer, uae_u64 offset, int len)
{
    DWORD outlen = 0;
    int coffset;

    if (offset == 0)
	hfd->cache_valid = 0;
    coffset = isincache (hfd, offset, len);
    if (coffset >= 0) {
	memcpy (buffer, hfd->cache + coffset, len);
	return len;
    }
    hfd->cache_offset = offset;
    if (offset + CACHE_SIZE > hfd->offset + (hfd->physsize - hfd->virtual_size))
	hfd->cache_offset = hfd->offset + (hfd->physsize - hfd->virtual_size) - CACHE_SIZE;
    hdf_seek (hfd, hfd->cache_offset);
    poscheck (hfd, CACHE_SIZE);
    if (hfd->handle_valid == HDF_HANDLE_WIN32)
	ReadFile (hfd->handle, hfd->cache, CACHE_SIZE, &outlen, NULL);
    else if (hfd->handle_valid == HDF_HANDLE_ZFILE)
	outlen = zfile_fread (hfd->cache, 1, CACHE_SIZE, hfd->handle);
    hfd->cache_valid = 0;
    if (outlen != CACHE_SIZE)
	return 0;
    hfd->cache_valid = 1;
    coffset = isincache (hfd, offset, len);
    if (coffset >= 0) {
	memcpy (buffer, hfd->cache + coffset, len);
	return len;
    }
    write_log ("hdf_read: cache bug! offset=%I64d len=%d\n", offset, len);
    hfd->cache_valid = 0;
    return 0;
}

int hdf_read_target (struct hardfiledata *hfd, void *buffer, uae_u64 offset, int len)
{
    int got = 0;
    uae_u8 *p = buffer;

    if (hfd->drive_empty)
	return 0;
    if (offset < hfd->virtual_size) {
	uae_u64 len2 = offset + len <= hfd->virtual_size ? len : hfd->virtual_size - offset;
	if (!hfd->virtual_rdb)
	    return 0;
	memcpy (buffer, hfd->virtual_rdb + offset, len2);
	return len2;
    }
    offset -= hfd->virtual_size;
    while (len > 0) {
	int ret, maxlen;
	if (hfd->physsize < CACHE_SIZE) {
	    hfd->cache_valid = 0;
	    hdf_seek (hfd, offset);
	    poscheck (hfd, len);
	    if (hfd->handle_valid == HDF_HANDLE_WIN32) {
		ReadFile (hfd->handle, hfd->cache, len, &ret, NULL);
		memcpy (buffer, hfd->cache, ret);
	    } else if (hfd->handle_valid == HDF_HANDLE_ZFILE) {
		ret = zfile_fread (buffer, 1, len, hfd->handle);
	    }
	    maxlen = len;
	} else {
	    maxlen = len > CACHE_SIZE ? CACHE_SIZE : len;
	    ret = hdf_read_2 (hfd, p, offset, maxlen);
	}
	got += ret;
	if (ret != maxlen)
	    return got;
	offset += maxlen;
	p += maxlen;
	len -= maxlen;
    }
    return got;
}

static int hdf_write_2 (struct hardfiledata *hfd, void *buffer, uae_u64 offset, int len)
{
    DWORD outlen = 0;
    if (hfd->readonly)
	return 0;
    hfd->cache_valid = 0;
    hdf_seek (hfd, offset);
    poscheck (hfd, len);
    memcpy (hfd->cache, buffer, len);
    if (hfd->handle_valid == HDF_HANDLE_WIN32)
	WriteFile (hfd->handle, hfd->cache, len, &outlen, NULL);
    else if (hfd->handle_valid == HDF_HANDLE_ZFILE)
	outlen = zfile_fwrite (hfd->cache, 1, len, hfd->handle);
    return outlen;
}
int hdf_write_target (struct hardfiledata *hfd, void *buffer, uae_u64 offset, int len)
{
    int got = 0;
    uae_u8 *p = buffer;

    if (hfd->drive_empty)
	return 0;
    if (offset < hfd->virtual_size)
	return len;
    offset -= hfd->virtual_size;
    while (len > 0) {
	int maxlen = len > CACHE_SIZE ? CACHE_SIZE : len;
	int ret = hdf_write_2(hfd, p, offset, maxlen);
	got += ret;
	if (ret != maxlen)
	    return got;
	offset += maxlen;
	p += maxlen;
	len -= maxlen;
    }
    return got;
}

int hdf_resize_target (struct hardfiledata *hfd, uae_u64 newsize)
{
    LONG highword = 0;
    DWORD ret;

    if (newsize >= 0x80000000) {
        highword = (DWORD)(newsize >> 32);
	ret = SetFilePointer (hfd->handle, (DWORD)newsize, &highword, FILE_BEGIN);
    } else {
        ret = SetFilePointer (hfd->handle, (DWORD)newsize, NULL, FILE_BEGIN);
    }
    if (ret == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
	return 0;
    if (SetEndOfFile (hfd->handle)) {
	hfd->physsize = newsize;
	return 1;
    }
    return 0;
}

#endif

#ifdef WINDDK

static void generatestorageproperty (struct uae_driveinfo *udi, int ignoreduplicates)
{
    strcpy (udi->vendor_id, "UAE");
    strcpy (udi->product_id, "DISK");
    strcpy (udi->product_rev, "1.0");
    sprintf (udi->device_name, "%s", udi->device_path);
    udi->removablemedia = 1;
}

static int getstorageproperty (PUCHAR outBuf, int returnedLength, struct uae_driveinfo *udi, int ignoreduplicates)
{
    PSTORAGE_DEVICE_DESCRIPTOR devDesc;
    char orgname[1024];
    PUCHAR p;
    int i, j;
    int size;

    devDesc = (PSTORAGE_DEVICE_DESCRIPTOR) outBuf;
    size = devDesc->Version;
    p = (PUCHAR) outBuf;
    if (offsetof(STORAGE_DEVICE_DESCRIPTOR, CommandQueueing) > size) {
	write_log ("too short STORAGE_DEVICE_DESCRIPTOR only %d bytes\n", size);
	return 1;
    }
    if (devDesc->DeviceType != INQ_DASD && devDesc->DeviceType != INQ_ROMD && devDesc->DeviceType != INQ_OPTD) {
        write_log ("not a direct access device, ignored (type=%d)\n", devDesc->DeviceType);
        return 1;
    }
    if (size > offsetof(STORAGE_DEVICE_DESCRIPTOR, VendorIdOffset) && devDesc->VendorIdOffset && p[devDesc->VendorIdOffset]) {
        j = 0;
        for (i = devDesc->VendorIdOffset; p[i] != (UCHAR) NULL && i < returnedLength; i++)
	    udi->vendor_id[j++] = p[i];
    }
    if (size > offsetof(STORAGE_DEVICE_DESCRIPTOR, ProductIdOffset) && devDesc->ProductIdOffset && p[devDesc->ProductIdOffset]) {
	j = 0;
	for (i = devDesc->ProductIdOffset; p[i] != (UCHAR) NULL && i < returnedLength; i++)
	    udi->product_id[j++] = p[i];
    }
    if (size > offsetof(STORAGE_DEVICE_DESCRIPTOR, ProductRevisionOffset) && devDesc->ProductRevisionOffset && p[devDesc->ProductRevisionOffset]) {
        j = 0;
        for (i = devDesc->ProductRevisionOffset; p[i] != (UCHAR) NULL && i < returnedLength; i++)
	    udi->product_rev[j++] = p[i];
    }
    if (size > offsetof(STORAGE_DEVICE_DESCRIPTOR, SerialNumberOffset) && devDesc->SerialNumberOffset && p[devDesc->SerialNumberOffset]) {
        j = 0;
        for (i = devDesc->SerialNumberOffset; p[i] != (UCHAR) NULL && i < returnedLength; i++)
	    udi->product_serial[j++] = p[i];
    }
    if (udi->vendor_id[0])
        strcat (udi->device_name, udi->vendor_id);
    if (udi->product_id[0]) {
        if (udi->device_name[0])
	    strcat (udi->device_name, " ");
	strcat (udi->device_name, udi->product_id);
    }
    if (udi->product_rev[0]) {
        if (udi->device_name[0])
	    strcat (udi->device_name, " ");
	strcat (udi->device_name, udi->product_rev);
    }
    if (udi->product_serial[0]) {
        if (udi->device_name[0])
	    strcat (udi->device_name, " ");
	strcat (udi->device_name, udi->product_serial);
    }
    if (!udi->device_name[0]) {
        write_log ("empty device id?!?, replacing with device path\n");
        strcpy (udi->device_name, udi->device_path);
    }
    udi->removablemedia = devDesc->RemovableMedia;
    write_log ("device id string: '%s'\n", udi->device_name);
    if (ignoreduplicates) {
	sprintf (orgname, "HD_%s", udi->device_name);
	if (isharddrive (orgname) >= 0) {
	    write_log ("duplicate device, ignored\n");
	    return 1;
	}
	if (!udi->removablemedia) {
	    write_log ("drive letter not removable, ignored\n");
	    return 1;
	}
    }
    return 0;
}

static BOOL GetDevicePropertyFromName(const char *DevicePath, DWORD Index, DWORD *index2, uae_u8 *buffer, int ignoreduplicates)
{
    int i, nosp;
    int ret = -1;
    STORAGE_PROPERTY_QUERY query;
    DRIVE_LAYOUT_INFORMATION		*dli;
    struct uae_driveinfo *udi;
    char orgname[1024];
    HANDLE hDevice = INVALID_HANDLE_VALUE;
    UCHAR outBuf[20000];
    DISK_GEOMETRY			dg;
    GET_LENGTH_INFORMATION		gli;
    PSTORAGE_ADAPTER_DESCRIPTOR         adpDesc;
    int gli_ok;
    BOOL                                status;
    ULONG                               length = 0,
					returned = 0,
					returnedLength;

    //
    // Now we have the device path. Open the device interface
    // to send Pass Through command

    udi = &uae_drives[*index2];
    memset (udi, 0, sizeof (struct uae_driveinfo));
    strcpy (udi->device_path, DevicePath);
    write_log ("opening device '%s'\n", udi->device_path);
    hDevice = CreateFile(
		udi->device_path,    // device interface name
		GENERIC_READ | GENERIC_WRITE,       // dwDesiredAccess
		FILE_SHARE_READ | FILE_SHARE_WRITE, // dwShareMode
		NULL,                               // lpSecurityAttributes
		OPEN_EXISTING,                      // dwCreationDistribution
		0,                                  // dwFlagsAndAttributes
		NULL                                // hTemplateFile
		);

    //
    // We have the handle to talk to the device.
    // So we can release the interfaceDetailData buffer
    //


    if (hDevice == INVALID_HANDLE_VALUE) {
	write_log ("CreateFile failed with error: %d\n", GetLastError());
	ret = 1;
	goto end;
    }

    query.PropertyId = StorageAdapterProperty;
    query.QueryType = PropertyStandardQuery;

    status = DeviceIoControl(
			hDevice,
			IOCTL_STORAGE_QUERY_PROPERTY,
			&query,
			sizeof(STORAGE_PROPERTY_QUERY),
			&outBuf,
			sizeof (outBuf),
			&returnedLength,
			NULL
			);
    if (!status) {
	write_log ("IOCTL_STORAGE_QUERY_PROPERTY failed with error code %d.\n", GetLastError());
    } else {
        adpDesc = (PSTORAGE_ADAPTER_DESCRIPTOR) outBuf;
    }

    memset (outBuf, 0, sizeof outBuf);
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    status = DeviceIoControl(
			hDevice,
			IOCTL_STORAGE_QUERY_PROPERTY,
			&query,
			sizeof(STORAGE_PROPERTY_QUERY),
			&outBuf,
			sizeof outBuf,
			&returnedLength,
			NULL);
    if (!status) {
	DWORD err = GetLastError ();
        write_log ("IOCTL_STORAGE_QUERY_PROPERTY failed with error code %d.\n", err);
	if (err != ERROR_INVALID_FUNCTION) {
	    ret = 1;
	    goto end;
	}
	nosp = 1;
	generatestorageproperty (udi, ignoreduplicates);
    } else {
	int r;
	nosp = 0;
	r = getstorageproperty (outBuf, returnedLength, udi, ignoreduplicates);
	if (r) {
	    ret = r;
	    goto end;
	}
    }
    strcpy (orgname, udi->device_name);
    udi->bytespersector = 512;
    if (!DeviceIoControl (hDevice, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, (void*)&dg, sizeof (dg), &returnedLength, NULL)) {
	DWORD err = GetLastError();
	if (isnomediaerr (err)) {
	    udi->nomedia = 1;
	    goto amipartfound;
	}
	write_log ("IOCTL_DISK_GET_DRIVE_GEOMETRY failed with error code %d.\n", err);
	ret = 1;
	goto end;
    }
    udi->readonly = 0;
    if (!DeviceIoControl (hDevice, IOCTL_DISK_IS_WRITABLE, NULL, 0, NULL, 0, &returnedLength, NULL)) {
	DWORD err = GetLastError ();
	if (err == ERROR_WRITE_PROTECT)
	    udi->readonly = 1;
    }

    gli_ok = 1;
    if (!DeviceIoControl (hDevice, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, (void*)&gli, sizeof (gli), &returnedLength, NULL)) {
	gli_ok = 0;
	write_log ("IOCTL_DISK_GET_LENGTH_INFO failed with error code %d.\n", GetLastError());
	if (!nosp)
	    write_log ("IOCTL_DISK_GET_LENGTH_INFO not supported, detected disk size may not be correct.\n");
    }
    udi->bytespersector = dg.BytesPerSector;
    if (dg.BytesPerSector < 512) {
	write_log ("unsupported blocksize < 512 (%d)\n", dg.BytesPerSector);
	ret = 1;
	goto end;
    }
    if (dg.BytesPerSector > 2048) {
	write_log ("unsupported blocksize > 2048 (%d)\n", dg.BytesPerSector);
	ret = 1;
	goto end;
    }
    udi->offset = 0;
    write_log ("BPS=%d Cyls=%I64d TPC=%d SPT=%d MediaType=%d\n",
	dg.BytesPerSector, dg.Cylinders.QuadPart, dg.TracksPerCylinder, dg.SectorsPerTrack, dg.MediaType);
    udi->size = (uae_u64)dg.BytesPerSector * (uae_u64)dg.Cylinders.QuadPart *
	(uae_u64)dg.TracksPerCylinder * (uae_u64)dg.SectorsPerTrack;
    if (gli_ok)
	udi->size = gli.Length.QuadPart;
    write_log ("device size %I64d (0x%I64x) bytes\n", udi->size, udi->size);
    trim (orgname);

    memset (outBuf, 0, sizeof (outBuf));
    status = DeviceIoControl(hDevice, IOCTL_DISK_GET_DRIVE_LAYOUT, NULL, 0,
	&outBuf, sizeof (outBuf), &returnedLength, NULL);
    if (!status) {
	DWORD err = GetLastError();
	write_log ("IOCTL_DISK_GET_DRIVE_LAYOUT failed with error code %d.\n", err);
	if (err != ERROR_INVALID_FUNCTION) {
	    ret = 1;
	    goto end;
	}
	goto amipartfound;
    }
    dli = (DRIVE_LAYOUT_INFORMATION*)outBuf;
    if (dli->PartitionCount) {
	struct uae_driveinfo *udi2 = udi;
	int nonzeropart = 0;
	int gotpart = 0;
	int safepart = 0;
	write_log ("%d MBR partitions found\n", dli->PartitionCount);
	for (i = 0; i < dli->PartitionCount && (*index2) < MAX_FILESYSTEM_UNITS; i++) {
	    PARTITION_INFORMATION *pi = &dli->PartitionEntry[i];
	    if (pi->PartitionType == PARTITION_ENTRY_UNUSED)
		continue;
	    write_log ("%d: num: %d type: %02X offset: %I64d size: %I64d, ", i, pi->PartitionNumber, pi->PartitionType, pi->StartingOffset.QuadPart, pi->PartitionLength.QuadPart);
	    if (pi->RecognizedPartition == 0) {
		write_log ("unrecognized\n");
		continue;
	    }
	    nonzeropart++;
	    if (pi->PartitionType != 0x76) {
		write_log ("type not 0x76\n");
		continue;
	    }
	    memmove (udi, udi2, sizeof (*udi));
	    udi->device_name[0] = 0;
	    udi->offset = pi->StartingOffset.QuadPart;
	    udi->size = pi->PartitionLength.QuadPart;
	    write_log ("used\n");
	    if (safetycheck (hDevice, udi->offset, buffer, dg.BytesPerSector) <= 0) {
		sprintf (udi->device_name, "HD_P#%d_%s", pi->PartitionNumber, orgname);
		udi++;
		(*index2)++;
		safepart = 1;
	    }
	    gotpart = 1;
	}
	if (!nonzeropart) {
	    write_log ("empty MBR partition table detected, checking for RDB\n");
	} else if (!gotpart) {
	    write_log ("non-empty MBR partition table detected, doing RDB check anyway\n");
	} else if (safepart) {
	    goto amipartfound; /* ugly but bleh.. */
	}
    } else {
	write_log ("no MBR partition table detected, checking for RDB\n");
    }

    udi->dangerous = safetycheck (hDevice, 0, buffer, dg.BytesPerSector);
    if (udi->dangerous > 0)
	goto end;
amipartfound:
    sprintf (udi->device_name, "HD_%s", orgname);
    {
	int cnt = 1;
	int off = strlen (udi->device_name);
	while (isharddrive (udi->device_name) >= 0) {
	    udi->device_name[off] = '_';
	    udi->device_name[off + 1] = cnt + '0';
	    udi->device_name[off + 2] = 0;
	    cnt++;
	}
    }	
    (*index2)++;
end:
    if (hDevice != INVALID_HANDLE_VALUE)
	CloseHandle (hDevice);
    return ret;
}

/* see MS KB article Q264203 more more information */

static BOOL GetDeviceProperty(HDEVINFO IntDevInfo, DWORD Index, DWORD *index2, uae_u8 *buffer)
/*++

Routine Description:

    This routine enumerates the disk devices using the Device interface
    GUID DiskClassGuid. Gets the Adapter & Device property from the port
    driver. Then sends IOCTL through SPTI to get the device Inquiry data.

Arguments:

    IntDevInfo - Handles to the interface device information list

    Index      - Device member

Return Value:

  TRUE / FALSE. This decides whether to continue or not

--*/
{
    SP_DEVICE_INTERFACE_DATA            interfaceData;
    PSP_DEVICE_INTERFACE_DETAIL_DATA    interfaceDetailData = NULL;
    BOOL                                status;
    DWORD                               interfaceDetailDataSize = 0,
					reqSize,
					errorCode;
    int ret = -1;

    interfaceData.cbSize = sizeof (SP_INTERFACE_DEVICE_DATA);

    status = SetupDiEnumDeviceInterfaces (
		IntDevInfo,             // Interface Device Info handle
		0,                      // Device Info data
		&GUID_DEVINTERFACE_DISK, // Interface registered by driver
		Index,                  // Member
		&interfaceData          // Device Interface Data
		);

    if (status == FALSE) {
	errorCode = GetLastError();
	if (errorCode != ERROR_NO_MORE_ITEMS) {
	    write_log ("SetupDiEnumDeviceInterfaces failed with error: %d\n", errorCode);
	}
	ret = 0;
	goto end;
    }

    //
    // Find out required buffer size, so pass NULL
    //

    status = SetupDiGetDeviceInterfaceDetail (
		IntDevInfo,         // Interface Device info handle
		&interfaceData,     // Interface data for the event class
		NULL,               // Checking for buffer size
		0,                  // Checking for buffer size
		&reqSize,           // Buffer size required to get the detail data
		NULL                // Checking for buffer size
		);

    //
    // This call returns ERROR_INSUFFICIENT_BUFFER with reqSize
    // set to the required buffer size. Ignore the above error and
    // pass a bigger buffer to get the detail data
    //

    if (status == FALSE) {
	errorCode = GetLastError();
	if (errorCode != ERROR_INSUFFICIENT_BUFFER) {
	    write_log ("SetupDiGetDeviceInterfaceDetail failed with error: %d\n", errorCode);
	    ret = 0;
	    goto end;
	}
    }

    //
    // Allocate memory to get the interface detail data
    // This contains the devicepath we need to open the device
    //

    interfaceDetailDataSize = reqSize;
    interfaceDetailData = malloc (interfaceDetailDataSize);
    if (interfaceDetailData == NULL) {
	write_log ("Unable to allocate memory to get the interface detail data.\n");
	ret = 0;
	goto end;
    }
    interfaceDetailData->cbSize = sizeof (SP_INTERFACE_DEVICE_DETAIL_DATA);

    status = SetupDiGetDeviceInterfaceDetail (
		  IntDevInfo,               // Interface Device info handle
		  &interfaceData,           // Interface data for the event class
		  interfaceDetailData,      // Interface detail data
		  interfaceDetailDataSize,  // Interface detail data size
		  &reqSize,                 // Buffer size required to get the detail data
		  NULL);                    // Interface device info

    if (status == FALSE) {
	write_log ("Error in SetupDiGetDeviceInterfaceDetail failed with error: %d\n", GetLastError());
	ret = 0;
	goto end;
    }
    
    ret = GetDevicePropertyFromName (interfaceDetailData->DevicePath, Index, index2, buffer, 0);

end:
    free (interfaceDetailData);

    return ret;
}


#endif


static int num_drives;

static int hdf_init2 (int force)
{
#ifdef WINDDK
    HDEVINFO hIntDevInfo;
#endif
    DWORD index = 0, index2 = 0, drive;
    uae_u8 *buffer;
    UINT errormode;
    DWORD dwDriveMask;
    static int done;

    if (done && !force)
	return num_drives;
    done = 1;
    num_drives = 0;
#ifdef WINDDK
    buffer = VirtualAlloc (NULL, 65536, MEM_COMMIT, PAGE_READWRITE);
    if (buffer) {
	memset (uae_drives, 0, sizeof (uae_drives));
	num_drives = 0;
	hIntDevInfo = SetupDiGetClassDevs (&GUID_DEVINTERFACE_DISK, NULL, NULL, DIGCF_PRESENT | DIGCF_INTERFACEDEVICE);
	if (hIntDevInfo != INVALID_HANDLE_VALUE) {
	    while (index < MAX_FILESYSTEM_UNITS) {
		memset (uae_drives + index2, 0, sizeof (struct uae_driveinfo));
		if (!GetDeviceProperty (hIntDevInfo, index, &index2, buffer))
		    break;
		index++;
		num_drives = index2;
	    }
	    SetupDiDestroyDeviceInfoList(hIntDevInfo);
	}
	errormode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
	dwDriveMask = GetLogicalDrives();
        for(drive = 'A'; drive <= 'Z'; drive++) {
	    if((dwDriveMask & 1) && (drive >= 'C' || usefloppydrives)) {
		char tmp1[20], tmp2[20];
		DWORD drivetype;
		sprintf (tmp1, "%c:\\", drive);
		drivetype = GetDriveType(tmp1);
		if (drivetype != DRIVE_REMOTE) {
		    sprintf (tmp2, "\\\\.\\%c:", drive);
		    GetDevicePropertyFromName (tmp2, index, &index2, buffer, 1);
		    num_drives = index2;
		}
	    }
	    dwDriveMask >>= 1;
	}
	SetErrorMode(errormode);
#if 0
	hIntDevInfo = SetupDiGetClassDevs (&GUID_DEVCLASS_MTD, NULL, NULL, DIGCF_PRESENT);
	if (hIntDevInfo != INVALID_HANDLE_VALUE) {
	    while (index < MAX_FILESYSTEM_UNITS) {
		memset (uae_drives + index2, 0, sizeof (struct uae_driveinfo));
		index++;
		num_drives = index2;
	    }
	    SetupDiDestroyDeviceInfoList(hIntDevInfo);
	}
#endif
	VirtualFree (buffer, 0, MEM_RELEASE);
    }
    num_drives = index2;
    write_log ("Drive scan result: %d Amiga formatted drives detected\n", num_drives);
#endif
    return num_drives;
}

int hdf_init_target (void)
{
    return hdf_init2 (0);
}

int hdf_getnumharddrives (void)
{
    return num_drives;
}

char *hdf_getnameharddrive (int index, int flags, int *sectorsize)
{
    static char name[512];
    char tmp[32];
    uae_u64 size = uae_drives[index].size;
    int nomedia = uae_drives[index].nomedia;
    char *dang = "?";
    char *rw = "RW";

    switch (uae_drives[index].dangerous)
    {
	case -9:
	dang = "Empty";
	break;
	case -2:
	dang = "SRAM";
	break;
	case -1:
	dang = "RDB";
	break;
	case 0:
	dang = "NON-EMPTY";
	break;
    }
    if (nomedia)	
	dang = "NO MEDIA";
    if (uae_drives[index].readonly)
	rw = "RO";

    if (sectorsize)
	*sectorsize = uae_drives[index].bytespersector;
    if (flags & 1) {
	if (nomedia) {
	    strcpy (tmp, "N/A");
	} else {
	    if (size >= 1024 * 1024 * 1024)
		sprintf (tmp, "%.1fG", ((double)(uae_u32)(size / (1024 * 1024))) / 1024.0);
	    else if (size < 10 * 1024 * 1024)
		sprintf (tmp, "%dK", size / 1024);
	    else
		sprintf (tmp, "%.1fM", ((double)(uae_u32)(size / (1024))) / 1024.0);
	}
	sprintf (name, "%10s [%s,%s] %s", dang, tmp, rw, uae_drives[index].device_name + 3);
	return name;
    }
    if (flags & 2)
	return uae_drives[index].device_path;
    return uae_drives[index].device_name;
}

static int hmc (struct hardfiledata *hfd)
{
    uae_u8 *buf = xmalloc (hfd->blocksize);
    DWORD ret, got, err, status;
    int first = 1;

    while (hfd->handle_valid) {
	DWORD errormode;
	write_log ("testing if %s has media inserted\n", hfd->emptyname);
	status = 0;
	errormode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
	SetFilePointer (hfd->handle, 0, NULL, FILE_BEGIN);
	ret = ReadFile (hfd->handle, buf, hfd->blocksize, &got, NULL);
	err = GetLastError ();
	SetErrorMode(errormode);
	if (ret) {
	    if (got == hfd->blocksize) {
		write_log ("read ok (%d)\n", got);
	    } else {
		write_log ("read ok but no data (%d)\n", hfd->blocksize);
		ret = 0;
		err = 0;
	    }
	} else {
	    write_log ("=%d\n", err);
	}
	if (!ret && (err == ERROR_DEV_NOT_EXIST || err == ERROR_WRONG_DISK)) {
	    if (!first)
		break;
	    first = 0;
	    hdf_open (hfd, hfd->emptyname);
	    if (!hfd->handle_valid) {
		/* whole device has disappeared */
		status = -1;
		goto end;
	    }
	    continue;
	}
	break;
    }
    if (ret && hfd->drive_empty)
	status = 1;
    if (!ret && !hfd->drive_empty && isnomediaerr (err))
	status = -1;
end:
    xfree (buf);
    write_log("hmc returned %d\n", status);
    return status;
}

int hardfile_remount (int nr);


static void hmc_check (struct hardfiledata *hfd, struct uaedev_config_info *uci, int *rescanned, int *reopen,
		       int *gotinsert, const char *drvname, int inserted)
{
    int ret;

    if (!hfd || !hfd->emptyname)
	return;
    if (*rescanned == 0) {
	hdf_init2 (1);
	*rescanned = 1;
    }
    if (hfd->emptyname && strlen (hfd->emptyname) >= 6 && drvname && strlen (drvname) == 3 && drvname[1] == ':' && drvname[2] == '\\' && !inserted) { /* drive letter check */
	char tmp[10], *p;
	sprintf (tmp, "\\\\.\\%c:", drvname[0]);
	p = hfd->emptyname + strlen (hfd->emptyname) - 6;
	write_log("workaround-remove test: '%s' and '%s'\n", p, drvname);
	if (!strcmp (tmp, p)) {
	    char *en = my_strdup (hfd->emptyname);
	    write_log ("workaround-remove done\n");
	    hdf_close (hfd);
	    hfd->emptyname = en;
	    hfd->drive_empty = 1;
	    hardfile_do_disk_change (uci, 0);
	    return;
	}
    }

    if (hfd->drive_empty < 0 || !hfd->handle_valid) {
        int empty = hfd->drive_empty;
        int r;
        //write_log ("trying to open '%s' de=%d hv=%d\n", hfd->emptyname, hfd->drive_empty, hfd->handle_valid);
        r = hdf_open (hfd, hfd->emptyname);
        //write_log ("=%d\n", r);
        if (!r)
	    return;
	*reopen = 1;
	if (hfd->drive_empty < 0)
	    return;
	hfd->drive_empty = empty ? 1 : 0;
    }
    ret = hmc (hfd);
    if (!ret)
	return;
    if (ret > 0) {
        if (*reopen == 0) {
	    hdf_open (hfd, hfd->emptyname);
	    if (!hfd->handle_valid)
	        return;
	}
	*gotinsert = 1;
	//hardfile_remount (uci);
    }
    hardfile_do_disk_change (uci, ret < 0 ? 0 : 1);
}

int win32_hardfile_media_change (const char *drvname, int inserted)
{
    int gotinsert = 0, rescanned = 0;
    int i, j;

    for (i = 0; i < MAX_FILESYSTEM_UNITS; i++) {
	struct hardfiledata *hfd = get_hardfile_data (i);
	int reopen = 0;
	if (!hfd || !(hfd->flags & HFD_FLAGS_REALDRIVE))
	    continue;
	for (j = 0; j < currprefs.mountitems; j++) {
	    if (currprefs.mountconfig[j].configoffset == i) {
		hmc_check (hfd, &currprefs.mountconfig[j], &rescanned, &reopen, &gotinsert, drvname, inserted);
		break;
	    }
	}
    }
    for (i = 0; i < currprefs.mountitems; i++) {
	extern struct hd_hardfiledata *pcmcia_sram;
	int reopen = 0;
	struct uaedev_config_info *uci = &currprefs.mountconfig[i];
	if (uci->controller == HD_CONTROLLER_PCMCIA_SRAM) {
	    hmc_check (&pcmcia_sram->hfd, uci, &rescanned, &reopen, &gotinsert, drvname, inserted);
	}
    }

    //write_log ("win32_hardfile_media_change returned %d\n", gotinsert);
    return gotinsert;
}

static int progressdialogreturn;
static int progressdialogactive;

static INT_PTR CALLBACK ProgressDialogProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch(msg)
    {
	case WM_DESTROY:
	PostQuitMessage (0);
	progressdialogactive = 0;
	return TRUE;
	case WM_CLOSE:
	DestroyWindow(hDlg);
	if (progressdialogreturn < 0)
	    progressdialogreturn = 0;
	return TRUE;
	case WM_INITDIALOG:
	    return TRUE;
	case WM_COMMAND:
	    switch (LOWORD(wParam))
	    {
		case IDCANCEL:
		    progressdialogreturn = 0;
		    DestroyWindow (hDlg);
		return TRUE;
	    }
	break;
    }
    return FALSE;
}

extern HMODULE hUIDLL;
extern HINSTANCE hInst;

#define COPY_CACHE_SIZE 1024*1024
int harddrive_to_hdf(HWND hDlg, struct uae_prefs *p, int idx)
{
    HANDLE h = INVALID_HANDLE_VALUE, hdst = INVALID_HANDLE_VALUE;
    void *cache = NULL;
    DWORD ret, got, gotdst, get;
    uae_u64 size, sizecnt, written;
    LARGE_INTEGER li;
    char path[MAX_DPATH], tmp[MAX_DPATH], tmp2[MAX_DPATH];
    DWORD retcode = 0;
    HWND hwnd, hwndprogress, hwndprogresstxt;
    MSG msg;
    int pct, cnt;

    cache = VirtualAlloc (NULL, COPY_CACHE_SIZE, MEM_COMMIT, PAGE_READWRITE);
    if (!cache)
	goto err;
    h = CreateFile (uae_drives[idx].device_path, GENERIC_READ, FILE_SHARE_READ, NULL,
	OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_NO_BUFFERING, NULL);
    if (h == INVALID_HANDLE_VALUE)
	goto err;
    size = uae_drives[idx].size;
    path[0] = 0;
    DiskSelection (hDlg, IDC_PATH_NAME, 3, p, 0);
    GetDlgItemText (hDlg, IDC_PATH_NAME, path, MAX_DPATH);
    if (*path == 0)
	goto err;
    hdst = CreateFile (path, GENERIC_WRITE, 0, NULL,
	CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_NO_BUFFERING, NULL);
    if (hdst == INVALID_HANDLE_VALUE)
	goto err;
    li.QuadPart = size;
    ret = SetFilePointer(hdst, li.LowPart, &li.HighPart, FILE_BEGIN);
    if (ret == INVALID_FILE_SIZE && GetLastError() != NO_ERROR)
	goto err;
    if (!SetEndOfFile(hdst))
	goto err;
    li.QuadPart = 0;
    SetFilePointer(hdst, 0, &li.HighPart, FILE_BEGIN);
    li.QuadPart = 0;
    SetFilePointer(h, 0, &li.HighPart, FILE_BEGIN);
    progressdialogreturn = -1;
    progressdialogactive = 1;
    hwnd = CreateDialog (hUIDLL ? hUIDLL : hInst, MAKEINTRESOURCE (IDD_PROGRESSBAR), hDlg, ProgressDialogProc);
    if (hwnd == NULL)
	goto err;
    hwndprogress = GetDlgItem(hwnd, IDC_PROGRESSBAR);
    hwndprogresstxt = GetDlgItem(hwnd, IDC_PROGRESSBAR_TEXT);
    ShowWindow (hwnd, SW_SHOW);
    pct = 0;
    cnt = 1000;
    sizecnt = 0;
    written = 0;
    for (;;) {
	if (progressdialogreturn >= 0)
	    break;
	if (cnt > 0) {
	    SendMessage(hwndprogress, PBM_SETPOS, (WPARAM)pct, 0);
	    sprintf (tmp, "%dM / %dM (%d%%)", (int)(written >> 20), (int)(size >> 20), pct);
	    SendMessage(hwndprogresstxt, WM_SETTEXT, 0, (LPARAM)tmp);
	    while (PeekMessage (&msg, 0, 0, 0, PM_REMOVE)) {
		TranslateMessage (&msg);
		DispatchMessage (&msg);
	    }
	    cnt = 0;
	}
	got = gotdst = 0;
	li.QuadPart = sizecnt;
	if (SetFilePointer(h, li.LowPart, &li.HighPart, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
	    DWORD err = GetLastError();
	    if (err != NO_ERROR) {
		progressdialogreturn = 3;
		break;
	    }
	}
	get = COPY_CACHE_SIZE;
	if (sizecnt + get > size)
	    get = size - sizecnt;
	if (!ReadFile(h, cache, get, &got, NULL)) {
	    progressdialogreturn = 4;
	    break;
	}
	if (get != got) {
	    progressdialogreturn = 5;
	    break;
	}
	if (got > 0) {
	    if (written + got > size)
		got = size - written;
	    if (!WriteFile(hdst, cache, got, &gotdst, NULL))  {
		progressdialogreturn = 5;
		break;
	    }
	    written += gotdst;
	    if (written == size)
		break;
	}
	if (got != COPY_CACHE_SIZE) {
	    progressdialogreturn = 1;
	    break;
	}
	if (got != gotdst) {
	    progressdialogreturn = 2;
	    break;
	}
	cnt++;
	sizecnt += got;
	pct = (int)(sizecnt * 100 / size);
    }
    if (progressdialogactive) {
	DestroyWindow (hwnd);
	while (PeekMessage (&msg, 0, 0, 0, PM_REMOVE)) {
	    TranslateMessage (&msg);
	    DispatchMessage (&msg);
	}
    }
    if (progressdialogreturn >= 0)
	goto err;
    retcode = 1;
    WIN32GUI_LoadUIString (IDS_HDCLONE_OK, tmp, MAX_DPATH);
    gui_message (tmp);
    goto ok;

err:
    WIN32GUI_LoadUIString (IDS_HDCLONE_FAIL, tmp, MAX_DPATH);
    sprintf (tmp2, tmp, progressdialogreturn, GetLastError());
    gui_message (tmp2);

ok:
    if (h != INVALID_HANDLE_VALUE)
	CloseHandle(h);
    if (cache)
	VirtualFree(cache, 0, MEM_RELEASE);
    if (hdst != INVALID_HANDLE_VALUE)
	CloseHandle(hdst);
    return retcode;
}
