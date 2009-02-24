#include <linux/namei.h>
#include <linux/sched.h>

#include "ceph_debug.h"

int ceph_debug_dir = -1;
#define DOUT_MASK DOUT_MASK_DIR
#define DOUT_VAR ceph_debug_dir
#include "super.h"

/*
 * Ceph MDS operations are specified in terms of a base ino and
 * relative path.  Thus, the client can specify an operation on a
 * specific inode (e.g., a getattr due to fstat(2)), or as a path
 * relative to, say, the root directory.
 *
 * Because the MDS does not statefully track which inodes the client
 * has in its cache, the client has to take care to only specify
 * operations relative to inodes it knows the MDS has cached.  (The
 * MDS cannot do a lookup by ino.)
 *
 * So, in general, we try to specify operations in terms of generate
 * path names relative to the root.
 */

const struct inode_operations ceph_dir_iops;
const struct file_operations ceph_dir_fops;
struct dentry_operations ceph_dentry_ops;

static int ceph_dentry_revalidate(struct dentry *dentry, struct nameidata *nd);

/*
 * for readdir, encoding the directory frag and offset within that frag
 * into f_pos.
 */
static loff_t make_fpos(unsigned frag, unsigned off)
{
	return ((loff_t)frag << 32) | (loff_t)off;
}
static unsigned fpos_frag(loff_t p)
{
	return p >> 32;
}
static unsigned fpos_off(loff_t p)
{
	return p & 0xffffffff;
}

static int ceph_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct ceph_file_info *fi = filp->private_data;
	struct inode *inode = filp->f_dentry->d_inode;
	struct ceph_inode_info *ci = ceph_inode(inode);
	struct ceph_mds_client *mdsc = &ceph_inode_to_client(inode)->mdsc;
	unsigned frag = fpos_frag(filp->f_pos);
	unsigned off = fpos_off(filp->f_pos);
	unsigned skew;
	int err;
	u32 ftype;
	struct ceph_mds_reply_info_parsed *rinfo;

	/* set I_READDIR at start of readdir */
	if (filp->f_pos == 0)
		ceph_i_set(inode, CEPH_I_READDIR);

nextfrag:
	dout(5, "readdir filp %p at frag %u off %u\n", filp, frag, off);

	/* do we have the correct frag content buffered? */
	if (fi->frag != frag || fi->last_readdir == NULL) {
		struct ceph_mds_request *req;
		int op = ceph_snap(inode) == CEPH_SNAPDIR ?
			CEPH_MDS_OP_LSSNAP : CEPH_MDS_OP_READDIR;

		/* discard old result, if any */
		if (fi->last_readdir)
			ceph_mdsc_put_request(fi->last_readdir);

		/* requery frag tree, as the frag topology may have changed */
		frag = ceph_choose_frag(ceph_inode(inode), frag, NULL, NULL);

		dout(10, "readdir querying mds for ino %llx.%llx frag %x\n",
		     ceph_vinop(inode), frag);
		req = ceph_mdsc_create_request(mdsc, op, filp->f_dentry, NULL,
					       NULL, NULL, USE_AUTH_MDS);
		if (IS_ERR(req))
			return PTR_ERR(req);
		/* hints to request -> mds selection code */
		req->r_direct_mode = USE_AUTH_MDS;
		req->r_direct_hash = frag_value(frag);
		req->r_direct_is_hash = true;
		req->r_args.readdir.frag = cpu_to_le32(frag);
		err = ceph_mdsc_do_request(mdsc, req, NULL);
		if (err < 0) {
			ceph_mdsc_put_request(req);
			return err;
		}
		dout(10, "readdir got and parsed readdir result=%d"
		     " on frag %x\n", err, frag);
		fi->last_readdir = req;
	}

	/* include . and .. with first fragment */
	if (frag_is_leftmost(frag)) {
		switch (off) {
		case 0:
			dout(10, "readdir off 0 -> '.'\n");
			if (filldir(dirent, ".", 1, make_fpos(0, 0),
				    inode->i_ino, inode->i_mode >> 12) < 0)
				return 0;
			off++;
			filp->f_pos++;
		case 1:
			dout(10, "readdir off 1 -> '..'\n");
			if (filp->f_dentry->d_parent != NULL &&
			    filldir(dirent, "..", 2, make_fpos(0, 1),
				    filp->f_dentry->d_parent->d_inode->i_ino,
				    inode->i_mode >> 12) < 0)
				return 0;
			off++;
			filp->f_pos++;
		}
		skew = -2;  /* compensate for . and .. */
	} else {
		skew = 0;
	}

	rinfo = &fi->last_readdir->r_reply_info;
	dout(10, "readdir frag %x num %d off %d skew %d\n", frag,
	     rinfo->dir_nr, off, skew);
	while (off+skew < rinfo->dir_nr) {
		dout(10, "readdir off %d -> %d / %d name '%.*s'\n",
		     off, off+skew,
		     rinfo->dir_nr, rinfo->dir_dname_len[off+skew],
		     rinfo->dir_dname[off+skew]);
		ftype = le32_to_cpu(rinfo->dir_in[off+skew].in->mode) >> 12;
		if (filldir(dirent,
			    rinfo->dir_dname[off+skew],
			    rinfo->dir_dname_len[off+skew],
			    make_fpos(frag, off),
			    le64_to_cpu(rinfo->dir_in[off+skew].in->ino),
			    ftype) < 0) {
			dout(20, "filldir stopping us...\n");
			return 0;
		}
		off++;
		filp->f_pos++;
	}

	/* more frags? */
	if (!frag_is_rightmost(frag)) {
		frag = frag_next(frag);
		off = 0;
		filp->f_pos = make_fpos(frag, off);
		dout(10, "readdir next frag is %x\n", frag);
		goto nextfrag;
	}

	/*
	 * if I_READDIR is still set, no dentries were released
	 * during the whole readdir, and we should have the complete
	 * dir contents in our cache.
	 */
	spin_lock(&inode->i_lock);
	if (ci->i_ceph_flags & CEPH_I_READDIR) {
		dout(10, " marking %p complete\n", inode);
		ci->i_ceph_flags |= CEPH_I_COMPLETE;
		ci->i_ceph_flags &= ~CEPH_I_READDIR;
	}
	spin_unlock(&inode->i_lock);

	dout(20, "readdir done.\n");
	return 0;
}

static loff_t ceph_dir_llseek(struct file *file, loff_t offset, int origin)
{
	struct ceph_file_info *fi = file->private_data;
	struct inode *inode = file->f_mapping->host;
	loff_t old_offset = offset;
	loff_t retval;

	mutex_lock(&inode->i_mutex);
	switch (origin) {
	case SEEK_END:
		offset += inode->i_size;
		break;
	case SEEK_CUR:
		offset += file->f_pos;
	}
	retval = -EINVAL;
	if (offset >= 0 && offset <= inode->i_sb->s_maxbytes) {
		if (offset != file->f_pos) {
			file->f_pos = offset;
			file->f_version = 0;
		}
		retval = offset;
		/*
		 * discard buffered readdir content on seekdir(0)
		 */
		if (offset == 0 && fi->last_readdir) {
			dout(10, "dir_llseek dropping %p content\n", file);
			ceph_mdsc_put_request(fi->last_readdir);
			fi->last_readdir = NULL;
		}

		/* clear I_READDIR if we did a forward seek */
		if (offset > old_offset)
			ceph_inode(inode)->i_ceph_flags &= ~CEPH_I_READDIR;
	}
	mutex_unlock(&inode->i_mutex);
	return retval;
}


/*
 * Process result of a lookup/open request.
 *
 * Mainly, make sure that return r_last_dentry (the dentry
 * the MDS trace ended on) in place of the original VFS-provided
 * dentry, if they differ.
 *
 * Gracefully handle the case where the MDS replies with -ENOENT and
 * no trace (which it may do, at its discretion, e.g., if it doesn't
 * care to issue a lease on the negative dentry).
 */
struct dentry *ceph_finish_lookup(struct ceph_mds_request *req,
				  struct dentry *dentry, int err)
{
	struct ceph_client *client = ceph_client(dentry->d_sb);
	struct inode *parent = dentry->d_parent->d_inode;

	/* snap dir? */
	if (err == -ENOENT &&
	    ceph_vino(parent).ino != CEPH_INO_ROOT && /* no .snap in root dir */
	    strcmp(dentry->d_name.name, client->mount_args.snapdir_name) == 0) {
		struct inode *inode = ceph_get_snapdir(parent);
		dout(10, "ENOENT on snapdir %p '%.*s', linking to snapdir %p\n",
		     dentry, dentry->d_name.len, dentry->d_name.name, inode);
		d_add(dentry, inode);
		err = 0;
	}

	if (err == -ENOENT) {
		/* no trace? */
		if (req->r_reply_info.trace_numd == 0) {
			dout(20, "ENOENT and no trace, dentry %p inode %p\n",
			     dentry, dentry->d_inode);
			ceph_init_dentry(dentry);
			if (dentry->d_inode) {
				d_drop(dentry);
				req->r_dentry = d_alloc(dentry->d_parent,
							&dentry->d_name);
				d_rehash(req->r_dentry);
			} else {
				d_add(dentry, NULL);
			}
		}
		err = 0;
	}
	if (err)
		dentry = ERR_PTR(err);
	else if (dentry != req->r_dentry)
		dentry = dget(req->r_dentry);   /* we got spliced */
	else
		dentry = NULL;
	return dentry;
}

/*
 * Do a lookup / lstat (same thing, modulo the metadata @mask).
 * @on_inode indicates that we should stat the ino directly, and not a
 * path built from @dentry.
 */
struct dentry *ceph_do_lookup(struct super_block *sb, struct dentry *dentry,
			      int mask, int on_inode)
{
	struct ceph_client *client = ceph_sb_to_client(sb);
	struct ceph_mds_client *mdsc = &client->mdsc;
	struct ceph_mds_request *req;
	struct inode *dir = dentry->d_parent->d_inode;
	int err;

	if (dentry->d_name.len > NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);

	/* can we conclude ENOENT locally? */
	if (dentry->d_inode == NULL) {
		struct ceph_inode_info *ci = ceph_inode(dir);

		spin_lock(&dir->i_lock);
		dout(10, "%p flags are %d\n", dir, ci->i_ceph_flags);
		if (ceph_ino(dir) != CEPH_INO_ROOT &&
		    (ci->i_ceph_flags & CEPH_I_COMPLETE) &&
		    (__ceph_caps_issued(ci, NULL) & CEPH_CAP_FILE_RDCACHE)) {
			spin_unlock(&dir->i_lock);
			dout(10, "do_lookup %p dir %p complete, -ENOENT\n",
			     dentry, dir);
			ceph_init_dentry(dentry);
			d_add(dentry, NULL);
			dentry->d_time = ci->i_version;
			return NULL;
		}
		spin_unlock(&dir->i_lock);
	}

	dout(10, "do_lookup %p mask %d\n", dentry, mask);
	req = ceph_mdsc_create_request(mdsc, CEPH_MDS_OP_LSTAT,
				       dentry, NULL, NULL, NULL,
				       on_inode ? USE_CAP_MDS:USE_ANY_MDS);
	if (IS_ERR(req))
		return ERR_PTR(PTR_ERR(req));
	req->r_args.stat.mask = cpu_to_le32(mask);
	req->r_locked_dir = dentry->d_parent->d_inode;  /* by the VFS */
	err = ceph_mdsc_do_request(mdsc, req, NULL);
	dentry = ceph_finish_lookup(req, dentry, err);
	ceph_mdsc_put_request(req);  /* will dput(dentry) */
	dout(20, "do_lookup result=%p\n", dentry);
	return dentry;
}

/*
 * Try to do a lookup+open, if possible.
 */
static struct dentry *ceph_lookup(struct inode *dir, struct dentry *dentry,
				  struct nameidata *nd)
{
	dout(5, "lookup in dir %p dentry %p '%.*s'\n",
	     dir, dentry, dentry->d_name.len, dentry->d_name.name);

	if (dentry->d_inode) {
		switch (dentry->d_inode->i_mode) {
			case S_IFREG:
			case S_IFLNK:
			case S_IFDIR:
				/* open (but not create!) intent? */
				if (nd &&
				    (nd->flags & LOOKUP_OPEN) &&
				    (nd->flags & LOOKUP_CONTINUE) == 0 && /* only open last component */
				    !(nd->intent.open.flags & O_CREAT)) {
					int mode = nd->intent.open.create_mode & ~current->fs->umask;
					return ceph_lookup_open(dir, dentry, nd, mode);
				}

		}
	}

	return ceph_do_lookup(dir->i_sb, dentry, CEPH_STAT_CAP_INODE, 0);
}

static int ceph_mknod(struct inode *dir, struct dentry *dentry,
		      int mode, dev_t rdev)
{
	struct ceph_client *client = ceph_sb_to_client(dir->i_sb);
	struct ceph_mds_client *mdsc = &client->mdsc;
	struct ceph_mds_request *req;
	int err;
	int issued = ceph_caps_issued(ceph_inode(dir));

	if (ceph_snap(dir) != CEPH_NOSNAP)
		return -EROFS;

	dout(5, "mknod in dir %p dentry %p mode 0%o rdev %d\n",
	     dir, dentry, mode, rdev);
	if (ceph_async_create(dir, dentry, issued, mode, NULL) == 0) {
		dentry->d_inode->i_rdev = rdev;
		return 0;
	}

	req = ceph_mdsc_create_request(mdsc, CEPH_MDS_OP_MKNOD, dentry, NULL,
				       NULL, NULL, USE_AUTH_MDS);
	if (IS_ERR(req)) {
		d_drop(dentry);
		return PTR_ERR(req);
	}
	req->r_locked_dir = dir;
	req->r_args.mknod.mode = cpu_to_le32(mode);
	req->r_args.mknod.rdev = cpu_to_le32(rdev);
	if ((issued & CEPH_CAP_FILE_EXCL) == 0)
		ceph_release_caps(dir, CEPH_CAP_FILE_RDCACHE);
	err = ceph_mdsc_do_request(mdsc, req, dir);
	if (!err && req->r_reply_info.trace_numd == 0) {
		/*
		 * no trace.  do lookup, in case we are called from create
		 * and the VFS needs a valid dentry.
		 */
		struct dentry *d;
		d = ceph_do_lookup(dir->i_sb, dentry, CEPH_STAT_CAP_INODE_ALL,
				   0);
		if (d) {
			/* ick.  this is untested, and inherently racey... i
			   suppose we _did_ create the file, but it has since
			   been deleted?  hrm. */
			dput(d);
			err = -ESTALE;
			dentry = NULL;
		}
	}
	ceph_mdsc_put_request(req);
	if (err)
		d_drop(dentry);
	return err;
}

static int ceph_create(struct inode *dir, struct dentry *dentry, int mode,
			   struct nameidata *nd)
{
	dout(5, "create in dir %p dentry %p name '%.*s'\n",
	     dir, dentry, dentry->d_name.len, dentry->d_name.name);

	if (ceph_snap(dir) != CEPH_NOSNAP)
		return -EROFS;

	if (nd) {
		BUG_ON((nd->flags & LOOKUP_OPEN) == 0);
		dentry = ceph_lookup_open(dir, dentry, nd, mode);
		/* hrm, what should i do here if we get aliased? */
		if (IS_ERR(dentry))
			return PTR_ERR(dentry);
		return 0;
	}

	/* fall back to mknod */
	return ceph_mknod(dir, dentry, (mode & ~S_IFMT) | S_IFREG, 0);
}

static int ceph_symlink(struct inode *dir, struct dentry *dentry,
			const char *dest)
{
	struct ceph_client *client = ceph_sb_to_client(dir->i_sb);
	struct ceph_mds_client *mdsc = &client->mdsc;
	struct ceph_mds_request *req;
	int err;
	int issued = ceph_caps_issued(ceph_inode(dir));

	if (ceph_snap(dir) != CEPH_NOSNAP)
		return -EROFS;

	dout(5, "symlink in dir %p dentry %p to '%s'\n", dir, dentry, dest);
	if (ceph_async_create(dir, dentry, issued, S_IFLNK|S_IRWXUGO,
			      dest) == 0)
		return 0;

	req = ceph_mdsc_create_request(mdsc, CEPH_MDS_OP_SYMLINK,
				       dentry, NULL, NULL, dest, USE_AUTH_MDS);
	if (IS_ERR(req)) {
		d_drop(dentry);
		return PTR_ERR(req);
	}
	req->r_locked_dir = dir;
	if ((issued & CEPH_CAP_FILE_EXCL) == 0)
		ceph_release_caps(dir, CEPH_CAP_FILE_RDCACHE);
	err = ceph_mdsc_do_request(mdsc, req, dir);
	ceph_mdsc_put_request(req);
	if (err)
		d_drop(dentry);
	return err;
}

static int ceph_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	struct ceph_client *client = ceph_sb_to_client(dir->i_sb);
	struct ceph_mds_client *mdsc = &client->mdsc;
	struct ceph_mds_request *req;
	int err;
	char *snap = NULL;
	int snaplen;
	struct dentry *pathdentry = dentry;
	int op = CEPH_MDS_OP_MKDIR;
	int issued = ceph_caps_issued(ceph_inode(dir));

	if (ceph_snap(dir) == CEPH_SNAPDIR) {
		/* mkdir .snap/foo is a MKSNAP */
		op = CEPH_MDS_OP_MKSNAP;
		snaplen = dentry->d_name.len;
		snap = kmalloc(snaplen + 1, GFP_NOFS);
		memcpy(snap, dentry->d_name.name, snaplen);
		snap[snaplen] = 0;
		pathdentry = d_find_alias(dir);
		dout(5, "mksnap dir %p snap '%s' dn %p\n", dir, snap, dentry);
	} else if (ceph_snap(dir) != CEPH_NOSNAP) {
		return -EROFS;
	} else {
		dout(5, "mkdir dir %p dn %p mode 0%o\n", dir, dentry, mode);
		mode |= S_IFDIR;
		if (ceph_async_create(dir, dentry, issued, mode, NULL) == 0)
			return 0;
	}
	req = ceph_mdsc_create_request(mdsc, op, pathdentry, NULL, 
				       NULL, NULL, USE_AUTH_MDS);
	if (pathdentry != dentry)
		dput(pathdentry);
	if (IS_ERR(req)) {
		d_drop(dentry);
		return PTR_ERR(req);
	}

	req->r_locked_dir = dir;
	req->r_args.mkdir.mode = cpu_to_le32(mode);

	if ((issued & CEPH_CAP_FILE_EXCL) == 0)
		ceph_release_caps(dir, CEPH_CAP_FILE_RDCACHE);
	err = ceph_mdsc_do_request(mdsc, req, dir);
	ceph_mdsc_put_request(req);
	if (err < 0)
		d_drop(dentry);
	return err;
}

static int ceph_link(struct dentry *old_dentry, struct inode *dir,
		     struct dentry *dentry)
{
	struct ceph_client *client = ceph_sb_to_client(dir->i_sb);
	struct ceph_mds_client *mdsc = &client->mdsc;
	struct ceph_mds_request *req;
	int err;

	if (ceph_snap(dir) != CEPH_NOSNAP)
		return -EROFS;

	ceph_pending_flush(dentry);

	dout(5, "link in dir %p old_dentry %p dentry %p\n", dir,
	     old_dentry, dentry);
	req = ceph_mdsc_create_request(mdsc, CEPH_MDS_OP_LINK, dentry,
				       old_dentry, NULL, NULL, USE_AUTH_MDS);
	if (IS_ERR(req)) {
		d_drop(dentry);
		return PTR_ERR(req);
	}

	req->r_locked_dir = old_dentry->d_inode;

	if ((ceph_caps_issued(ceph_inode(dir)) & CEPH_CAP_FILE_EXCL) == 0)
		ceph_release_caps(dir, CEPH_CAP_FILE_RDCACHE);
	err = ceph_mdsc_do_request(mdsc, req, dir);
	if (err) {
		d_drop(dentry);
	} else if (req->r_reply_info.trace_numd == 0) {
		/* no trace */
		struct inode *inode = old_dentry->d_inode;
		inc_nlink(inode);
		atomic_inc(&inode->i_count);
		dget(dentry);
		d_instantiate(dentry, inode);
	}
	ceph_mdsc_put_request(req);
	return err;
}

/*
 * rmdir and unlink are differ only by the metadata op code
 */
static int ceph_unlink(struct inode *dir, struct dentry *dentry)
{
	struct ceph_client *client = ceph_sb_to_client(dir->i_sb);
	struct ceph_mds_client *mdsc = &client->mdsc;
	struct inode *inode = dentry->d_inode;
	struct ceph_mds_request *req;
	char *snap = NULL;
	int snaplen;
	int err;
	int op = ((dentry->d_inode->i_mode & S_IFMT) == S_IFDIR) ?
		CEPH_MDS_OP_RMDIR : CEPH_MDS_OP_UNLINK;
	struct dentry *pathdentry = dentry;

	ceph_pending_flush(dentry);

	if (ceph_snap(dir) == CEPH_SNAPDIR) {
		/* rmdir .snap/foo is RMSNAP */
		op = CEPH_MDS_OP_RMSNAP;
		snaplen = dentry->d_name.len;
		snap = kmalloc(snaplen + 1, GFP_NOFS);
		memcpy(snap, dentry->d_name.name, snaplen);
		snap[snaplen] = 0;
		pathdentry = d_find_alias(dir);
		dout(5, "rmsnap dir %p '%s' dn %p\n", dir, snap, dentry);
	} else if (ceph_snap(dir) != CEPH_NOSNAP) {
		return -EROFS;
	} else {
		dout(5, "unlink/rmdir dir %p dn %p inode %p\n",
		     dir, dentry, inode);
	}
	req = ceph_mdsc_create_request(mdsc, op, pathdentry, NULL, NULL, NULL,
				       USE_AUTH_MDS);
	if (pathdentry != dentry)
		dput(pathdentry);
	if (IS_ERR(req))
		return PTR_ERR(req);

	req->r_locked_dir = dir;  /* by VFS */

	if ((ceph_caps_issued(ceph_inode(dir)) & CEPH_CAP_FILE_EXCL) == 0)
		ceph_release_caps(dir, CEPH_CAP_FILE_RDCACHE);
	ceph_mdsc_lease_release(mdsc, dir, dentry, CEPH_LOCK_DN);
	ceph_release_caps(inode, CEPH_CAP_LINK_RDCACHE);
	err = ceph_mdsc_do_request(mdsc, req, dir);
	ceph_mdsc_put_request(req);
	return err;
}

static int ceph_rename(struct inode *old_dir, struct dentry *old_dentry,
		       struct inode *new_dir, struct dentry *new_dentry)
{
	struct ceph_client *client = ceph_sb_to_client(old_dir->i_sb);
	struct ceph_mds_client *mdsc = &client->mdsc;
	struct ceph_mds_request *req;
	int err;

	if (ceph_snap(old_dir) != ceph_snap(new_dir))
		return -EXDEV;
	if (ceph_snap(old_dir) != CEPH_NOSNAP ||
	    ceph_snap(new_dir) != CEPH_NOSNAP)
		return -EROFS;

	ceph_pending_flush(old_dentry);
	ceph_pending_flush(d_find_alias(new_dir));

	dout(5, "dir_rename in dir %p dentry %p to dir %p dentry %p\n",
	     old_dir, old_dentry, new_dir, new_dentry);
	req = ceph_mdsc_create_request(mdsc, CEPH_MDS_OP_RENAME,
				       new_dentry, old_dentry, NULL, NULL,
				       USE_AUTH_MDS);
	if (IS_ERR(req))
		return PTR_ERR(req);
	req->r_locked_dir = new_dir;
	if ((ceph_caps_issued(ceph_inode(old_dir)) & CEPH_CAP_FILE_EXCL) == 0)
		ceph_release_caps(old_dir, CEPH_CAP_FILE_RDCACHE);
	ceph_mdsc_lease_release(mdsc, old_dir, old_dentry, CEPH_LOCK_DN);
	if ((ceph_caps_issued(ceph_inode(new_dir)) & CEPH_CAP_FILE_EXCL) == 0)
		ceph_release_caps(new_dir, CEPH_CAP_FILE_RDCACHE);
	ceph_mdsc_lease_release(mdsc, new_dir, new_dentry, CEPH_LOCK_DN);
	err = ceph_mdsc_do_request(mdsc, req, old_dir);
	if (!err && req->r_reply_info.trace_numd == 0) {
		/*
		 * no trace
		 *
		 * Normally d_move() is done by fill_trace (called by
		 * do_request, above).  If there is no trace, we need
		 * to do it here.
		 */
		if (new_dentry->d_inode)
			dput(new_dentry);
		d_move(old_dentry, new_dentry);
	}
	ceph_mdsc_put_request(req);
	return err;
}



/*
 * check if dentry lease, or parent directory inode lease/cap says
 * this dentry is still valid
 */
static int ceph_dentry_revalidate(struct dentry *dentry, struct nameidata *nd)
{
	struct inode *dir = dentry->d_parent->d_inode;

	/* always trust cached snapped metadata... for now */
	if (ceph_snap(dir) != CEPH_NOSNAP) {
		dout(10, "d_revalidate %p '%.*s' inode %p is SNAPPED\n", dentry,
		     dentry->d_name.len, dentry->d_name.name, dentry->d_inode);
		return 1;
	}

	dout(10, "d_revalidate %p '%.*s' inode %p\n", dentry,
	     dentry->d_name.len, dentry->d_name.name, dentry->d_inode);

	if (ceph_ino(dir) != CEPH_INO_ROOT &&
	    ceph_inode(dir)->i_version == dentry->d_time &&
	    ceph_inode_holds_cap(dir, CEPH_CAP_FILE_RDCACHE)) {
		dout(20, "dentry_revalidate %p %lu file RDCACHE dir %p %llu\n",
		     dentry, dentry->d_time, dir, ceph_inode(dir)->i_version);
		return 1;
	}
	if (ceph_dentry_lease_valid(dentry)) {
		dout(20, "dentry_revalidate %p lease valid\n", dentry);
		return 1;
	}

	dout(20, "dentry_revalidate %p no lease\n", dentry);
	dout(10, " clearing %p complete (d_revalidate)\n", dir);
	ceph_i_clear(dir, CEPH_I_COMPLETE|CEPH_I_READDIR);
	d_drop(dentry);
	return 0;
}

static void ceph_dentry_release(struct dentry *dentry)
{
	struct ceph_dentry_info *di = ceph_dentry(dentry);
	
	if (di) {
		ceph_put_mds_session(di->lease_session);
		kfree(di);
		dentry->d_fsdata = NULL;
	}

	dout(10, " clearing %p complete (d_release)\n", dentry->d_parent);
	ceph_i_clear(dentry->d_parent->d_inode, CEPH_I_COMPLETE|CEPH_I_READDIR);
}

static int ceph_snapdir_dentry_revalidate(struct dentry *dentry,
					  struct nameidata *nd)
{
	/*
	 * Eventually, we'll want to revalidate snapped metadata
	 * too... probably.
	 */
	return 1;
}



/*
 * read() on a dir.  This weird interface hack only works if mounted
 * with '-o dirstat'.
 */
static ssize_t ceph_read_dir(struct file *file, char __user *buf, size_t size,
			     loff_t *ppos)
{
	struct ceph_file_info *cf = file->private_data;
	struct inode *inode = file->f_dentry->d_inode;
	struct ceph_inode_info *ci = ceph_inode(inode);
	int left;

	if (!(ceph_client(inode->i_sb)->mount_args.flags & CEPH_MOUNT_DIRSTAT))
		return -EISDIR;

	if (!cf->dir_info) {
		cf->dir_info = kmalloc(1024, GFP_NOFS);
		if (!cf->dir_info)
			return -ENOMEM;
		cf->dir_info_len =
			sprintf(cf->dir_info,
				"entries:   %20lld\n"
				" files:    %20lld\n"
				" subdirs:  %20lld\n"
				"rentries:  %20lld\n"
				" rfiles:   %20lld\n"
				" rsubdirs: %20lld\n"
				"rbytes:    %20lld\n"
				"rctime:    %10ld.%09ld\n",
				ci->i_files + ci->i_subdirs,
				ci->i_files,
				ci->i_subdirs,
				ci->i_rfiles + ci->i_rsubdirs,
				ci->i_rfiles,
				ci->i_rsubdirs,
				ci->i_rbytes,
				(long)ci->i_rctime.tv_sec,
				(long)ci->i_rctime.tv_nsec);
	}

	if (*ppos >= cf->dir_info_len)
		return 0;
	size = min_t(unsigned, size, cf->dir_info_len-*ppos);
	left = copy_to_user(buf, cf->dir_info + *ppos, size);
	if (left == size)
		return -EFAULT;
	*ppos += (size - left);
	return size - left;
}

static int ceph_dir_fsync(struct file *file, struct dentry *dentry, int datasync)
{
	struct inode *inode = dentry->d_inode;
	struct ceph_inode_info *ci = ceph_inode(inode);
	int ret, err;
	struct list_head *p, *n, *head;
	struct ceph_mds_request *req;
	u64 last_tid;

	ret = 0;
	dout(10, "sync on directory\n");

	head = &ci->i_listener_list;

	spin_lock(&ci->i_listener_lock);

	if (list_empty(head))
		goto out;

	req = list_entry(head->prev,
			 struct ceph_mds_request, r_listener_item);
	last_tid = req->r_tid;

	list_for_each_safe(p, n, head) {
		req = list_entry(p, struct ceph_mds_request, r_listener_item);

		/* avoid starvation */
		if (req->r_tid > last_tid)
			goto out;

		ceph_mdsc_get_request(req);
		spin_unlock(&ci->i_listener_lock);
		if (req->r_timeout) {
			err = wait_for_completion_timeout(&req->r_safe_completion,
							req->r_timeout);
			if (err == 0)
				ret = -EIO;  /* timed out */
		} else {
			wait_for_completion(&req->r_safe_completion);
		}
		spin_lock(&ci->i_listener_lock);

		ceph_mdsc_put_request(req);
	}
out:
	spin_unlock(&ci->i_listener_lock);
	return ret;
}

const struct file_operations ceph_dir_fops = {
	.read = ceph_read_dir,
	.readdir = ceph_readdir,
	.llseek = ceph_dir_llseek,
	.open = ceph_open,
	.release = ceph_release,
	.unlocked_ioctl = ceph_ioctl,
	.fsync = ceph_dir_fsync,
};

const struct inode_operations ceph_dir_iops = {
	.lookup = ceph_lookup,
	.getattr = ceph_getattr,
	.setattr = ceph_setattr,
	.setxattr = ceph_setxattr,
	.getxattr = ceph_getxattr,
	.listxattr = ceph_listxattr,
	.removexattr = ceph_removexattr,
	.mknod = ceph_mknod,
	.symlink = ceph_symlink,
	.mkdir = ceph_mkdir,
	.link = ceph_link,
	.unlink = ceph_unlink,
	.rmdir = ceph_unlink,
	.rename = ceph_rename,
	.create = ceph_create,
};

struct dentry_operations ceph_dentry_ops = {
	.d_revalidate = ceph_dentry_revalidate,
	.d_release = ceph_dentry_release,
};

struct dentry_operations ceph_snapdir_dentry_ops = {
	.d_revalidate = ceph_snapdir_dentry_revalidate,
};

struct dentry_operations ceph_snap_dentry_ops = {
};
