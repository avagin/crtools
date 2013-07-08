#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "crtools.h"
#include "asm/types.h"
#include "util.h"
#include "log.h"
#include "mount.h"
#include "proc_parse.h"
#include "image.h"
#include "namespaces.h"
#include "protobuf.h"
#include "protobuf/mnt.pb-c.h"

static struct mount_info *mntinfo;
int mntns_root = -1;

int open_mount(unsigned int s_dev)
{
	struct mount_info *i;

	for (i = mntinfo; i != NULL; i = i->next)
		if (s_dev == i->s_dev)
			return open(i->mountpoint, O_RDONLY);

	return -ENOENT;
}

int collect_mount_info(pid_t pid)
{
	pr_info("Collecting mountinfo\n");

	mntinfo = parse_mountinfo(pid);
	if (!mntinfo) {
		pr_err("Parsing mountinfo %d failed\n", getpid());
		return -1;
	}

	return 0;
}

static struct mount_info *__lookup_mnt_id(struct mount_info *list, int id)
{
	struct mount_info *m;

	for (m = list; m != NULL; m = m->next)
		if (m->mnt_id == id)
			return m;

	return NULL;
}

struct mount_info *lookup_mnt_id(unsigned int id)
{
	return __lookup_mnt_id(mntinfo, id);
}

struct mount_info *lookup_mnt_sdev(unsigned int s_dev)
{
	struct mount_info *m;

	for (m = mntinfo; m != NULL; m = m->next)
		if (m->s_dev == s_dev)
			return m;

	return NULL;
}

/* two bind-mounts are reported as equal if bind is true */
static bool mntcmp(struct mount_info* mi, struct mount_info *c, bool bind)
{
	return (mi->s_dev == c->s_dev &&
		(bind || !strcmp(c->root, mi->root)) &&
		!strcmp(c->source, mi->source) &&
		!strcmp(c->options, mi->options) &&
		c->fstype == mi->fstype);
}

static struct mount_info *mnt_build_ids_tree(struct mount_info *list)
{
	struct mount_info *m, *root = NULL;

	/*
	 * Just resolve the mnt_id:parent_mnt_id relations
	 */

	pr_debug("\tBuilding plain mount tree\n");
	for (m = list; m != NULL; m = m->next) {
		struct mount_info *p;

		pr_debug("\t\tWorking on %d->%d\n", m->mnt_id, m->parent_mnt_id);
		p = __lookup_mnt_id(list, m->parent_mnt_id);
		if (!p) {
			/* This should be / */
			if (root == NULL && !strcmp(m->mountpoint, "/")) {
				root = m;
				continue;
			}

			pr_err("Mountpoint %d w/o parent %d found @%s (root %s)\n",
					m->mnt_id, m->parent_mnt_id, m->mountpoint,
					root ? "found" : "not found");
			return NULL;
		}

		m->parent = p;
		list_add_tail(&m->siblings, &p->children);
	}

	if (!root) {
		pr_err("No root found for tree\n");
		return NULL;
	}

	return root;
}

static int mnt_depth(struct mount_info *m)
{
	int depth = 0;
	char *c;

	for (c = m->mountpoint; *c != '\0'; c++)
		if (*c == '/')
			depth++;

	return depth;
}

static void mnt_resort_siblings(struct mount_info *tree)
{
	struct mount_info *m, *p;
	LIST_HEAD(list);

	/*
	 * Put siblings of each node in an order they can be (u)mounted
	 * I.e. if we have mounts on foo/bar/, foo/bar/foobar/ and foo/
	 * we should put them in the foo/bar/foobar/, foo/bar/, foo/ order.
	 * Otherwise we will not be able to (u)mount them in a sequence.
	 *
	 * Funny, but all we need for this is to sort them in the descending
	 * order of the amount of /-s in a path =)
	 *
	 * Use stupid insertion sort here, we're not expecting mount trees
	 * to contain hundreds (or more) elements.
	 */

	pr_info("\tResorting siblings on %d\n", tree->mnt_id);
	while (!list_empty(&tree->children)) {
		int depth;

		m = list_first_entry(&tree->children, struct mount_info, siblings);
		list_del(&m->siblings);

		depth = mnt_depth(m);
		list_for_each_entry(p, &list, siblings)
			if (mnt_depth(p) <= depth)
				break;

		list_add(&m->siblings, &p->siblings);
		mnt_resort_siblings(m);
	}

	list_splice(&list, &tree->children);
}

static void mnt_tree_show(struct mount_info *tree, int off)
{
	struct mount_info *m;

	pr_info("%*s[%s](%d->%d)\n", off, "",
			tree->mountpoint, tree->mnt_id, tree->parent_mnt_id);

	list_for_each_entry(m, &tree->children, siblings)
		mnt_tree_show(m, off + 1);

	pr_info("%*s<--\n", off, "");
}

static int collect_shared(struct mount_info *info)
{
	struct mount_info *m, *t;

	/*
	 * If we have a shared mounts, both master
	 * slave targets are to be present in mount
	 * list, otherwise we can't be sure if we can
	 * recreate the scheme later on restore.
	 */
	for (m = info; m; m = m->next) {
		bool need_share, need_master;

		need_share = m->shared_id && list_empty(&m->mnt_share);
		need_master = m->master_id;

		for (t = info; t && (need_share || need_master); t = t->next) {
			if (t == m)
				continue;
			if (need_master && t->shared_id == m->master_id) {
				pr_debug("The mount %d is slave for %d\n", m->mnt_id, t->mnt_id);
				list_add(&m->mnt_slave, &t->mnt_slave_list);
				m->mnt_master = t;
				need_master = false;
			}

			/* Collect all mounts from this group */
			if (need_share && t->shared_id == m->shared_id) {
				pr_debug("Mount %d is shared with %d group %d\n",
						m->mnt_id, t->mnt_id, m->shared_id);
				list_add(&t->mnt_share, &m->mnt_share);
			}
		}

		if (need_master) {
			pr_err("Mount %d (master_id: %d shared_id: %d) "
			       "has unreachable sharing\n", m->mnt_id,
				m->master_id, m->shared_id);
			return -1;
		}

		/* Search bind-mounts */
		if (list_empty(&m->mnt_bind))
			for (t = m->next; t; t = t->next) {
				if (mntcmp(m, t, true))
					list_add(&t->mnt_bind, &m->mnt_bind);
			}
	}

	return 0;
}

static struct mount_info *mnt_build_tree(struct mount_info *list)
{
	struct mount_info *tree;

	/*
	 * Organize them in a sequence in which they can be mounted/umounted.
	 */

	pr_info("Building mountpoints tree\n");
	tree = mnt_build_ids_tree(list);
	if (!tree)
		return NULL;

	mnt_resort_siblings(tree);
	collect_shared(tree);
	pr_info("Done:\n");
	mnt_tree_show(tree, 0);
	return tree;
}

static DIR *open_mountpoint(struct mount_info *pm)
{
	int fd, ret;
	char path[PATH_MAX + 1];
	struct stat st;
	DIR *fdir;

	if (!list_empty(&pm->children)) {
		pr_err("Something is mounted on top of %s\n", pm->fstype->name);
		return NULL;
	}

	snprintf(path, sizeof(path), ".%s", pm->mountpoint);
	fd = openat(mntns_root, path, O_RDONLY);
	if (fd < 0) {
		pr_perror("Can't open %s", pm->mountpoint);
		return NULL;
	}

	ret = fstat(fd, &st);
	if (ret < 0) {
		pr_perror("fstat(%s) failed", path);
		close(fd);
		return NULL;
	}

	if (st.st_dev != pm->s_dev) {
		pr_err("The file system %#x %s %s is inaccessible\n",
				pm->s_dev, pm->fstype->name, pm->mountpoint);
		close(fd);
		return NULL;
	}

	fdir = fdopendir(fd);
	if (fdir == NULL) {
		close(fd);
		pr_perror("Can't open %s", pm->mountpoint);
		return NULL;
	}

	return fdir;
}

static int close_mountpoint(DIR *dfd)
{
	if (closedir(dfd)) {
		pr_perror("Unable to close directory");
		return -1;
	}
	return 0;
}

static int tmpfs_dump(struct mount_info *pm)
{
	int ret = -1;
	char tmpfs_path[PATH_MAX];
	int fd, fd_img = -1;
	DIR *fdir = NULL;

	fdir = open_mountpoint(pm);
	if (fdir == NULL)
		return -1;

	fd = dirfd(fdir);
	if (fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) & ~FD_CLOEXEC) == -1) {
		pr_perror("Can not drop FD_CLOEXEC");
		goto out;
	}

	fd_img = open_image(CR_FD_TMPFS, O_DUMP, pm->mnt_id);
	if (fd_img < 0)
		goto out;

	snprintf(tmpfs_path, sizeof(tmpfs_path),
				       "/proc/self/fd/%d", fd);

	ret = cr_system(-1, fd_img, -1, "tar", (char *[])
			{ "tar", "--create",
			"--gzip",
			"--one-file-system",
			"--check-links",
			"--preserve-permissions",
			"--sparse",
			"--numeric-owner",
			"--directory", tmpfs_path, ".", NULL });

	if (ret)
		pr_err("Can't dump tmpfs content\n");

out:
	close_safe(&fd_img);
	close_mountpoint(fdir);
	return ret;
}

static int tmpfs_restore(struct mount_info *pm)
{
	int ret;
	int fd_img;

	fd_img = open_image(CR_FD_TMPFS, O_RSTR, pm->mnt_id);
	if (fd_img < 0)
		return -1;

	ret = cr_system(fd_img, -1, -1, "tar",
			(char *[]) {"tar", "--extract", "--gzip",
				"--directory", pm->mountpoint, NULL});
	close(fd_img);

	if (ret) {
		pr_err("Can't restore tmpfs content\n");
		return -1;
	}

	return 0;
}

static int binfmt_misc_dump(struct mount_info *pm)
{
	int ret = -1;
	struct dirent *de;
	DIR *fdir = NULL;

	fdir = open_mountpoint(pm);
	if (fdir == NULL)
		return -1;

	while ((de = readdir(fdir))) {
		if (dir_dots(de))
			continue;
		if (!strcmp(de->d_name, "register"))
			continue;
		if (!strcmp(de->d_name, "status"))
			continue;

		pr_err("binfmt_misc isn't empty: %s\n", de->d_name);
		goto out;
	}

	ret = 0;
out:
	close_mountpoint(fdir);
	return ret;
}

static struct fstype fstypes[] = {
	[FSTYPE__UNSUPPORTED]	= { "unsupported" },
	[FSTYPE__PROC]		= { "proc" },
	[FSTYPE__SYSFS]		= { "sysfs" },
	[FSTYPE__DEVTMPFS]	= { "devtmpfs" },
	[FSTYPE__BINFMT_MISC]	= { "binfmt_misc", binfmt_misc_dump },
	[FSTYPE__TMPFS]		= { "tmpfs", tmpfs_dump, tmpfs_restore },
	[FSTYPE__DEVPTS]	= { "devpts" },
	[FSTYPE__SIMFS]		= { "simfs" },
};

struct fstype *find_fstype_by_name(char *fst)
{
	int i;

	/*
	 * This fn is required for two things.
	 * 1st -- to check supported filesystems (as just mounting
	 * anything is wrong, almost every fs has its own features)
	 * 2nd -- save some space in the image (since we scan all
	 * names anyway)
	 */

	for (i = 0; i < ARRAY_SIZE(fstypes); i++)
		if (!strcmp(fstypes[i].name, fst))
			return fstypes + i;

	return &fstypes[0];
}

static u32 encode_fstype(struct fstype *fst)
{
	return fst - fstypes;
}

static struct fstype *decode_fstype(u32 fst)
{

	if (fst >= ARRAY_SIZE(fstypes))
		return &fstypes[0];

	return &fstypes[fst];
}

static inline int is_root(char *p)
{
	return p[0] == '/' && p[1] == '\0';
}

static inline int is_root_mount(struct mount_info *mi)
{
	return is_root(mi->mountpoint);
}

static int dump_one_mountpoint(struct mount_info *pm, int fd)
{
	MntEntry me = MNT_ENTRY__INIT;

	pr_info("\t%d: %x:%s @ %s\n", pm->mnt_id, pm->s_dev,
			pm->root, pm->mountpoint);

	me.fstype		= encode_fstype(pm->fstype);
	if (fstypes[me.fstype].dump && fstypes[me.fstype].dump(pm))
		return -1;

	me.mnt_id		= pm->mnt_id;
	me.root_dev		= pm->s_dev;
	me.parent_mnt_id	= pm->parent_mnt_id;
	me.flags		= pm->flags;
	me.root			= pm->root;
	me.mountpoint		= pm->mountpoint;
	me.source		= pm->source;
	me.options		= pm->options;
	me.group		= pm->shared_id;
	me.has_group		= true;
	me.master		= pm->master_id;
	me.has_master		= true;

	if (!me.fstype && !is_root_mount(pm)) {
		pr_err("FS mnt %s dev %#x root %s unsupported\n",
				pm->mountpoint, pm->s_dev, pm->root);
		return -1;
	}

	if (pb_write_one(fd, &me, PB_MOUNTPOINTS))
		return -1;

	return 0;
}

int dump_mnt_ns(int ns_pid, struct cr_fdset *fdset)
{
	struct mount_info *pm;
	int img_fd;

	pm = parse_mountinfo(ns_pid);
	if (!pm) {
		pr_err("Can't parse %d's mountinfo\n", ns_pid);
		return -1;
	}

	if (collect_shared(mntinfo)) {
		pr_err("Can't proceed %d's mountinfo\n", ns_pid);
		return -1;
	}

	pr_info("Dumping mountpoints\n");

	img_fd = fdset_fd(fdset, CR_FD_MOUNTPOINTS);
	do {
		struct mount_info *n = pm->next;

		if (dump_one_mountpoint(pm, img_fd))
			return -1;

		xfree(pm);
		pm = n;
	} while (pm);

	return 0;
}

static int mount_progress;

#define MNT_TREE_WALK(_r, _el, _fn_f, _fn_r, _plist) do {			\
		struct mount_info *_mi = _r;					\
										\
		while (1) {							\
			int ret;						\
										\
			ret = _fn_f(_mi);					\
			if (ret < 0)						\
				return -1;					\
			else if (ret > 0) {					\
				list_add_tail(&_mi->postpone, _plist);		\
				goto up;					\
			}							\
										\
			list_del(&_mi->postpone);				\
			mount_progress++;					\
										\
			if (!list_empty(&_mi->children)) {			\
				_mi = list_entry(_mi->children._el,		\
						struct mount_info, siblings);	\
				continue;					\
			}							\
	up:									\
			if (_fn_r(_mi))						\
				return -1;					\
			if (_mi == _r)						\
				return 0;					\
			if (_mi->siblings._el == &_mi->parent->children) {	\
				_mi = _mi->parent;				\
				goto up;					\
			}							\
			_mi = list_entry(_mi->siblings._el,			\
					struct mount_info, siblings);		\
		}								\
	} while (0)

#define MNT_WALK_NONE	0 &&


static int mnt_tree_for_each(struct mount_info *m,
		int (*fn)(struct mount_info *))
{
	static LIST_HEAD(postpone);

	list_add(&m->postpone, &postpone);

	while (!list_empty(&postpone)) {
		int old = mount_progress;
		list_for_each_entry(m, &postpone, postpone)
			MNT_TREE_WALK(m, next, fn, MNT_WALK_NONE, &postpone);
		if (old == mount_progress) {
			pr_err("A few mount points can't be mounted");
			list_for_each_entry(m, &postpone, postpone) {
				pr_err("%d:%d %s %s %s\n", m->mnt_id,
					m->parent_mnt_id, m->root,
					m->mountpoint, m->source);
			}
			return -1;
		}
	}

	return 0;
}

static int mnt_tree_for_each_reverse(struct mount_info *m,
		int (*fn)(struct mount_info *))
{
	MNT_TREE_WALK(m, prev, MNT_WALK_NONE, fn, (struct list_head *) NULL);
}

static char *resolve_source(struct mount_info *mi)
{
	if (kdev_major(mi->s_dev) == 0)
		/*
		 * Anonymous block device. Kernel creates them for
		 * diskless mounts.
		 */
		return mi->source;

	pr_err("No device for %s mount\n", mi->mountpoint);
	return NULL;
}

static inline int fsroot_mounted(struct mount_info *mi)
{
	return is_root(mi->root);
}

static int propogate_mount(struct mount_info *mi)
{
	struct mount_info *t;

	list_for_each_entry(t, &mi->mnt_share, mnt_share)
		t->bind = mi;

	list_for_each_entry(t, &mi->mnt_slave_list, mnt_slave)
		t->bind = mi;

	/* Mart this mnt as mounted for all who in this group */
	list_for_each_entry(t, &mi->parent->mnt_share, mnt_share) {
		struct mount_info *c;

		list_for_each_entry(c, &t->children, siblings) {
			if (mntcmp(mi, c, false))
				t->mounted = true;
		}
	}

	list_for_each_entry(t, &mi->parent->mnt_slave_list, mnt_slave) {
		struct mount_info *c;
		list_for_each_entry(c, &t->children, siblings) {
			if (mntcmp(mi, c, false))
				t->mounted = true;
		}
	}

	if (fsroot_mounted(mi))
		list_for_each_entry(t, &mi->mnt_bind, mnt_bind) {
			if (t->bind)
				continue;
			if (t->master_id)
				continue;
			t->bind = mi;
		}

	return 0;
}

static int do_new_mount(struct mount_info *mi)
{
	char *src;
	struct fstype *tp = mi->fstype;
	struct mount_info *t;

	src = resolve_source(mi);
	if (!src)
		return -1;

	if (!fsroot_mounted(mi) || (mi->flags & MS_SLAVE)) {
		pr_debug("Postpone %s\n", mi->mountpoint);
		return 1;
	}

	/* Wait while all parent are not mounted */
	/*
	 * FIXME a child is shared only between parents,
	 * who was present in a moment of birth
	 */
	if (mi->parent->flags & MS_SHARED) {
		list_for_each_entry(t, &mi->parent->mnt_share, mnt_share)
			if (!t->mounted) {
				pr_debug("Postpone %s\n", mi->mountpoint);
				return 1;
			}
		list_for_each_entry(t, &mi->parent->mnt_slave_list, mnt_slave)
			if (!t->mounted) {
				pr_debug("Postpone %s\n", mi->mountpoint);
				return 1;
			}
	}

	if (mount(src, mi->mountpoint, tp->name,
			mi->flags & (~MS_SHARED), mi->options) < 0) {
		pr_perror("Can't mount at %s", mi->mountpoint);
		return -1;
	}

	if (mi->flags & MS_SHARED) {
		if (mount(NULL, mi->mountpoint, NULL, MS_SHARED, NULL) < 0) {
			pr_perror("Can't mark %s as shared", mi->mountpoint);
			return -1;
		}
	}

	mi->mounted = true;

	if (tp->restore && tp->restore(mi))
		return -1;

	if (propogate_mount(mi))
		return 0;

	return 0;
}

static int do_bind_mount(struct mount_info *mi)
{
	char rpath[PATH_MAX];

	if (mi->bind == NULL) {
		pr_debug("Postpone %s\n", mi->mountpoint);
		return 1;
	}

	snprintf(rpath, sizeof(rpath), "%s%s", mi->bind->mountpoint, mi->root);

	pr_info("Bind %s to %s\n", rpath, mi->mountpoint);

	if (mount(rpath, mi->mountpoint, NULL,
				MS_BIND, NULL) < 0) {
		pr_perror("Can't mount at %s", mi->mountpoint);
		return -1;
	}

	if ((!(mi->bind->flags & MS_SHARED)) && (mi->flags & MS_SHARED)) {
		pr_debug("share %d %d", mi->bind->flags & MS_SHARED, mi->flags & MS_SHARED);
		if (mount(NULL, mi->mountpoint, NULL, MS_SHARED, NULL) < 0) {
			pr_perror("Can't mark %s as shared", mi->mountpoint);
			return -1;
		}
	}

	if (mi->flags & MS_SLAVE) {
		if (mount(NULL, mi->mountpoint, NULL, MS_SLAVE, NULL) < 0) {
			pr_perror("Can't mark %s as shared", mi->mountpoint);
			return -1;
		}
	}

	if (propogate_mount(mi))
		return -1;

	return 0;
}

static int do_mount_one(struct mount_info *mi)
{
	if (!mi->parent)
		return 0;

	pr_debug("\tMounting %s @%s\n", mi->fstype->name, mi->mountpoint);

	if (!mi->bind)
		return do_new_mount(mi);
	else
		return do_bind_mount(mi);
}

static int do_umount_one(struct mount_info *mi)
{
	if (!mi->parent)
		return 0;

	if (mount("none", mi->parent->mountpoint, "none", MS_REC|MS_PRIVATE, NULL)) {
		pr_perror("Can't mark %s as private", mi->parent->mountpoint);
		return -1;
	}

	if (umount(mi->mountpoint)) {
		pr_perror("Can't umount at %s", mi->mountpoint);
		return -1;
	}

	pr_info("Umounted at %s\n", mi->mountpoint);
	return 0;
}

static int clean_mnt_ns(void)
{
	int ret;
	struct mount_info *pm;

	pr_info("Cleaning mount namespace\n");

	/*
	 * Mountinfos were collected at prepare stage
	 */

	if (mount("none", "/", "none", MS_REC|MS_PRIVATE, NULL)) {
		pr_perror("Can't remount root with MS_PRIVATE");
		return -1;
	}

	pm = mnt_build_tree(mntinfo);
	if (!pm)
		return -1;

	ret = mnt_tree_for_each_reverse(pm, do_umount_one);

	while (mntinfo) {
		pm = mntinfo->next;
		xfree(mntinfo);
		mntinfo = pm;
	}

	return ret;
}

static int cr_pivot_root()
{
	char put_root[] = "crtools-put-root.XXXXXX";

	pr_info("Move the root to %s\n", opts.root);

	if (chdir(opts.root)) {
		pr_perror("chdir(%s) failed", opts.root);
		return -1;
	}
	if (mkdtemp(put_root) == NULL) {
		pr_perror("Can't create a temporary directory");
		return -1;
	}

	if (mount("none", "/", "none", MS_REC|MS_PRIVATE, NULL)) {
		pr_perror("Can't remount root with MS_PRIVATE");
		return -1;
	}

	if (pivot_root(".", put_root)) {
		pr_perror("pivot_root(., %s) failed", put_root);
		if (rmdir(put_root))
			pr_perror("Can't remove the directory %s", put_root);
		return -1;
	}
	if (umount2(put_root, MNT_DETACH)) {
		pr_perror("Can't umount %s", put_root);
		return -1;
	}
	if (rmdir(put_root)) {
		pr_perror("Can't remove the directory %s", put_root);
		return -1;
	}

	return 0;
}

struct mount_info *mnt_entry_alloc()
{
	struct mount_info *new;

	new = xzalloc(sizeof(struct mount_info));
	if (new) {
		INIT_LIST_HEAD(&new->children);
		INIT_LIST_HEAD(&new->siblings);
		INIT_LIST_HEAD(&new->mnt_slave_list);
		INIT_LIST_HEAD(&new->mnt_share);
		INIT_LIST_HEAD(&new->mnt_bind);
		INIT_LIST_HEAD(&new->postpone);
		new->mnt_master = NULL;
	}
	return new;
}

void mnt_entry_free(struct mount_info *mi)
{
	if (mi == NULL)
		return;

	xfree(mi->root);
	xfree(mi->mountpoint);
	xfree(mi->source);
	xfree(mi->options);
	xfree(mi);
}

static int populate_mnt_ns(int ns_pid)
{
	MntEntry *me = NULL;
	int img, ret;
	struct mount_info *pms = NULL;

	pr_info("Populating mount namespace\n");

	img = open_image(CR_FD_MOUNTPOINTS, O_RSTR, ns_pid);
	if (img < 0)
		return -1;

	pr_debug("Reading mountpoint images\n");

	while (1) {
		struct mount_info *pm;

		ret = pb_read_one_eof(img, &me, PB_MOUNTPOINTS);
		if (ret <= 0)
			break;

		pm = mnt_entry_alloc();
		if (!pm)
			goto err;

		pm->next = pms;
		pms = pm;

		pm->mnt_id		= me->mnt_id;
		pm->parent_mnt_id	= me->parent_mnt_id;
		pm->s_dev		= me->root_dev;
		pm->flags		= me->flags;
		pm->shared_id		= me->group;
		pm->master_id		= me->master;

		/* FIXME: abort unsupported early */
		pm->fstype		= decode_fstype(me->fstype);

		pr_debug("\t\tGetting root for %d\n", pm->mnt_id);
		pm->root = xstrdup(me->root);
		if (!pm->root)
			goto err;

		pr_debug("\t\tGetting mpt for %d\n", pm->mnt_id);
		pm->mountpoint = xstrdup(me->mountpoint);
		if (!pm->mountpoint)
			goto err;

		pr_debug("\t\tGetting source for %d\n", pm->mnt_id);
		pm->source = xstrdup(me->source);
		if (!pm->source)
			goto err;

		pr_debug("\t\tGetting opts for %d\n", pm->mnt_id);
		pm->options = xstrdup(me->options);
		if (!pm->options)
			goto err;

		pr_debug("\tRead %d mp @ %s\n", pm->mnt_id, pm->mountpoint);
	}

	if (me)
		mnt_entry__free_unpacked(me, NULL);

	close(img);
	mntinfo = pms;

	pms = mnt_build_tree(pms);
	if (!pms)
		return -1;

	return mnt_tree_for_each(pms, do_mount_one);
err:
	while (pms) {
		struct mount_info *pm = pms;
		pms = pm->next;
		mnt_entry_free(pm);
	}
	close_safe(&img);
	return -1;
}

int prepare_mnt_ns(int ns_pid)
{
	int ret;

	pr_info("Restoring mount namespace\n");

	close_proc();

	/*
	 * The new mount namespace is filled with the mountpoint
	 * clones from the original one. We have to umount them
	 * prior to recreating new ones.
	 */

	if (opts.root)
		ret = cr_pivot_root();
	else
		ret = clean_mnt_ns();

	if (!ret)
		ret = populate_mnt_ns(ns_pid);

	return ret;
}

void show_mountpoints(int fd)
{
	pb_show_plain(fd, PB_MOUNTPOINTS);
}

int mntns_collect_root(pid_t pid)
{
	int fd, pfd;
	int ret;
	char path[PATH_MAX + 1];

	/*
	 * If /proc/pid/root links on '/', it signs that a root of the task
	 * and a root of mntns is the same.
	 */

	pfd = open_pid_proc(pid);
	ret = readlinkat(pfd, "root", path, sizeof(path) - 1);
	if (ret < 0){
		close_pid_proc();
		return ret;
	}

	path[ret] = '\0';

	if (ret != 1 || path[0] != '/') {
		pr_err("The root task has another root than mntns: %s\n", path);
		close_pid_proc();
		return -1;
	}

	fd = openat(pfd, "root", O_RDONLY | O_DIRECTORY, 0);
	close_pid_proc();
	if (fd < 0) {
		pr_perror("Can't open the task root");
		return -1;
	}

	mntns_root = fd;

	return 0;
}

struct ns_desc mnt_ns_desc = NS_DESC_ENTRY(CLONE_NEWNS, "mnt");
