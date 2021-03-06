#ifndef __CR_TUN_H__
#define __CR_TUN_H__

#ifndef TUN_MINOR
#define TUN_MINOR	200
#endif

#include "protobuf/netdev.pb-c.h"

extern const struct fdtype_ops tunfile_dump_ops;
extern int dump_tun_link(NetDeviceEntry *nde, struct cr_fdset *fds);
extern int restore_one_tun(NetDeviceEntry *nde, int nlsk);
extern struct collect_image_info tunfile_cinfo;
extern int check_tun(void);

#endif /* __CR_TUN_H__ */
