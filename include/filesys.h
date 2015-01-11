 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Unix file system handler for AmigaDOS
  *
  * Copyright 1997 Bernd Schmidt
  */

#ifndef FILESYS_H
#define FILESYS_H

struct hardfilehandle;

#define MAX_HDF_CACHE_BLOCKS 128
#define MAX_SCSI_SENSE 36
struct hdf_cache
{
	bool valid;
	uae_u8 *data;
	uae_u64 block;
	bool dirty;
	int readcount;
	int writecount;
	time_t lastaccess;
};

struct hardfiledata {
    uae_u64 virtsize; // virtual size
    uae_u64 physsize; // physical size (dynamic disk)
    uae_u64 offset;
	struct uaedev_config_info ci;
	struct hardfilehandle *handle;
    int handle_valid;
    int dangerous;
    int flags;
    uae_u8 *cache;
    int cache_valid;
    uae_u64 cache_offset;
    TCHAR vendor_id[8 + 1];
    TCHAR product_id[16 + 1];
    TCHAR product_rev[4 + 1];
    /* geometry from possible RDSK block */
    int rdbcylinders;
    int rdbsectors;
    int rdbheads;
    uae_u8 *virtual_rdb;
    uae_u64 virtual_size;
    int unitnum;
    int byteswap;
    int adide;
    int hfd_type;

    uae_u8 *vhd_header;
    uae_u32 vhd_bamoffset;
    uae_u32 vhd_bamsize;
    uae_u32 vhd_blocksize;
    uae_u8 *vhd_sectormap;
    uae_u64 vhd_sectormapblock;
    uae_u32 vhd_bitmapsize;
    uae_u64 vhd_footerblock;

	void *chd_handle;

    int drive_empty;
    TCHAR *emptyname;

	struct hdf_cache bcache[MAX_HDF_CACHE_BLOCKS];
	uae_u8 scsi_sense[MAX_SCSI_SENSE];

	struct uaedev_config_info delayedci;
	int reinsertdelay;
	bool isreinsert;
};

#define HFD_FLAGS_REALDRIVE 1
#define HFD_FLAGS_REALDRIVEPARTITION 2

struct hd_hardfiledata {
    struct hardfiledata hfd;
    uae_u64 size;
    int cyls;
    int heads;
    int secspertrack;
    int cyls_def;
    int secspertrack_def;
    int heads_def;
    int ansi_version;
};

#define HD_CONTROLLER_TYPE_UAE 0
#define HD_CONTROLLER_TYPE_IDE_AUTO 1
#define HD_CONTROLLER_TYPE_IDE_MB 2
#define HD_CONTROLLER_TYPE_IDE_GVP 3
#define HD_CONTROLLER_TYPE_SCSI_AUTO 4
#define HD_CONTROLLER_TYPE_SCSI_A2091 5
#define HD_CONTROLLER_TYPE_SCSI_A2091_2 6
#define HD_CONTROLLER_TYPE_SCSI_GVP 7
#define HD_CONTROLLER_TYPE_SCSI_GVP_2 8
#define HD_CONTROLLER_TYPE_SCSI_A4091 9
#define HD_CONTROLLER_TYPE_SCSI_A4091_2 10
#define HD_CONTROLLER_TYPE_SCSI_FASTLANE 11
#define HD_CONTROLLER_TYPE_SCSI_FASTLANE_2 12
#define HD_CONTROLLER_TYPE_SCSI_OKTAGON 13
#define HD_CONTROLLER_TYPE_SCSI_OKTAGON_2 14
#define HD_CONTROLLER_TYPE_SCSI_A3000 15
#define HD_CONTROLLER_TYPE_SCSI_A4000T 16
#define HD_CONTROLLER_TYPE_SCSI_CDTV 17
#define HD_CONTROLLER_TYPE_SCSI_CPUBOARD 18
#define HD_CONTROLLER_TYPE_PCMCIA_SRAM 19
#define HD_CONTROLLER_TYPE_PCMCIA_IDE 20

#define HD_CONTROLLER_TYPE_IDE_FIRST 1
#define HD_CONTROLLER_TYPE_IDE_LAST 3
#define HD_CONTROLLER_TYPE_SCSI_FIRST 4
#define HD_CONTROLLER_TYPE_SCSI_LAST 18

#define FILESYS_VIRTUAL 0
#define FILESYS_HARDFILE 1
#define FILESYS_HARDFILE_RDB 2
#define FILESYS_HARDDRIVE 3
#define FILESYS_CD 4
#define FILESYS_TAPE 5

#define MAX_FILESYSTEM_UNITS 30

struct uaedev_mount_info;
extern struct uaedev_mount_info options_mountinfo;

extern struct hardfiledata *get_hardfile_data (int nr);
#define FILESYS_MAX_BLOCKSIZE 2048
extern int hdf_open (struct hardfiledata *hfd);
extern int hdf_open (struct hardfiledata *hfd, const TCHAR *altname);
extern int hdf_dup (struct hardfiledata *dhfd, const struct hardfiledata *shfd);
extern void hdf_close (struct hardfiledata *hfd);
extern int hdf_read_rdb (struct hardfiledata *hfd, void *buffer, uae_u64 offset, int len);
extern int hdf_read (struct hardfiledata *hfd, void *buffer, uae_u64 offset, int len);
extern int hdf_write (struct hardfiledata *hfd, void *buffer, uae_u64 offset, int len);
extern int hdf_getnumharddrives (void);
extern TCHAR *hdf_getnameharddrive (int index, int flags, int *sectorsize, int *dangerousdrive);
extern int isspecialdrive(const TCHAR *name);
extern int get_native_path(uae_u32 lock, TCHAR *out);
extern void hardfile_do_disk_change (struct uaedev_config_data *uci, bool insert);
extern void hardfile_send_disk_change (struct hardfiledata *hfd, bool insert);
extern int hardfile_media_change (struct hardfiledata *hfd, struct uaedev_config_info *ci, bool inserted, bool timer);
extern int hardfile_added (struct uaedev_config_info *ci);

void hdf_hd_close(struct hd_hardfiledata *hfd);
int hdf_hd_open(struct hd_hardfiledata *hfd);


extern int vhd_create (const TCHAR *name, uae_u64 size, uae_u32);

extern int hdf_init_target (void);
extern int hdf_open_target (struct hardfiledata *hfd, const TCHAR *name);
extern int hdf_dup_target (struct hardfiledata *dhfd, const struct hardfiledata *shfd);
extern void hdf_close_target (struct hardfiledata *hfd);
extern int hdf_read_target (struct hardfiledata *hfd, void *buffer, uae_u64 offset, int len);
extern int hdf_write_target (struct hardfiledata *hfd, void *buffer, uae_u64 offset, int len);
extern int hdf_resize_target (struct hardfiledata *hfd, uae_u64 newsize);
extern void getchsgeometry (uae_u64 size, int *pcyl, int *phead, int *psectorspertrack);
extern void getchsgeometry_hdf (struct hardfiledata *hfd, uae_u64 size, int *pcyl, int *phead, int *psectorspertrack);
extern void getchspgeometry (uae_u64 total, int *pcyl, int *phead, int *psectorspertrack, bool idegeometry);

#endif /* MEMORY_H */
