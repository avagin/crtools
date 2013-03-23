#ifndef __CR_MAGIC_H__
#define __CR_MAGIC_H__

/*
 * Basic multi-file images
 */

#define CRTOOLS_IMAGES_V1	1

/*
 * Raw images are images in which data is stored in some
 * non-crtool format (ip tool dumps, tarballs, etc.)
 */

#define RAW_IMAGE_MAGIC		0x0

/*
 * The magic-s below correspond to coordinates
 * of various Russian towns in the NNNNEEEE form.
 */

#define INVENTORY_MAGIC		0x58313116 /* Veliky Novgorod */
#define PSTREE_MAGIC		0x50273030 /* Kyiv */
#define FDINFO_MAGIC		0x56213732 /* Dmitrov */
#define PAGEMAP_MAGIC		0x56084025 /* Vladimir */
#define SHMEM_PAGEMAP_MAGIC	PAGEMAP_MAGIC
#define PAGES_MAGIC		RAW_IMAGE_MAGIC
#define CORE_MAGIC		0x55053847 /* Kolomna */
#define IDS_MAGIC		0x54432030 /* Konigsberg */
#define VMAS_MAGIC		0x54123737 /* Tula */
#define PIPES_MAGIC		0x56513555 /* Tver */
#define PIPES_DATA_MAGIC	0x56453709 /* Dubna */
#define FIFO_MAGIC		0x58364939 /* Kirov */
#define FIFO_DATA_MAGIC		0x59333054 /* Tosno */
#define SIGACT_MAGIC		0x55344201 /* Murom */
#define UNIXSK_MAGIC		0x54373943 /* Ryazan */
#define INETSK_MAGIC		0x56443851 /* Pereslavl */
#define PACKETSK_MAGIC		0x60454618 /* Veliky Ustyug */
#define ITIMERS_MAGIC		0x57464056 /* Kostroma */
#define SK_QUEUES_MAGIC		0x56264026 /* Suzdal */
#define UTSNS_MAGIC		0x54473203 /* Smolensk */
#define CREDS_MAGIC		0x54023547 /* Kozelsk */
#define IPCNS_VAR_MAGIC		0x53115007 /* Samara */
#define IPCNS_SHM_MAGIC		0x46283044 /* Odessa */
#define IPCNS_MSG_MAGIC		0x55453737 /* Moscow */
#define IPCNS_SEM_MAGIC		0x59573019 /* St. Petersburg */
#define REG_FILES_MAGIC		0x50363636 /* Belgorod */
#define FS_MAGIC		0x51403912 /* Voronezh */
#define MM_MAGIC		0x57492820 /* Pskov */
#define REMAP_FPATH_MAGIC	0x59133954 /* Vologda */
#define GHOST_FILE_MAGIC	0x52583605 /* Oryol */
#define TCP_STREAM_MAGIC	0x51465506 /* Orenburg */
#define EVENTFD_MAGIC		0x44523722 /* Anapa */
#define EVENTPOLL_MAGIC		0x45023858 /* Krasnodar */
#define EVENTPOLL_TFD_MAGIC	0x44433746 /* Novorossiysk */
#define SIGNALFD_MAGIC		0x57323820 /* Uglich */
#define INOTIFY_MAGIC		0x48424431 /* Volgograd */
#define INOTIFY_WD_MAGIC	0x54562009 /* Svetlogorsk (Rauschen) */
#define MOUNTPOINTS_MAGIC	0x55563928 /* Petushki */
#define NETDEV_MAGIC		0x57373951 /* Yaroslavl */
#define TTY_MAGIC		0x59433025 /* Pushkin */
#define TTY_INFO_MAGIC		0x59453036 /* Kolpino */
#define FILE_LOCKS_MAGIC	0x54323616 /* Kaluga */
#define RLIMIT_MAGIC		0x57113925 /* Rostov */
#define FANOTIFY_MAGIC		0x55096122 /* Chelyabinsk */
#define FANOTIFY_MARK_MAGIC	0x56506035 /* Yekaterinburg */
#define NETLINKSK_MAGIC		0x58005614 /* Perm */

#define IFADDR_MAGIC		RAW_IMAGE_MAGIC
#define ROUTE_MAGIC		RAW_IMAGE_MAGIC
#define TMPFS_MAGIC		RAW_IMAGE_MAGIC

#endif /* __CR_MAGIC_H__ */