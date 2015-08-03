/*
 * This file is intended only for use by zfs_vnops_osx.c.  It should contain
 * a library of functions useful for vnode operations.
 */
#include <sys/cred.h>
#include <sys/vnode.h>
#include <sys/zfs_dir.h>
#include <sys/zfs_ioctl.h>
#include <sys/fs/zfs.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/spa.h>
#include <sys/txg.h>
#include <sys/dbuf.h>
#include <sys/zap.h>
#include <sys/sa.h>
#include <sys/zfs_vnops.h>
#include <sys/stat.h>

#include <sys/unistd.h>
#include <sys/xattr.h>
#include <sys/utfconv.h>
#include <sys/finderinfo.h>

extern int zfs_vnop_force_formd_normalized_output; /* disabled by default */


/* Originally from illumos:uts/common/sys/vfs.h */
typedef uint64_t vfs_feature_t;
#define	VFSFT_XVATTR		0x100000001	/* Supports xvattr for attrs */
#define	VFSFT_CASEINSENSITIVE	0x100000002	/* Supports case-insensitive */
#define	VFSFT_NOCASESENSITIVE	0x100000004	/* NOT case-sensitive */
#define	VFSFT_DIRENTFLAGS	0x100000008	/* Supports dirent flags */
#define	VFSFT_ACLONCREATE	0x100000010	/* Supports ACL on create */
#define	VFSFT_ACEMASKONACCESS	0x100000020	/* Can use ACEMASK for access */
#define	VFSFT_SYSATTR_VIEWS	0x100000040	/* Supports sysattr view i/f */
#define	VFSFT_ACCESS_FILTER	0x100000080	/* dirents filtered by access */
#define	VFSFT_REPARSE		0x100000100	/* Supports reparse point */
#define	VFSFT_ZEROCOPY_SUPPORTED 0x100000200	/* Supports loaning buffers */

#define	ZFS_SUPPORTED_VATTRS                    \
	( VNODE_ATTR_va_mode |                      \
	  VNODE_ATTR_va_uid |                       \
	  VNODE_ATTR_va_gid |                       \
      VNODE_ATTR_va_fsid |                      \
	  VNODE_ATTR_va_fileid |                    \
	  VNODE_ATTR_va_nlink |                     \
	  VNODE_ATTR_va_data_size |                 \
	  VNODE_ATTR_va_total_size |                \
	  VNODE_ATTR_va_rdev |                      \
	  VNODE_ATTR_va_gen |                       \
	  VNODE_ATTR_va_create_time |               \
	  VNODE_ATTR_va_access_time |               \
	  VNODE_ATTR_va_modify_time |               \
	  VNODE_ATTR_va_change_time |               \
	  VNODE_ATTR_va_backup_time |               \
	  VNODE_ATTR_va_flags |                     \
	  VNODE_ATTR_va_parentid |                  \
	  VNODE_ATTR_va_iosize |                    \
      VNODE_ATTR_va_filerev |                   \
      VNODE_ATTR_va_type    |                   \
      VNODE_ATTR_va_encoding |                  \
      0)
	  //VNODE_ATTR_va_uuuid |
	  //VNODE_ATTR_va_guuid |

/* For part 1 of zfs_getattr() */
int
zfs_getattr_znode_locked(vattr_t *vap, znode_t *zp, cred_t *cr)
{
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	int error;
    uint64_t times[2];
    uint64_t val;

    VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_MODE(zfsvfs),
                     &val, sizeof (val)) == 0);
	vap->va_mode = val & MODEMASK;
    VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_UID(zfsvfs),
                     &val, sizeof (val)) == 0);
	vap->va_uid = val;
    VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_GID(zfsvfs),
                     &val, sizeof (val)) == 0);
	vap->va_gid = val;
	//vap->va_fsid = zp->z_zfsvfs->z_vfs->vfs_dev;

	/* On OS X, the root directory id is always 2 */
	vap->va_fileid = (zp->z_id == zfsvfs->z_root) ? 2 : zp->z_id;

    VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_LINKS(zfsvfs),
                     &val, sizeof (val)) == 0);
	vap->va_nlink = val;

    VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_SIZE(zfsvfs),
                     &val, sizeof (val)) == 0);
	vap->va_data_size = val;
	vap->va_total_size = val;

    VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_RDEV(zfsvfs),
                     &val, sizeof (val)) == 0);
	vap->va_rdev = val;
    VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_GEN(zfsvfs),
                     &val, sizeof (val)) == 0);
	vap->va_gen = val;

    (void) sa_lookup(zp->z_sa_hdl, SA_ZPL_CRTIME(zfsvfs),
                     times, sizeof (times));
	ZFS_TIME_DECODE(&vap->va_create_time, times);
    (void) sa_lookup(zp->z_sa_hdl, SA_ZPL_ATIME(zfsvfs),
                     times, sizeof (times));
	ZFS_TIME_DECODE(&vap->va_access_time, times);
    (void) sa_lookup(zp->z_sa_hdl, SA_ZPL_MTIME(zfsvfs),
                     times, sizeof (times));
	ZFS_TIME_DECODE(&vap->va_modify_time, times);
    (void) sa_lookup(zp->z_sa_hdl, SA_ZPL_CTIME(zfsvfs),
                     times, sizeof (times));
	ZFS_TIME_DECODE(&vap->va_change_time, times);

	if (VATTR_IS_ACTIVE(vap, va_backup_time)) {
		vap->va_backup_time.tv_sec = 0;
		vap->va_backup_time.tv_nsec = 0;
		VATTR_SET_SUPPORTED(vap, va_backup_time);
	}
	vap->va_flags = zfs_getbsdflags(zp);

	/* On OS X, the root directory id is always 2 and its parent is 1 */
    VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_PARENT(zfsvfs),
                     &val, sizeof (val)) == 0);
	if (zp->z_id == zfsvfs->z_root)
		vap->va_parentid = 1;
	else if (val == zfsvfs->z_root)
		vap->va_parentid = 2;
	else
		vap->va_parentid = val;

	vap->va_iosize = zp->z_blksz ? zp->z_blksz : zfsvfs->z_max_blksz;
    VATTR_SET_SUPPORTED(vap, va_iosize);
    printf("stat blksize set to %d\n", vap->va_iosize);

	vap->va_supported |= ZFS_SUPPORTED_VATTRS;

	if (VATTR_IS_ACTIVE(vap, va_nchildren) && vnode_isdir(ZTOV(zp)))
		VATTR_RETURN(vap, va_nchildren, vap->va_nlink - 2);

	if (VATTR_IS_ACTIVE(vap, va_acl)) {

        if ((error = sa_lookup(zp->z_sa_hdl, SA_ZPL_ZNODE_ACL(zfsvfs),
                               times, sizeof (times)))) {
            //		if (zp->z_phys->zp_acl.z_acl_count == 0) {
			vap->va_acl = (kauth_acl_t) KAUTH_FILESEC_NONE;
		} else {
			error = zfs_getacl(zp, &vap->va_acl, B_TRUE, cr);
			if (error)
				return (error);
			VATTR_SET_SUPPORTED(vap, va_acl);
			/*
			 * va_acl implies that va_uuuid and va_guuid are
			 * also supported.
			 */
			VATTR_RETURN(vap, va_uuuid, kauth_null_guid);
			VATTR_RETURN(vap, va_guuid, kauth_null_guid);
		}
	}
	return (0);
}
int
zfs_getattr_znode_unlocked(struct vnode *vp, vattr_t *vap)
{
	znode_t *zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	int error = 0;
	uint64_t	parent;

    //printf("getattr_osx\n");

	ZFS_ENTER(zfsvfs);
	ZFS_VERIFY_ZP(zp);

	if (zp->z_unlinked) {
		dprintf("ZFS: getattr for unlinked!\n");
		ZFS_EXIT(zfsvfs);
		return ENOENT;
	}

	if (VATTR_IS_ACTIVE(vap, va_acl)) {
        //printf("want acl\n");
        VATTR_RETURN(vap, va_uuuid, kauth_null_guid);
        VATTR_RETURN(vap, va_guuid, kauth_null_guid);

        //dprintf("Calling getacl\n");
        if ((error = zfs_getacl(zp, &vap->va_acl, B_FALSE, NULL))) {
            //  dprintf("zfs_getacl returned error %d\n", error);
            error = 0;
        } else {

            VATTR_SET_SUPPORTED(vap, va_acl);
            /* va_acl implies that va_uuuid and va_guuid are also supported. */
            VATTR_RETURN(vap, va_uuuid, kauth_null_guid);
            VATTR_RETURN(vap, va_guuid, kauth_null_guid);
        }

    }

    mutex_enter(&zp->z_lock);

	/*
	 * On Mac OS X we always export the root directory id as 2
	 */
	vap->va_fileid = (zp->z_id == zfsvfs->z_root) ? 2 : zp->z_id;
	//printf("fileid set to %u as z_id %u == z_root %u\n",
	//	   vap->va_fileid, zp->z_id,  zfsvfs->z_root);

	//vap->va_fileid = (zp->z_id == zfsvfs->z_root) ? 2 : zp->z_vid;
	vap->va_data_size = zp->z_size;
	vap->va_total_size = zp->z_size;
	vap->va_gen = zp->z_gen;
	vap->va_gen = 0;

	if (vnode_isdir(vp)) {
		vap->va_nlink = zp->z_size;
	} else {
		vap->va_nlink = zp->z_links;
	}

	/*
	 * For Carbon compatibility,pretend to support this legacy/unused attribute
	 */
	if (VATTR_IS_ACTIVE(vap, va_backup_time)) {
		vap->va_backup_time.tv_sec = 0;
		vap->va_backup_time.tv_nsec = 0;
		VATTR_SET_SUPPORTED(vap, va_backup_time);
    }
	vap->va_flags = zfs_getbsdflags(zp);
	/*
	 * On Mac OS X we always export the root directory id as 2
     * and its parent as 1
	 */
	error = sa_lookup(zp->z_sa_hdl, SA_ZPL_PARENT(zfsvfs),
                      &parent, sizeof (parent));

    if (!error) {
        if (zp->z_id == zfsvfs->z_root) {
			//printf("z_id %u == z_root %u - setting parent 1\n", zp->z_id,
			//	   zfsvfs->z_root);
            vap->va_parentid = 1;
		} else if (parent == zfsvfs->z_root) {
			//printf("parent %u == z_root %u - setting parent 2\n", parent,
			//	   zfsvfs->z_root);
            vap->va_parentid = 2;
		} else {
			//printf("neither, setting parent %u\n", parent);
            vap->va_parentid = parent;
		}
    }

	vap->va_iosize = zp->z_blksz ? zp->z_blksz : zfsvfs->z_max_blksz;
	//vap->va_iosize = 512;
    VATTR_SET_SUPPORTED(vap, va_iosize);

	/* Don't include '.' and '..' in the number of entries */
	if (VATTR_IS_ACTIVE(vap, va_nchildren) && vnode_isdir(vp)) {
		VATTR_RETURN(vap, va_nchildren, vap->va_nlink - 2);
    }

	/*
	 * va_dirlinkcount is the count of directory hard links. When a file
	 * system does not support ATTR_DIR_LINKCOUNT, xnu will default to 1.
	 * Since we claim to support ATTR_DIR_LINKCOUNT both as valid and as
	 * native, we'll just return 1. We set 1 for this value in dirattrpack
	 * as well. If in the future ZFS actually supports directory hard links,
	 * we can return a real value.
	 */
	if (VATTR_IS_ACTIVE(vap, va_dirlinkcount) && vnode_isdir(vp)) {
		VATTR_RETURN(vap, va_dirlinkcount, 1);
    }


	if (VATTR_IS_ACTIVE(vap, va_data_alloc) || VATTR_IS_ACTIVE(vap, va_total_alloc)) {
		uint32_t  blksize;
		u_longlong_t  nblks;
        sa_object_size(zp->z_sa_hdl, &blksize, &nblks);
		vap->va_data_alloc = (uint64_t)512LL * (uint64_t)nblks;
		vap->va_total_alloc = vap->va_data_alloc;
		vap->va_supported |= VNODE_ATTR_va_data_alloc |
            VNODE_ATTR_va_total_alloc;
	}

	if (VATTR_IS_ACTIVE(vap, va_name)) {
        vap->va_name[0] = 0;

        if (!vnode_isvroot(vp)) {
            /*
             * Finder (Carbon) relies on getattr returning the correct name
             * for hardlinks to work, so we store the lookup name in
             * vnop_lookup if file references are high, then set the
             * return name here.
             * If we also want ATTR_CMN_* lookups to work, we need to
             * set a unique va_linkid for each entry, and based on the
             * linkid in the lookup, return the correct name.
             * It is set in zfs_finder_keep_hardlink()
             */

            if ((zp->z_links > 1) && (IFTOVT((mode_t)zp->z_mode) == VREG) &&
                zp->z_finder_hardlink_name[0]) {

                strlcpy(vap->va_name, zp->z_finder_hardlink_name,
                        MAXPATHLEN);
                VATTR_SET_SUPPORTED(vap, va_name);

            } else {

				if (zap_value_search(zfsvfs->z_os, parent, zp->z_id,
									 ZFS_DIRENT_OBJ(-1ULL), vap->va_name) == 0)
					VATTR_SET_SUPPORTED(vap, va_name);

			}

			dprintf("getattr: %p return name '%s':%04llx\n", vp,
				   vap->va_name,
				   vap->va_linkid);

        } else {
            /*
             * The vroot objects must return a unique name for Finder to
             * be able to distringuish between mounts. For this reason
             * we simply return the fullname, from the statfs mountedfrom
             */
			char osname[MAXNAMELEN];
			char *r;
			dmu_objset_name(zfsvfs->z_os, osname);
			r = strrchr(osname, '/');
            strlcpy(vap->va_name,
                    r ? &r[1] : osname,
                    MAXPATHLEN);

            VATTR_SET_SUPPORTED(vap, va_name);
			printf("getattr root returning '%s'\n", vap->va_name);
        }
	}

    if (VATTR_IS_ACTIVE(vap, va_linkid)) {
        VATTR_RETURN(vap, va_linkid, vap->va_fileid);
    }
	if (VATTR_IS_ACTIVE(vap, va_filerev)) {
        VATTR_RETURN(vap, va_filerev, 0);
    }

	/*
	 * kpi_vfs.c: 2355
	 * "
	 * * The fsid can be obtained from the mountpoint directly.
	 * VATTR_RETURN(vap, va_fsid, vp->v_mount->mnt_vfsstat.f_fsid.val[0]);
	 * "
	 * It is pointless to set fsid here, but in case XNU changes in future
	 * we might as well keep it.
	 */
	if (VATTR_IS_ACTIVE(vap, va_fsid)) {
        VATTR_RETURN(vap, va_fsid, zfsvfs->z_rdev);
    }
	if (VATTR_IS_ACTIVE(vap, va_type)) {
        VATTR_RETURN(vap, va_type, vnode_vtype(ZTOV(zp)));
    }
	if (VATTR_IS_ACTIVE(vap, va_encoding)) {
        VATTR_RETURN(vap, va_encoding, kTextEncodingMacUnicode);
    }
#ifdef VNODE_ATTR_va_addedtime
   /* ADDEDTIME should come from finderinfo according to hfs_attrlist.c
	* in ZFS we can use crtime, and add logic to getxattr finderinfo to
	* copy the ADDEDTIME into the structure. See vnop_getxattr
	*/
	if (VATTR_IS_ACTIVE(vap, va_addedtime)) {
		uint64_t addtime[2];
		/* Lookup the ADDTIME if it exists, if not, use CRTIME */
		if (sa_lookup(zp->z_sa_hdl, SA_ZPL_ADDTIME(zfsvfs),
					  &addtime, sizeof (addtime)) != 0) {
			dprintf("ZFS: ADDEDTIME using crtime %llu (error %d)\n",
					vap->va_crtime.tv_sec, error);
			vap->va_addedtime.tv_sec  = vap->va_crtime.tv_sec;
			vap->va_addedtime.tv_nsec = vap->va_crtime.tv_nsec;
		} else {
			dprintf("ZFS: ADDEDTIME using addtime %llu\n",
					addtime[0]);
			ZFS_TIME_DECODE(&vap->va_addedtime, addtime);
		}
        VATTR_SET_SUPPORTED(vap, va_addedtime);
    }
#endif
#ifdef VNODE_ATTR_va_fsid64
	if (VATTR_IS_ACTIVE(vap, va_fsid64)) {
		vap->va_fsid64.val[0] = vfs_statfs(zfsvfs->z_vfs)->f_fsid.val[0];
		vap->va_fsid64.val[1] = vfs_typenum(zfsvfs->z_vfs);
        VATTR_SET_SUPPORTED(vap, va_fsid64);
    }
#endif
#ifdef VNODE_ATTR_va_write_gencount
	if (VATTR_IS_ACTIVE(vap, va_write_gencount)) {
		if (!zp->z_write_gencount)
			atomic_inc_64(&zp->z_write_gencount);
        VATTR_RETURN(vap, va_write_gencount, (uint32_t)zp->z_write_gencount);
    }
#endif

#ifdef VNODE_ATTR_va_document_id
	if (/*VATTR_IS_ACTIVE(vap, va_flags) && (vap->va_flags & UF_TRACKED)
		  &&*/ VATTR_IS_ACTIVE(vap, va_document_id)) {

		/* If they requested document_id, we will go look for it (in case
		 * it was already set before), or, generate a new one.
		 * document_id is generated from PARENT's ID and name then hashed
		 * into a 32bit value.
		 */
		uint64_t docid = 0;
		uint32_t documentid = 0;
		dmu_tx_t *tx;

#if 0 /* Not yet */

		error = sa_lookup(zp->z_sa_hdl, SA_ZPL_DOCUMENTID(zfsvfs),
						  &docid, sizeof (docid));

		if (error || !docid) {
			/* Generate new ID */

#define FNV1_32A_INIT ((uint32_t)0x811c9dc5)
			documentid = fnv_32a_buf(&docid, sizeof(docid), FNV1_32A_INIT);
			/* What if we haven't looked up name above? */
			if (vap->va_name)
				documentid = fnv_32a_str(vap->va_name, documentid);

			printf("ZFS: Generated new ID for %llu '%s' : %08u\n",
				   zp->z_id,
				   vap->va_name ? vap->va_name : "",
				   documentid);

			docid = documentid;  // 32 to 64

			/* Write the new documentid to SA */
			if (zfsvfs->z_use_sa == B_TRUE) {

				tx = dmu_tx_create(zfsvfs->z_os);
				dmu_tx_hold_sa_create(tx, sizeof(docid));
				dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_TRUE);

				error = dmu_tx_assign(tx, TXG_WAIT);
				if (error) {
					dmu_tx_abort(tx);
				} else {
					error = sa_update(zp->z_sa_hdl, SA_ZPL_DOCUMENTID(zfsvfs),
									  &docid, sizeof(docid), tx);
					if (error)
						dmu_tx_abort(tx);
					else
						dmu_tx_commit(tx);
				}

				if (error)
					printf("ZFS: sa_update(SA_ZPL_DOCUMENTID) failed %d\n",
						   error);
			}

			// Clear error so we don't fail getattr
			error = 0;

		} else {
			documentid = docid;  // 64 to 32
		}
#endif


		VATTR_RETURN(vap, va_document_id, documentid);
    }
#endif


#if 0 // Issue #192
	if (VATTR_IS_ACTIVE(vap, va_uuuid)) {
        kauth_cred_uid2guid(zp->z_uid, &vap->va_uuuid);
    }
	if (VATTR_IS_ACTIVE(vap, va_guuid)) {
        kauth_cred_uid2guid(zp->z_gid, &vap->va_guuid);
    }
#endif

	vap->va_supported |= ZFS_SUPPORTED_VATTRS;
	uint64_t missing = 0;
	missing = (vap->va_active ^ (vap->va_active & vap->va_supported));
	if ( missing != 0) {
		dprintf("vnop_getattr:: asked %08llx replied %08llx       missing %08llx\n",
			   vap->va_active, vap->va_supported,
			   missing);
	}

	mutex_exit(&zp->z_lock);

	ZFS_EXIT(zfsvfs);
	return (error);
}

boolean_t
vfs_has_feature(vfs_t *vfsp, vfs_feature_t vfsft)
{

	switch(vfsft) {
	case VFSFT_CASEINSENSITIVE:
	case VFSFT_NOCASESENSITIVE:
		return (B_TRUE);
	default:
		return (B_FALSE);
	}
}


int pn_alloc(pathname_t *p)
{
    return ENOTSUP;
}

int pn_free(pathname_t *p)
{
    return ENOTSUP;
}


int
zfs_access_native_mode(struct vnode *vp, int *mode, cred_t *cr,
                       caller_context_t *ct)
{
	int accmode = *mode & (VREAD|VWRITE|VEXEC/*|VAPPEND*/);
	int error = 0;
    int flag = 0; // FIXME

	if (accmode != 0)
		error = zfs_access(vp, accmode, flag, cr, ct);

	*mode &= ~(accmode);

	return (error);
}

int
zfs_ioflags(int ap_ioflag)
{
	int flags = 0;

	if (ap_ioflag & IO_APPEND)
		flags |= FAPPEND;
	if (ap_ioflag & IO_NDELAY)
		flags |= FNONBLOCK;
	if (ap_ioflag & IO_SYNC)
		flags |= (FSYNC | FDSYNC | FRSYNC);

	return (flags);
}

int
zfs_vnop_ioctl_fullfsync(struct vnode *vp, vfs_context_t ct, zfsvfs_t *zfsvfs)
{
	int error;

    error = zfs_fsync(vp, /*syncflag*/0, NULL, (caller_context_t *)ct);
	if (error)
		return (error);

	if (zfsvfs->z_log != NULL)
		zil_commit(zfsvfs->z_log, 0);
	else
		txg_wait_synced(dmu_objset_pool(zfsvfs->z_os), 0);
	return (0);
}

uint32_t
zfs_getbsdflags(znode_t *zp)
{
	uint32_t  bsdflags = 0;
    uint64_t zflags=0;
    if (zp->z_sa_hdl)
        VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_FLAGS(zp->z_zfsvfs),
                         &zflags, sizeof (zflags)) == 0);

	if (zflags & ZFS_NODUMP)
		bsdflags |= UF_NODUMP;
	if (zflags & ZFS_IMMUTABLE)
		bsdflags |= UF_IMMUTABLE;
	if (zflags & ZFS_APPENDONLY)
		bsdflags |= UF_APPEND;
	if (zflags & ZFS_OPAQUE)
		bsdflags |= UF_OPAQUE;
	if (zflags & ZFS_HIDDEN)
		bsdflags |= UF_HIDDEN;
    /*
     * Due to every file getting archive set automatically, and OSX
     * don't let you move/copy it as a user, we disable archive connection
     * for now
	if (zflags & ZFS_ARCHIVE)
		bsdflags |= SF_ARCHIVED;
    */
    dprintf("getbsd changing zfs %08lx to osx %08lx\n",
           zflags, bsdflags);
	return (bsdflags);
}

void
zfs_setbsdflags(znode_t *zp, uint32_t bsdflags)
{
    uint64_t zflags;
    VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_FLAGS(zp->z_zfsvfs),
                     &zflags, sizeof (zflags)) == 0);

	if (bsdflags & UF_NODUMP)
		zflags |= ZFS_NODUMP;
	else
		zflags &= ~ZFS_NODUMP;

	if (bsdflags & UF_IMMUTABLE)
		zflags |= ZFS_IMMUTABLE;
	else
		zflags &= ~ZFS_IMMUTABLE;

	if (bsdflags & UF_APPEND)
		zflags |= ZFS_APPENDONLY;
	else
		zflags &= ~ZFS_APPENDONLY;

	if (bsdflags & UF_OPAQUE)
		zflags |= ZFS_OPAQUE;
	else
		zflags &= ~ZFS_OPAQUE;

	if (bsdflags & UF_HIDDEN)
		zflags |= ZFS_HIDDEN;
	else
		zflags &= ~ZFS_HIDDEN;

    /*
	if (bsdflags & SF_ARCHIVED)
		zflags |= ZFS_ARCHIVE;
	else
		zflags &= ~ZFS_ARCHIVE;
    */

    zp->z_pflags = zflags;
    dprintf("setbsd changing osx %08lx to zfs %08lx\n",
           bsdflags, zflags);

    /*
      (void )sa_update(zp->z_sa_hdl, SA_ZPL_FLAGS(zp->z_zfsvfs),
      (void *)&zp->z_pflags, sizeof (uint64_t), tx);
    */
}

/*
 * Lookup/Create an extended attribute entry.
 *
 * Input arguments:
 *	dzp	- znode for hidden attribute directory
 *	name	- name of attribute
 *	flag	- ZNEW: if the entry already exists, fail with EEXIST.
 *		  ZEXISTS: if the entry does not exist, fail with ENOENT.
 *
 * Output arguments:
 *	vpp	- pointer to the vnode for the entry (NULL if there isn't one)
 *
 * Return value: 0 on success or errno value on failure.
 */
int
zfs_obtain_xattr(znode_t *dzp, const char *name, mode_t mode, cred_t *cr,
                 vnode_t **vpp, int flag)
{
	znode_t  *xzp = NULL;
	zfsvfs_t  *zfsvfs = dzp->z_zfsvfs;
	zilog_t  *zilog;
	zfs_dirlock_t  *dl;
	dmu_tx_t  *tx;
	struct vnode_attr  vattr;
	int error;
	pathname_t cn = { 0 };
	zfs_acl_ids_t	acl_ids;

	/* zfs_dirent_lock() expects a component name */

    ZFS_ENTER(zfsvfs);
    ZFS_VERIFY_ZP(dzp);
    zilog = zfsvfs->z_log;

	VATTR_INIT(&vattr);
	VATTR_SET(&vattr, va_type, VREG);
	VATTR_SET(&vattr, va_mode, mode & ~S_IFMT);

	if ((error = zfs_acl_ids_create(dzp, 0,
                                    &vattr, cr, NULL, &acl_ids)) != 0) {
		ZFS_EXIT(zfsvfs);
		return (error);
	}

	cn.pn_bufsize = strlen(name)+1;
	cn.pn_buf = (char *)kmem_zalloc(cn.pn_bufsize, KM_SLEEP);


 top:
	/* Lock the attribute entry name. */
	if ( (error = zfs_dirent_lock(&dl, dzp, (char *)name, &xzp, flag,
                                  NULL, &cn)) ) {
		goto out;
	}
	/* If the name already exists, we're done. */
	if (xzp != NULL) {
		zfs_dirent_unlock(dl);
		goto out;
	}
	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_sa(tx, dzp->z_sa_hdl, B_FALSE);
	dmu_tx_hold_zap(tx, dzp->z_id, TRUE, (char *)name);
	dmu_tx_hold_zap(tx, DMU_NEW_OBJECT, FALSE, NULL);

#if 1 // FIXME
	if (dzp->z_pflags & ZFS_INHERIT_ACE) {
		dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0, SPA_MAXBLOCKSIZE);
	}
#endif
    zfs_sa_upgrade_txholds(tx, dzp);
	error = dmu_tx_assign(tx, TXG_NOWAIT);
	if (error) {
		zfs_dirent_unlock(dl);
		if (error == ERESTART) {
			dmu_tx_wait(tx);
			dmu_tx_abort(tx);
			goto top;
		}
		dmu_tx_abort(tx);
		goto out;
	}

	zfs_mknode(dzp, &vattr, tx, cr, 0, &xzp, &acl_ids);

    /*
      ASSERT(xzp->z_id == zoid);
    */
	(void) zfs_link_create(dl, xzp, tx, ZNEW);
	zfs_log_create(zilog, tx, TX_CREATE, dzp, xzp, (char *)name,
                   NULL /* vsecp */, 0 /*acl_ids.z_fuidp*/, &vattr);
    zfs_acl_ids_free(&acl_ids);
	dmu_tx_commit(tx);

	zfs_dirent_unlock(dl);
 out:
    if (cn.pn_buf)
		kmem_free(cn.pn_buf, cn.pn_bufsize);

	/* The REPLACE error if doesn't exist is ENOATTR */
	if ((flag & ZEXISTS) && (error == EEXIST))
		error = ENOATTR;

	if (xzp)
		*vpp = ZTOV(xzp);

    ZFS_EXIT(zfsvfs);
	return (error);
}

/*
 * ace_trivial:
 * determine whether an ace_t acl is trivial
 *
 * Trivialness implies that the acl is composed of only
 * owner, group, everyone entries.  ACL can't
 * have read_acl denied, and write_owner/write_acl/write_attributes
 * can only be owner@ entry.
 */
int
ace_trivial_common(void *acep, int aclcnt,
                   uint64_t (*walk)(void *, uint64_t, int aclcnt,
                                    uint16_t *, uint16_t *, uint32_t *))
{
    return 1;
}


void
acl_trivial_access_masks(mode_t mode, boolean_t isdir, trivial_acl_t *masks)
{
    uint32_t read_mask = ACE_READ_DATA;
    uint32_t write_mask = ACE_WRITE_DATA|ACE_APPEND_DATA;
    uint32_t execute_mask = ACE_EXECUTE;

    (void) isdir;   /* will need this later */

    masks->deny1 = 0;
    if (!(mode & S_IRUSR) && (mode & (S_IRGRP|S_IROTH)))
        masks->deny1 |= read_mask;
    if (!(mode & S_IWUSR) && (mode & (S_IWGRP|S_IWOTH)))
        masks->deny1 |= write_mask;
    if (!(mode & S_IXUSR) && (mode & (S_IXGRP|S_IXOTH)))
        masks->deny1 |= execute_mask;

    masks->deny2 = 0;
    if (!(mode & S_IRGRP) && (mode & S_IROTH))
        masks->deny2 |= read_mask;
    if (!(mode & S_IWGRP) && (mode & S_IWOTH))
        masks->deny2 |= write_mask;
    if (!(mode & S_IXGRP) && (mode & S_IXOTH))
        masks->deny2 |= execute_mask;

    masks->allow0 = 0;
    if ((mode & S_IRUSR) && (!(mode & S_IRGRP) && (mode & S_IROTH)))
        masks->allow0 |= read_mask;
    if ((mode & S_IWUSR) && (!(mode & S_IWGRP) && (mode & S_IWOTH)))
        masks->allow0 |= write_mask;
    if ((mode & S_IXUSR) && (!(mode & S_IXGRP) && (mode & S_IXOTH)))
        masks->allow0 |= execute_mask;

    masks->owner = ACE_WRITE_ATTRIBUTES|ACE_WRITE_OWNER|ACE_WRITE_ACL|
        ACE_WRITE_NAMED_ATTRS|ACE_READ_ACL|ACE_READ_ATTRIBUTES|
        ACE_READ_NAMED_ATTRS|ACE_SYNCHRONIZE;
    if (mode & S_IRUSR)
        masks->owner |= read_mask;
    if (mode & S_IWUSR)
        masks->owner |= write_mask;
    if (mode & S_IXUSR)
        masks->owner |= execute_mask;

    masks->group = ACE_READ_ACL|ACE_READ_ATTRIBUTES|ACE_READ_NAMED_ATTRS|
        ACE_SYNCHRONIZE;
    if (mode & S_IRGRP)
        masks->group |= read_mask;
    if (mode & S_IWGRP)
        masks->group |= write_mask;
    if (mode & S_IXGRP)
        masks->group |= execute_mask;

    masks->everyone = ACE_READ_ACL|ACE_READ_ATTRIBUTES|ACE_READ_NAMED_ATTRS|
        ACE_SYNCHRONIZE;
    if (mode & S_IROTH)
        masks->everyone |= read_mask;
    if (mode & S_IWOTH)
        masks->everyone |= write_mask;
    if (mode & S_IXOTH)
        masks->everyone |= execute_mask;
}

void commonattrpack(attrinfo_t *aip, zfsvfs_t *zfsvfs, znode_t *zp,
                    const char *name, ino64_t objnum, enum vtype vtype,
                    boolean_t user64)
{
	attrgroup_t commonattr = aip->ai_attrlist->commonattr;
	void *attrbufptr = *aip->ai_attrbufpp;
	void *varbufptr = *aip->ai_varbufpp;
	struct mount *mp = zfsvfs->z_vfs;
	cred_t  *cr = (cred_t *)vfs_context_ucred(aip->ai_context);
	finderinfo_t finderinfo;

    /*
     * We should probably combine all the sa_lookup into a bulk
     * lookup operand.
     */

	finderinfo.fi_flags = 0;

	if (ATTR_CMN_NAME & commonattr) {
		nameattrpack(aip, name, strlen(name));
		attrbufptr = *aip->ai_attrbufpp;
		varbufptr = *aip->ai_varbufpp;
	}
	if (ATTR_CMN_DEVID & commonattr) {
		*((dev_t *)attrbufptr) = vfs_statfs(mp)->f_fsid.val[0];
		attrbufptr = ((dev_t *)attrbufptr) + 1;
	}
	if (ATTR_CMN_FSID & commonattr) {
		*((fsid_t *)attrbufptr) = vfs_statfs(mp)->f_fsid;
		attrbufptr = ((fsid_t *)attrbufptr) + 1;
	}
	if (ATTR_CMN_OBJTYPE & commonattr) {
		*((fsobj_type_t *)attrbufptr) = vtype;
		attrbufptr = ((fsobj_type_t *)attrbufptr) + 1;
	}
	if (ATTR_CMN_OBJTAG & commonattr) {
		*((fsobj_tag_t *)attrbufptr) = VT_ZFS;
		attrbufptr = ((fsobj_tag_t *)attrbufptr) + 1;
	}
	/*
	 * Note: ATTR_CMN_OBJID is lossy (only 32 bits).
	 */
	if ((ATTR_CMN_OBJID | ATTR_CMN_OBJPERMANENTID) & commonattr) {
		u_int32_t fileid;
		/*
		 * On Mac OS X we always export the root directory id as 2
		 */
		fileid = (objnum == zfsvfs->z_root) ? 2 : objnum;

		if (ATTR_CMN_OBJID & commonattr) {
			((fsobj_id_t *)attrbufptr)->fid_objno = fileid;
			((fsobj_id_t *)attrbufptr)->fid_generation = 0;
			attrbufptr = ((fsobj_id_t *)attrbufptr) + 1;
		}
		if (ATTR_CMN_OBJPERMANENTID & commonattr) {
			((fsobj_id_t *)attrbufptr)->fid_objno = fileid;
			((fsobj_id_t *)attrbufptr)->fid_generation = 0;
			attrbufptr = ((fsobj_id_t *)attrbufptr) + 1;
		}
	}
	/*
	 * Note: ATTR_CMN_PAROBJID is lossy (only 32 bits).
	 */
	if (ATTR_CMN_PAROBJID & commonattr) {
		uint64_t parentid;

        VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_PARENT(zfsvfs),
                         &parentid, sizeof (parentid)) == 0);

		/*
		 * On Mac OS X we always export the root
		 * directory id as 2 and its parent as 1
		 */
		if (zp && zp->z_id == zfsvfs->z_root)
			parentid = 1;
		else if (parentid == zfsvfs->z_root)
			parentid = 2;

		ASSERT(parentid != 0);

		((fsobj_id_t *)attrbufptr)->fid_objno = (uint32_t)parentid;
		((fsobj_id_t *)attrbufptr)->fid_generation = 0;
		attrbufptr = ((fsobj_id_t *)attrbufptr) + 1;
	}
	if (ATTR_CMN_SCRIPT & commonattr) {
		*((text_encoding_t *)attrbufptr) = kTextEncodingMacUnicode;
		attrbufptr = ((text_encoding_t *)attrbufptr) + 1;
	}
	if (ATTR_CMN_CRTIME & commonattr) {
        uint64_t times[2];
        VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_CRTIME(zfsvfs),
                         times, sizeof(times)) == 0);
		if (user64) {
			ZFS_TIME_DECODE((timespec_user64_t *)attrbufptr,
			                times);
			attrbufptr = ((timespec_user64_t *)attrbufptr) + 1;
		} else {
			ZFS_TIME_DECODE((timespec_user32_t *)attrbufptr,
			                times);
			attrbufptr = ((timespec_user32_t *)attrbufptr) + 1;
		}
	}
	if (ATTR_CMN_MODTIME & commonattr) {
        uint64_t times[2];
        VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_MTIME(zfsvfs),
                         times, sizeof(times)) == 0);
		if (user64) {
			ZFS_TIME_DECODE((timespec_user64_t *)attrbufptr,
			                times);
			attrbufptr = ((timespec_user64_t *)attrbufptr) + 1;
		} else {
			ZFS_TIME_DECODE((timespec_user32_t *)attrbufptr,
			                times);
			attrbufptr = ((timespec_user32_t *)attrbufptr) + 1;
		}
	}
	if (ATTR_CMN_CHGTIME & commonattr) {
        uint64_t times[2];
        VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_CTIME(zfsvfs),
                         times, sizeof(times)) == 0);
		if (user64) {
			ZFS_TIME_DECODE((timespec_user64_t *)attrbufptr,
			                times);
			attrbufptr = ((timespec_user64_t *)attrbufptr) + 1;
		} else {
			ZFS_TIME_DECODE((timespec_user32_t *)attrbufptr,
			                times);
			attrbufptr = ((timespec_user32_t *)attrbufptr) + 1;
		}
	}
	if (ATTR_CMN_ACCTIME & commonattr) {
        uint64_t times[2];
        VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_ATIME(zfsvfs),
                         times, sizeof(times)) == 0);
		if (user64) {
			ZFS_TIME_DECODE((timespec_user64_t *)attrbufptr,
			                times);
			attrbufptr = ((timespec_user64_t *)attrbufptr) + 1;
		} else {
			ZFS_TIME_DECODE((timespec_user32_t *)attrbufptr,
			                times);
			attrbufptr = ((timespec_user32_t *)attrbufptr) + 1;
		}
	}
	if (ATTR_CMN_BKUPTIME & commonattr) {
		/* legacy attribute -- just pass zero */
		if (user64) {
			((timespec_user64_t *)attrbufptr)->tv_sec = 0;
			((timespec_user64_t *)attrbufptr)->tv_nsec = 0;
			attrbufptr = ((timespec_user64_t *)attrbufptr) + 1;
		}  else {
			((timespec_user32_t *)attrbufptr)->tv_sec = 0;
			((timespec_user32_t *)attrbufptr)->tv_nsec = 0;
			attrbufptr = ((timespec_user32_t *)attrbufptr) + 1;
		}
	}
	if (ATTR_CMN_FNDRINFO & commonattr) {
        uint64_t val;
        VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_FLAGS(zfsvfs),
                         &val, sizeof(val)) == 0);
		getfinderinfo(zp, cr, &finderinfo);
		/* Shadow ZFS_HIDDEN to Finder Info's invisible bit */
		if (val & ZFS_HIDDEN) {
			finderinfo.fi_flags |=
				OSSwapHostToBigConstInt16(kIsInvisible);
		}
		bcopy(&finderinfo, attrbufptr, sizeof (finderinfo));
		attrbufptr = (char *)attrbufptr + 32;
	}
	if (ATTR_CMN_OWNERID & commonattr) {
        uint64_t val;
        VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_UID(zfsvfs),
                         &val, sizeof(val)) == 0);
		*((uid_t *)attrbufptr) = val;
		attrbufptr = ((uid_t *)attrbufptr) + 1;
	}
	if (ATTR_CMN_GRPID & commonattr) {
        uint64_t val;
        VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_GID(zfsvfs),
                         &val, sizeof(val)) == 0);
		*((gid_t *)attrbufptr) = val;
		attrbufptr = ((gid_t *)attrbufptr) + 1;
	}
	if (ATTR_CMN_ACCESSMASK & commonattr) {
        uint64_t val;
        VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_MODE(zfsvfs),
                         &val, sizeof(val)) == 0);
		*((u_int32_t *)attrbufptr) = val;
		attrbufptr = ((u_int32_t *)attrbufptr) + 1;
	}
	if (ATTR_CMN_FLAGS & commonattr) {
		u_int32_t flags = zfs_getbsdflags(zp);

		/* Shadow Finder Info's invisible bit to UF_HIDDEN */
		if ((ATTR_CMN_FNDRINFO & commonattr) &&
		    (OSSwapBigToHostInt16(finderinfo.fi_flags) & kIsInvisible))
			flags |= UF_HIDDEN;

		*((u_int32_t *)attrbufptr) = flags;
		attrbufptr = ((u_int32_t *)attrbufptr) + 1;
	}
	if (ATTR_CMN_USERACCESS & commonattr) {
		u_int32_t user_access = 0;
        uint64_t val;
        VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_FLAGS(zfsvfs),
                         &val, sizeof(val)) == 0);

		user_access = getuseraccess(zp, aip->ai_context);

		/* Also consider READ-ONLY file system. */
		if (vfs_flags(mp) & MNT_RDONLY) {
			user_access &= ~W_OK;
		}

		/* Locked objects are not writable either */
		if ((val & ZFS_IMMUTABLE) &&
		    (vfs_context_suser(aip->ai_context) != 0)) {
			user_access &= ~W_OK;
		}

		*((u_int32_t *)attrbufptr) = user_access;
		attrbufptr = ((u_int32_t *)attrbufptr) + 1;
	}
	if (ATTR_CMN_FILEID & commonattr) {
		/*
		 * On Mac OS X we always export the root directory id as 2
		 */
		if (objnum == zfsvfs->z_root)
			objnum = 2;

		*((u_int64_t *)attrbufptr) = objnum;
		attrbufptr = ((u_int64_t *)attrbufptr) + 1;
	}
	if (ATTR_CMN_PARENTID & commonattr) {
		uint64_t parentid;

        VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_PARENT(zfsvfs),
                         &parentid, sizeof (parentid)) == 0);

		/*
		 * On Mac OS X we always export the root
		 * directory id as 2 and its parent as 1
		 */
		if (zp && zp->z_id == zfsvfs->z_root)
			parentid = 1;
		else if (parentid == zfsvfs->z_root)
			parentid = 2;

		ASSERT(parentid != 0);

		*((u_int64_t *)attrbufptr) = parentid;
		attrbufptr = ((u_int64_t *)attrbufptr) + 1;
	}

	*aip->ai_attrbufpp = attrbufptr;
	*aip->ai_varbufpp = varbufptr;
}

void dirattrpack(attrinfo_t *aip, znode_t *zp)
{
	attrgroup_t dirattr = aip->ai_attrlist->dirattr;
	void *attrbufptr = *aip->ai_attrbufpp;

	if (ATTR_DIR_LINKCOUNT & dirattr) {
		*((u_int32_t *)attrbufptr) = 1;  /* no dir hard links */
		attrbufptr = ((u_int32_t *)attrbufptr) + 1;
	}
	if (ATTR_DIR_ENTRYCOUNT & dirattr) {
        uint64_t val;
        VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_SIZE(zp->z_zfsvfs),
                         &val, sizeof(val)) == 0);
		*((u_int32_t *)attrbufptr) = (uint32_t)val;
		attrbufptr = ((u_int32_t *)attrbufptr) + 1;
	}
	if (ATTR_DIR_MOUNTSTATUS & dirattr && zp) {
		vnode_t *vp = ZTOV(zp);

		if (vp != NULL && vnode_mountedhere(vp) != NULL)
			*((u_int32_t *)attrbufptr) = DIR_MNTSTATUS_MNTPOINT;
		else
			*((u_int32_t *)attrbufptr) = 0;
		attrbufptr = ((u_int32_t *)attrbufptr) + 1;
	}
	*aip->ai_attrbufpp = attrbufptr;
}

void fileattrpack(attrinfo_t *aip, zfsvfs_t *zfsvfs, znode_t *zp)
{
	attrgroup_t fileattr = aip->ai_attrlist->fileattr;
	void *attrbufptr = *aip->ai_attrbufpp;
	void *varbufptr = *aip->ai_varbufpp;
	uint64_t allocsize = 0;
	cred_t  *cr = (cred_t *)vfs_context_ucred(aip->ai_context);

	if ((ATTR_FILE_ALLOCSIZE | ATTR_FILE_DATAALLOCSIZE) & fileattr && zp) {
		uint32_t  blksize;
		u_longlong_t  nblks;

		sa_object_size(zp->z_sa_hdl, &blksize, &nblks);
		allocsize = (uint64_t)512LL * (uint64_t)nblks;
	}
	if (ATTR_FILE_LINKCOUNT & fileattr) {
        uint64_t val;
        VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_LINKS(zfsvfs),
                         &val, sizeof(val)) == 0);
		*((u_int32_t *)attrbufptr) = val;
		attrbufptr = ((u_int32_t *)attrbufptr) + 1;
	}
	if (ATTR_FILE_TOTALSIZE & fileattr) {
        uint64_t val;
        VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_SIZE(zfsvfs),
                         &val, sizeof(val)) == 0);
		*((off_t *)attrbufptr) = val;
		attrbufptr = ((off_t *)attrbufptr) + 1;
	}
	if (ATTR_FILE_ALLOCSIZE & fileattr) {
		*((off_t *)attrbufptr) = allocsize;
		attrbufptr = ((off_t *)attrbufptr) + 1;
	}
	if (ATTR_FILE_IOBLOCKSIZE & fileattr && zp) {
		*((u_int32_t *)attrbufptr) =
            zp->z_blksz ? zp->z_blksz : zfsvfs->z_max_blksz;
		attrbufptr = ((u_int32_t *)attrbufptr) + 1;
	}
	if (ATTR_FILE_DEVTYPE & fileattr) {
        uint64_t mode, val=0;
        VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_MODE(zfsvfs),
                         &mode, sizeof(mode)) == 0);
        sa_lookup(zp->z_sa_hdl, SA_ZPL_RDEV(zfsvfs),
                  &val, sizeof(val));
		if (S_ISBLK(mode) || S_ISCHR(mode))
			*((u_int32_t *)attrbufptr) = (u_int32_t)val;
		else
			*((u_int32_t *)attrbufptr) = 0;
		attrbufptr = ((u_int32_t *)attrbufptr) + 1;
	}
	if (ATTR_FILE_DATALENGTH & fileattr) {
        uint64_t val;
        VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_SIZE(zfsvfs),
                         &val, sizeof(val)) == 0);
		*((off_t *)attrbufptr) = val;
		attrbufptr = ((off_t *)attrbufptr) + 1;
	}
	if (ATTR_FILE_DATAALLOCSIZE & fileattr) {
		*((off_t *)attrbufptr) = allocsize;
		attrbufptr = ((off_t *)attrbufptr) + 1;
	}
	if ((ATTR_FILE_RSRCLENGTH | ATTR_FILE_RSRCALLOCSIZE) & fileattr) {
		uint64_t rsrcsize = 0;
        uint64_t xattr;

        if (!sa_lookup(zp->z_sa_hdl, SA_ZPL_XATTR(zfsvfs),
                       &xattr, sizeof(xattr)) &&
            xattr) {
			vnode_t *xdvp = NULLVP;
			vnode_t *xvp = NULLVP;
			pathname_t cn = { 0 };
			char *name = NULL;

			name = spa_strdup(XATTR_RESOURCEFORK_NAME);
			cn.pn_bufsize = strlen(name)+1;
			cn.pn_buf = kmem_zalloc(cn.pn_bufsize, KM_SLEEP);

			/* Grab the hidden attribute directory vnode. */
			if (zfs_get_xattrdir(zp, &xdvp, cr, 0) == 0 &&
			    zfs_dirlook(VTOZ(xdvp), name, &xvp, 0, NULL,
                            &cn) == 0) {
				rsrcsize = VTOZ(xvp)->z_size;
			}
            spa_strfree(name);
			kmem_free(cn.pn_buf, cn.pn_bufsize);

			if (xvp)
				vnode_put(xvp);
			if (xdvp)
				vnode_put(xdvp);
		}
		if (ATTR_FILE_RSRCLENGTH & fileattr) {
			*((off_t *)attrbufptr) = rsrcsize;
			attrbufptr = ((off_t *)attrbufptr) + 1;
		}
		if (ATTR_FILE_RSRCALLOCSIZE & fileattr) {
			*((off_t *)attrbufptr) = roundup(rsrcsize, 512);
			attrbufptr = ((off_t *)attrbufptr) + 1;
		}
	}
	*aip->ai_attrbufpp = attrbufptr;
	*aip->ai_varbufpp = varbufptr;
}

void nameattrpack(attrinfo_t *aip, const char *name, int namelen)
{
	void *varbufptr;
	struct attrreference * attr_refptr;
	u_int32_t attrlen;
	size_t nfdlen, freespace;
	int force_formd_normalized_output;

	varbufptr = *aip->ai_varbufpp;
	attr_refptr = (struct attrreference *)(*aip->ai_attrbufpp);

	freespace = (char*)aip->ai_varbufend - (char*)varbufptr;
	/*
	 * Mac OS X: non-ascii names are UTF-8 NFC on disk
	 * so convert to NFD before exporting them.
	 */

	if (zfs_vnop_force_formd_normalized_output &&
	    !is_ascii_str(name))
		force_formd_normalized_output = 1;
	else
		force_formd_normalized_output = 0;

	namelen = strlen(name);
	if (!force_formd_normalized_output ||
	    utf8_normalizestr((const u_int8_t *)name, namelen,
                          (u_int8_t *)varbufptr, &nfdlen,
                          freespace, UTF_DECOMPOSED) != 0) {
		/* ASCII or normalization failed, just copy zap name. */
		strncpy((char *)varbufptr, name, MIN(freespace, namelen+1));
	} else {
		/* Normalization succeeded (already in buffer). */
		namelen = nfdlen;
	}
	attrlen = namelen + 1;
	attr_refptr->attr_dataoffset = (char *)varbufptr - (char *)attr_refptr;
	attr_refptr->attr_length = attrlen;
	/*
	 * Advance beyond the space just allocated and
	 * round up to the next 4-byte boundary:
	 */
	varbufptr = ((char *)varbufptr) + attrlen + ((4 - (attrlen & 3)) & 3);
	++attr_refptr;

	*aip->ai_attrbufpp = attr_refptr;
	*aip->ai_varbufpp = varbufptr;
}

int getpackedsize(struct attrlist *alp, boolean_t user64)
{
	attrgroup_t attrs;
	int timespecsize;
	int size = 0;

	timespecsize = user64 ? sizeof(timespec_user64_t) :
        sizeof(timespec_user32_t);

	if ((attrs = alp->commonattr) != 0) {
		if (attrs & ATTR_CMN_NAME)
			size += sizeof(struct attrreference);
		if (attrs & ATTR_CMN_DEVID)
			size += sizeof(dev_t);
		if (attrs & ATTR_CMN_FSID)
			size += sizeof(fsid_t);
		if (attrs & ATTR_CMN_OBJTYPE)
			size += sizeof(fsobj_type_t);
		if (attrs & ATTR_CMN_OBJTAG)
			size += sizeof(fsobj_tag_t);
		if (attrs & ATTR_CMN_OBJID)
			size += sizeof(fsobj_id_t);
		if (attrs & ATTR_CMN_OBJPERMANENTID)
			size += sizeof(fsobj_id_t);
		if (attrs & ATTR_CMN_PAROBJID)
			size += sizeof(fsobj_id_t);
		if (attrs & ATTR_CMN_SCRIPT)
			size += sizeof(text_encoding_t);
		if (attrs & ATTR_CMN_CRTIME)
			size += timespecsize;
		if (attrs & ATTR_CMN_MODTIME)
			size += timespecsize;
		if (attrs & ATTR_CMN_CHGTIME)
			size += timespecsize;
		if (attrs & ATTR_CMN_ACCTIME)
			size += timespecsize;
		if (attrs & ATTR_CMN_BKUPTIME)
			size += timespecsize;
		if (attrs & ATTR_CMN_FNDRINFO)
			size += 32 * sizeof(u_int8_t);
		if (attrs & ATTR_CMN_OWNERID)
			size += sizeof(uid_t);
		if (attrs & ATTR_CMN_GRPID)
			size += sizeof(gid_t);
		if (attrs & ATTR_CMN_ACCESSMASK)
			size += sizeof(u_int32_t);
		if (attrs & ATTR_CMN_FLAGS)
			size += sizeof(u_int32_t);
		if (attrs & ATTR_CMN_USERACCESS)
			size += sizeof(u_int32_t);
		if (attrs & ATTR_CMN_FILEID)
			size += sizeof(u_int64_t);
		if (attrs & ATTR_CMN_PARENTID)
			size += sizeof(u_int64_t);
		/*
		 * Also add:
		 * ATTR_CMN_GEN_COUNT         (|FSOPT_ATTR_CMN_EXTENDED)
		 * ATTR_CMN_DOCUMENT_ID       (|FSOPT_ATTR_CMN_EXTENDED)
		 * ATTR_CMN_EXTENDED_SECURITY
		 * ATTR_CMN_UUID
		 * ATTR_CMN_GRPUUID
		 * ATTR_CMN_FULLPATH
		 * ATTR_CMN_ADDEDTIME
		 * ATTR_CMN_ERROR
		 * ATTR_CMN_DATA_PROTECT_FLAGS
		 */
	}
	if ((attrs = alp->dirattr) != 0) {
		if (attrs & ATTR_DIR_LINKCOUNT)
			size += sizeof(u_int32_t);
		if (attrs & ATTR_DIR_ENTRYCOUNT)
			size += sizeof(u_int32_t);
		if (attrs & ATTR_DIR_MOUNTSTATUS)
			size += sizeof(u_int32_t);
	}
	if ((attrs = alp->fileattr) != 0) {
		if (attrs & ATTR_FILE_LINKCOUNT)
			size += sizeof(u_int32_t);
		if (attrs & ATTR_FILE_TOTALSIZE)
			size += sizeof(off_t);
		if (attrs & ATTR_FILE_ALLOCSIZE)
			size += sizeof(off_t);
		if (attrs & ATTR_FILE_IOBLOCKSIZE)
			size += sizeof(u_int32_t);
		if (attrs & ATTR_FILE_DEVTYPE)
			size += sizeof(u_int32_t);
		if (attrs & ATTR_FILE_DATALENGTH)
			size += sizeof(off_t);
		if (attrs & ATTR_FILE_DATAALLOCSIZE)
			size += sizeof(off_t);
		if (attrs & ATTR_FILE_RSRCLENGTH)
			size += sizeof(off_t);
		if (attrs & ATTR_FILE_RSRCALLOCSIZE)
			size += sizeof(off_t);
	}
	return (size);
}


void getfinderinfo(znode_t *zp, cred_t *cr, finderinfo_t *fip)
{
	vnode_t	*xdvp = NULLVP;
	vnode_t	*xvp = NULLVP;
	struct uio		*auio = NULL;
	pathname_t  cn = { 0 };
	int		error;
    uint64_t xattr = 0;
	char *name = NULL;

    if (sa_lookup(zp->z_sa_hdl, SA_ZPL_XATTR(zp->z_zfsvfs),
                   &xattr, sizeof(xattr)) ||
        (xattr == 0)) {
        goto nodata;
    }

	auio = uio_create(1, 0, UIO_SYSSPACE, UIO_READ);
	if (auio == NULL) {
		goto nodata;
	}
	uio_addiov(auio, CAST_USER_ADDR_T(fip), sizeof (finderinfo_t));

	/*
	 * Grab the hidden attribute directory vnode.
	 *
	 * XXX - switch to embedded Finder Info when it becomes available
	 */
	if ((error = zfs_get_xattrdir(zp, &xdvp, cr, 0))) {
		goto out;
	}

	name = spa_strdup(XATTR_FINDERINFO_NAME);
	cn.pn_bufsize = strlen(name)+1;
	cn.pn_buf = kmem_zalloc(cn.pn_bufsize, KM_SLEEP);

	if ((error = zfs_dirlook(VTOZ(xdvp), name, &xvp, 0, NULL, &cn))) {
		goto out;
	}
	error = dmu_read_uio(zp->z_zfsvfs->z_os, VTOZ(xvp)->z_id, auio,
	                     sizeof (finderinfo_t));
out:
    if (name)
        spa_strfree(name);
	if (cn.pn_buf)
		kmem_free(cn.pn_buf, cn.pn_bufsize);
	if (auio)
		uio_free(auio);
	if (xvp)
		vnode_put(xvp);
	if (xdvp)
		vnode_put(xdvp);
	if (error == 0)
		return;
nodata:
	bzero(fip, sizeof (finderinfo_t));
}

#define KAUTH_DIR_WRITE     (KAUTH_VNODE_ACCESS | KAUTH_VNODE_ADD_FILE | \
                             KAUTH_VNODE_ADD_SUBDIRECTORY | \
                             KAUTH_VNODE_DELETE_CHILD)

#define KAUTH_DIR_READ      (KAUTH_VNODE_ACCESS | KAUTH_VNODE_LIST_DIRECTORY)

#define KAUTH_DIR_EXECUTE   (KAUTH_VNODE_ACCESS | KAUTH_VNODE_SEARCH)

#define KAUTH_FILE_WRITE    (KAUTH_VNODE_ACCESS | KAUTH_VNODE_WRITE_DATA)

#define KAUTH_FILE_READ     (KAUTH_VNODE_ACCESS | KAUTH_VNODE_READ_DATA)

#define KAUTH_FILE_EXECUTE  (KAUTH_VNODE_ACCESS | KAUTH_VNODE_EXECUTE)

/*
 * Compute the same user access value as getattrlist(2)
 */
u_int32_t getuseraccess(znode_t *zp, vfs_context_t ctx)
{
	vnode_t	*vp;
	u_int32_t	user_access = 0;
    zfs_acl_phys_t acl_phys;
    int error;
	/* Only take the expensive vnode_authorize path when we have an ACL */

    error = sa_lookup(zp->z_sa_hdl, SA_ZPL_ZNODE_ACL(zp->z_zfsvfs),
                      &acl_phys, sizeof (acl_phys));

	if (error || acl_phys.z_acl_count == 0) {
		kauth_cred_t	cred = vfs_context_ucred(ctx);
		uint64_t		obj_uid;
		uint64_t    	obj_mode;

		/* User id 0 (root) always gets access. */
		if (!vfs_context_suser(ctx)) {
			return (R_OK | W_OK | X_OK);
		}

        sa_lookup(zp->z_sa_hdl, SA_ZPL_UID(zp->z_zfsvfs),
                  &obj_uid, sizeof (obj_uid));
        sa_lookup(zp->z_sa_hdl, SA_ZPL_MODE(zp->z_zfsvfs),
                  &obj_mode, sizeof (obj_mode));

		//obj_uid = pzp->zp_uid;
		obj_mode = obj_mode & MODEMASK;
		if (obj_uid == UNKNOWNUID) {
			obj_uid = kauth_cred_getuid(cred);
		}
		if ((obj_uid == kauth_cred_getuid(cred)) ||
		    (obj_uid == UNKNOWNUID)) {
			return (((u_int32_t)obj_mode & S_IRWXU) >> 6);
		}
		/* Otherwise, settle for 'others' access. */
		return ((u_int32_t)obj_mode & S_IRWXO);
	}
	vp = ZTOV(zp);
	if (vnode_isdir(vp)) {
		if (vnode_authorize(vp, NULLVP, KAUTH_DIR_WRITE, ctx) == 0)
			user_access |= W_OK;
		if (vnode_authorize(vp, NULLVP, KAUTH_DIR_READ, ctx) == 0)
			user_access |= R_OK;
		if (vnode_authorize(vp, NULLVP, KAUTH_DIR_EXECUTE, ctx) == 0)
			user_access |= X_OK;
	} else {
		if (vnode_authorize(vp, NULLVP, KAUTH_FILE_WRITE, ctx) == 0)
			user_access |= W_OK;
		if (vnode_authorize(vp, NULLVP, KAUTH_FILE_READ, ctx) == 0)
			user_access |= R_OK;
		if (vnode_authorize(vp, NULLVP, KAUTH_FILE_EXECUTE, ctx) == 0)
			user_access |= X_OK;
	}
	return (user_access);
}



static unsigned char fingerprint[] = {0xab, 0xcd, 0xef, 0xab, 0xcd, 0xef,
                                      0xab, 0xcd, 0xef, 0xab, 0xcd, 0xef};

/*
 * Convert "Well Known" GUID to enum type.
 */
int kauth_wellknown_guid(guid_t *guid)
{
    uint32_t last = 0;

    if (memcmp(fingerprint, guid->g_guid, sizeof(fingerprint)))
        return KAUTH_WKG_NOT;

    last = BE_32(*((u_int32_t *)&guid->g_guid[12]));

    switch(last) {
    case 0x0c:
        return KAUTH_WKG_EVERYBODY;
    case 0x0a:
        return KAUTH_WKG_OWNER;
    case 0x10:
        return KAUTH_WKG_GROUP;
    case 0xFFFFFFFE:
        return KAUTH_WKG_NOBODY;
    }

    return KAUTH_WKG_NOT;
}


/*
 * Set GUID to "well known" guid, based on enum type
 */
void nfsacl_set_wellknown(int wkg, guid_t *guid)
{
    /*
     * All WKGs begin with the same 12 bytes.
     */
    bcopy(fingerprint, (void *)guid, 12);
    /*
     * The final 4 bytes are our code (in network byte order).
     */
    switch (wkg) {
    case 4:
        *((u_int32_t *)&guid->g_guid[12]) = BE_32(0x0000000c);
        break;
    case 3:
        *((u_int32_t *)&guid->g_guid[12]) = BE_32(0xfffffffe);
        break;
    case 1:
        *((u_int32_t *)&guid->g_guid[12]) = BE_32(0x0000000a);
        break;
    case 2:
        *((u_int32_t *)&guid->g_guid[12]) = BE_32(0x00000010);
    };
}


/*
 * Convert Darwin ACL list, into ZFS ACL "aces" list.
 */
void aces_from_acl(ace_t *aces, int *nentries, struct kauth_acl *k_acl)
{
    int i;
    ace_t *ace;
    guid_t          *guidp;
    kauth_ace_rights_t  ace_rights;
    uid_t  who;
    uint32_t  mask = 0;
    uint16_t  flags = 0;
    uint16_t  type = 0;
    u_int32_t  ace_flags;
    int wkg;

    *nentries = k_acl->acl_entrycount;

    bzero(aces, sizeof(*aces) * *nentries);

    //*nentries = aclp->acl_cnt;

    for (i = 0; i < *nentries; i++) {
        //entry = &(aclp->acl_entry[i]);
        dprintf("aces %d\n", i);

        flags = 0;
        mask  = 0;


        ace = &(aces[i]);

        /* Note Mac OS X GUID is a 128-bit identifier */
        guidp = &k_acl->acl_ace[i].ace_applicable;

        who = -1;
        wkg = kauth_wellknown_guid(guidp);
        switch(wkg) {
        case KAUTH_WKG_OWNER:
            flags |= ACE_OWNER;
            break;
        case KAUTH_WKG_GROUP:
            flags |= ACE_GROUP|ACE_IDENTIFIER_GROUP;
            break;
        case KAUTH_WKG_EVERYBODY:
            flags |= ACE_EVERYONE;
            break;

        case KAUTH_WKG_NOBODY:
        default:
            /* Try to get a uid from supplied guid */
            if (kauth_cred_guid2uid(guidp, &who) != 0) {
                /* If we couldn't generate a uid, try for a gid */
                if (kauth_cred_guid2gid(guidp, &who) != 0) {
                    *nentries=0;
                    dprintf("returning due to guid2gid\n");
                    return;
                }
            }
        }

        ace->a_who = who;

        ace_rights = k_acl->acl_ace[i].ace_rights;
        if (ace_rights & KAUTH_VNODE_READ_DATA)
            mask |= ACE_READ_DATA;
        if (ace_rights & KAUTH_VNODE_WRITE_DATA)
            mask |= ACE_WRITE_DATA;
        if (ace_rights & KAUTH_VNODE_APPEND_DATA)
            mask |= ACE_APPEND_DATA;
        if (ace_rights & KAUTH_VNODE_READ_EXTATTRIBUTES)
            mask |= ACE_READ_NAMED_ATTRS;
        if (ace_rights & KAUTH_VNODE_WRITE_EXTATTRIBUTES)
            mask |= ACE_WRITE_NAMED_ATTRS;
        if (ace_rights & KAUTH_VNODE_EXECUTE)
            mask |= ACE_EXECUTE;
        if (ace_rights & KAUTH_VNODE_DELETE_CHILD)
            mask |= ACE_DELETE_CHILD;
        if (ace_rights & KAUTH_VNODE_READ_ATTRIBUTES)
            mask |= ACE_READ_ATTRIBUTES;
        if (ace_rights & KAUTH_VNODE_WRITE_ATTRIBUTES)
            mask |= ACE_WRITE_ATTRIBUTES;
        if (ace_rights & KAUTH_VNODE_DELETE)
            mask |= ACE_DELETE;
        if (ace_rights & KAUTH_VNODE_READ_SECURITY)
            mask |= ACE_READ_ACL;
        if (ace_rights & KAUTH_VNODE_WRITE_SECURITY)
            mask |= ACE_WRITE_ACL;
        if (ace_rights & KAUTH_VNODE_TAKE_OWNERSHIP)
            mask |= ACE_WRITE_OWNER;
        if (ace_rights & KAUTH_VNODE_SYNCHRONIZE)
            mask |= ACE_SYNCHRONIZE;
        ace->a_access_mask = mask;

        ace_flags = k_acl->acl_ace[i].ace_flags;
        if (ace_flags & KAUTH_ACE_FILE_INHERIT)
            flags |= ACE_FILE_INHERIT_ACE;
        if (ace_flags & KAUTH_ACE_DIRECTORY_INHERIT)
            flags |= ACE_DIRECTORY_INHERIT_ACE;
        if (ace_flags & KAUTH_ACE_LIMIT_INHERIT)
            flags |= ACE_NO_PROPAGATE_INHERIT_ACE;
        if (ace_flags & KAUTH_ACE_ONLY_INHERIT)
            flags |= ACE_INHERIT_ONLY_ACE;
        ace->a_flags = flags;

        switch(ace_flags & KAUTH_ACE_KINDMASK) {
        case KAUTH_ACE_PERMIT:
            type = ACE_ACCESS_ALLOWED_ACE_TYPE;
            break;
        case KAUTH_ACE_DENY:
            type = ACE_ACCESS_DENIED_ACE_TYPE;
            break;
        case KAUTH_ACE_AUDIT:
            type = ACE_SYSTEM_AUDIT_ACE_TYPE;
            break;
        case KAUTH_ACE_ALARM:
            type = ACE_SYSTEM_ALARM_ACE_TYPE;
            break;
        }
        ace->a_type = type;
        dprintf("  ACL: %d type %04x, mask %04x, flags %04x, who %d\n",
               i, type, mask, flags, who);
    }

}

void finderinfo_update(uint8_t *finderinfo, znode_t *zp)
{
	u_int8_t *finfo = NULL;
	uint64_t crtime[2];
	uint64_t addtime[2];
	struct timespec va_crtime;
	//static u_int32_t emptyfinfo[8] = {0};


	finfo = (u_int8_t *)finderinfo + 16;

	if (IFTOVT((mode_t)zp->z_mode) == VLNK) {
		struct FndrFileInfo *fip;

		fip = (struct FndrFileInfo *)finderinfo;
		fip->fdType = 0;
		fip->fdCreator = 0;
	}

	/* Lookup the ADDTIME if it exists, if not, use CRTIME */
	/* change this into bulk */
	sa_lookup(zp->z_sa_hdl, SA_ZPL_CRTIME(zp->z_zfsvfs), crtime, sizeof(crtime));

	if (sa_lookup(zp->z_sa_hdl, SA_ZPL_ADDTIME(zp->z_zfsvfs), &addtime,
				  sizeof (addtime)) != 0) {
		ZFS_TIME_DECODE(&va_crtime, crtime);
	} else {
		ZFS_TIME_DECODE(&va_crtime, addtime);
	}

	if (IFTOVT((mode_t)zp->z_mode) == VREG) {
		struct FndrExtendedFileInfo *extinfo =
			(struct FndrExtendedFileInfo *)finfo;
		extinfo->date_added = 0;

		/* listxattr shouldnt list it either if empty, fixme.
		   if (bcmp((const void *)finderinfo, emptyfinfo,
		   sizeof(emptyfinfo)) == 0)
		   error = ENOATTR;
		*/

		extinfo->date_added = OSSwapBigToHostInt32(va_crtime.tv_sec);
	}
	if (IFTOVT((mode_t)zp->z_mode) == VDIR) {
		struct FndrExtendedDirInfo *extinfo =
			(struct FndrExtendedDirInfo *)finfo;
		extinfo->date_added = 0;

		/*
		  if (bcmp((const void *)finderinfo, emptyfinfo,
		  sizeof(emptyfinfo)) == 0)
		  error = ENOATTR;
		*/

		extinfo->date_added = OSSwapBigToHostInt32(va_crtime.tv_sec);
	}

}



int
zpl_xattr_set_sa(struct vnode *vp, const char *name, const void *value,
				 size_t size, int flags, cred_t *cr)
{
	znode_t *zp = VTOZ(vp);
	nvlist_t *nvl;
	size_t sa_size;
	int error;

	ASSERT(zp->z_xattr_cached);
	nvl = zp->z_xattr_cached;

	if (value == NULL) {
		error = -nvlist_remove(nvl, name, DATA_TYPE_BYTE_ARRAY);
		if (error == -ENOENT)
			return error;
		//error = zpl_xattr_set_dir(vp, name, NULL, 0, flags, cr);
        } else {
                /* Limited to 32k to keep nvpair memory allocations small */
                if (size > DXATTR_MAX_ENTRY_SIZE)
                        return (-EFBIG);

                /* Prevent the DXATTR SA from consuming the entire SA region */
                error = -nvlist_size(nvl, &sa_size, NV_ENCODE_XDR);
                if (error)
                        return (error);

                if (sa_size > DXATTR_MAX_SA_SIZE)
                        return (-EFBIG);
                error = -nvlist_add_byte_array(nvl, name,
                    (uchar_t *)value, size);
                if (error)
                        return (error);
        }

        /* Update the SA for additions, modifications, and removals. */
        if (!error)
                error = -zfs_sa_set_xattr(zp);

        ASSERT3S(error, <=, 0);

        return (error);
}

int
zpl_xattr_get_sa(struct vnode *vp, const char *name, void *value, size_t size)
{
	znode_t *zp = VTOZ(vp);
	uchar_t *nv_value;
	uint_t nv_size;
	int error = 0;

	ASSERT(RW_LOCK_HELD(&zp->z_xattr_lock));

	mutex_enter(&zp->z_lock);
	if (zp->z_xattr_cached == NULL)
		error = -zfs_sa_get_xattr(zp);
	mutex_exit(&zp->z_lock);

	if (error)
		return (error);

	ASSERT(zp->z_xattr_cached);
	error = -nvlist_lookup_byte_array(zp->z_xattr_cached, name,
									  &nv_value, &nv_size);
	if (error)
		return (error);

	if (!size)
		return (nv_size);
	if (size < nv_size)
		return (-ERANGE);

	memcpy(value, nv_value, nv_size);

	return (nv_size);
}
