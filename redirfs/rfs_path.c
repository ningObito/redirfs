/*
 * RedirFS: Redirecting File System
 * Written by Frantisek Hrbata <frantisek.hrbata@redirfs.org>
 *
 * Copyright (C) 2008 Frantisek Hrbata
 * All rights reserved.
 *
 * This file is part of RedirFS.
 *
 * RedirFS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * RedirFS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with RedirFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "rfs.h"

static LIST_HEAD(rfs_path_list);
DEFINE_MUTEX(rfs_path_mutex);

static struct rfs_path *rfs_path_alloc(struct vfsmount *mnt,
		struct dentry *dentry)
{
	struct rfs_path *rpath;

	rpath = kzalloc(sizeof(struct rfs_path), GFP_KERNEL);
	if (!rpath)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&rpath->list);
	INIT_LIST_HEAD(&rpath->rroot_list);
	rpath->mnt = mntget(mnt);
	rpath->dentry = dget(dentry);
	atomic_set(&rpath->count, 1);

	return rpath;
}

/* increases rpath reference count */
struct rfs_path *rfs_path_get(struct rfs_path *rpath)
{
	if (!rpath || IS_ERR(rpath))
		return NULL;

	BUG_ON(!atomic_read(&rpath->count));
	atomic_inc(&rpath->count);

	return rpath;
}

/* decreases rpath reference count */
void rfs_path_put(struct rfs_path *rpath)
{
	if (!rpath || IS_ERR(rpath))
		return;

	BUG_ON(!atomic_read(&rpath->count));
	if (!atomic_dec_and_test(&rpath->count))
		return;

	dput(rpath->dentry);
	mntput(rpath->mnt);
	kfree(rpath);
}

/* searches for a rfs_path object in rfs_path_list */
static struct rfs_path *rfs_path_find(struct vfsmount *mnt,
		struct dentry *dentry)
{
	struct rfs_path *rpath = NULL;
	struct rfs_path *found = NULL;

	list_for_each_entry(rpath, &rfs_path_list, list) {
		if (rpath->mnt != mnt) 
			continue;

		if (rpath->dentry != dentry)
			continue;

		found = rfs_path_get(rpath);
		break;
	}

	return found;
}

struct rfs_path *rfs_path_find_id(int id)
{
	struct rfs_path *rpath = NULL;
	struct rfs_path *found = NULL;

	list_for_each_entry(rpath, &rfs_path_list, list) {
		if (rpath->id != id)
			continue;

		found = rfs_path_get(rpath);
		break;
	}

	return found;
}

static int rfs_path_add_rroot(struct rfs_path *rpath)
{
	struct rfs_root *rroot;

	rroot = rfs_root_add(rpath->dentry);
	if (IS_ERR(rroot))
		return PTR_ERR(rroot);
	
	rfs_root_add_rpath(rroot, rpath);
	rpath->rroot = rroot;

	return 0;
}

static void rfs_path_rem_rroot(struct rfs_path *rpath)
{
	rfs_root_rem_rpath(rpath->rroot, rpath);
	rfs_root_put(rpath->rroot);
	rpath->rroot = NULL;
}

static void rfs_path_list_add(struct rfs_path *rpath)
{
	list_add_tail(&rpath->list, &rfs_path_list);
	rfs_path_get(rpath);
}

static void rfs_path_list_rem(struct rfs_path *rpath)
{
	list_del_init(&rpath->list);
	rfs_path_put(rpath);
}

static int rfs_path_get_id(void)
{
	struct rfs_path *rpath = NULL;
	int i = 0;

	while (i < INT_MAX) {
		list_for_each_entry(rpath, &rfs_path_list, list) {
			if (rpath->id == i) {
				i++;
				continue;
			}
		}
		return i;
	}

	return -1;
}

static struct rfs_path *rfs_path_add(struct vfsmount *mnt,
		struct dentry *dentry)
{
	struct rfs_path *rpath;
	int id;
	int rv;

	rpath = rfs_path_find(mnt, dentry);
	if (rpath)
		return rpath;

	id = rfs_path_get_id();
	if (id < 0)
		return ERR_PTR(-EBUSY);

	rpath = rfs_path_alloc(mnt, dentry);
	if (IS_ERR(rpath))
		return rpath;

	rpath->id = id;

	rv = rfs_path_add_rroot(rpath);
	if (rv) {
		rfs_path_put(rpath);
		return ERR_PTR(rv);
	}

	rfs_path_list_add(rpath);

	return rpath;
}

static void rfs_path_rem(struct rfs_path *rpath)
{
	if (rpath->rinch || rpath->rexch)
		return;

	rfs_path_rem_rroot(rpath);
	rfs_path_list_rem(rpath);
}

static int rfs_path_add_dirs(struct dentry *dentry)
{
	struct rfs_inode *rinode;

	rinode = rfs_inode_find(dentry->d_inode);
	if (rinode) {
		rfs_inode_put(rinode);
		return 0;
	}

	return rfs_dcache_walk(dentry, rfs_dcache_add_dir, NULL);
}

static int rfs_path_add_include(struct rfs_path *rpath, struct rfs_flt *rflt)
{
	struct rfs_chain *rinch;
	int rv;

	if (rfs_chain_find(rpath->rinch, rflt) != -1)
		return 0;

	if (rfs_chain_find(rpath->rexch, rflt) != -1)
		return -EEXIST;

	rv = rfs_path_add_dirs(rpath->dentry->d_sb->s_root);
	if (rv)
		return rv;

	rinch = rfs_chain_add(rpath->rinch, rflt);
	if (IS_ERR(rinch))
		return PTR_ERR(rinch);

	rv = rfs_root_add_include(rpath->rroot, rflt);
	if (rv) {
		rfs_chain_put(rinch);
		return rv;
	}

	rfs_chain_put(rpath->rinch);
	rpath->rinch = rinch;
	rflt->paths_nr++;

	return 0;
}
	
static int rfs_path_add_exclude(struct rfs_path *rpath, struct rfs_flt *rflt)
{
	struct rfs_chain *rexch;
	int rv;

	if (rfs_chain_find(rpath->rexch, rflt) != -1)
		return 0;

	if (rfs_chain_find(rpath->rinch, rflt) != -1)
		return -EEXIST;

	rexch = rfs_chain_add(rpath->rexch, rflt);
	if (IS_ERR(rexch))
		return PTR_ERR(rexch);

	rv = rfs_root_add_exclude(rpath->rroot, rflt);
	if (rv) {
		rfs_chain_put(rexch);
		return rv;
	}

	rfs_chain_put(rpath->rexch);
	rpath->rexch = rexch;
	rflt->paths_nr++;

	return 0;
}

static int rfs_path_rem_include(struct rfs_path *rpath, struct rfs_flt *rflt)
{
	struct rfs_chain *rinch;
	int rv;

	if (rfs_chain_find(rpath->rinch, rflt) == -1)
		return 0;

	rinch = rfs_chain_rem(rpath->rinch, rflt);
	if (IS_ERR(rinch))
		return PTR_ERR(rinch);

	rv = rfs_root_rem_include(rpath->rroot, rflt);
	if (rv) {
		rfs_chain_put(rinch);
		return rv;
	}

	rfs_chain_put(rpath->rinch);
	rpath->rinch = rinch;
	rflt->paths_nr--;

	return 0;
}

static int rfs_path_rem_exclude(struct rfs_path *rpath, struct rfs_flt *rflt)
{
	struct rfs_chain *rexch;
	int rv;

	if (rfs_chain_find(rpath->rexch, rflt) == -1)
		return 0;

	rexch = rfs_chain_rem(rpath->rexch, rflt);
	if (IS_ERR(rexch))
		return PTR_ERR(rexch);

	rv = rfs_root_rem_exclude(rpath->rroot, rflt);
	if (rv) {
		rfs_chain_put(rexch);
		return rv;
	}

	rfs_chain_put(rpath->rexch);
	rpath->rexch = rexch;
	rflt->paths_nr--;

	return 0;
}

/**
 * redirfs_add_path() - add a path subtree to filter's path list
 * @filter:	pointer to the filter
 * @info:	describes the path subtree
 *
 * Description:
 *     Adds a path subtree to filter's path list. info identifies the subtree
 *     and contains information whether to include or exclude the path.
 *     Returns a pointer to a RedirFS path object on success.
 **/
redirfs_path redirfs_add_path(redirfs_filter filter,
		struct redirfs_path_info *info)
{
	struct rfs_path *rpath;
	int rv;

	if (!filter || !info)
		return ERR_PTR(-EINVAL);

	if (!info->mnt || !info->dentry || !info->flags)
		return ERR_PTR(-EINVAL);

	mutex_lock(&info->dentry->d_inode->i_sb->s_vfs_rename_mutex);
	mutex_lock(&rfs_path_mutex);

	rpath = rfs_path_add(info->mnt, info->dentry);
	if (IS_ERR(rpath))
		goto exit;

	if (info->flags == REDIRFS_PATH_INCLUDE)
		rv = rfs_path_add_include(rpath, filter);

	else if (info->flags == REDIRFS_PATH_EXCLUDE)
		rv = rfs_path_add_exclude(rpath, filter);

	else
		rv = -EINVAL;

	rfs_path_rem(rpath);

	if (rv) {
		rfs_path_put(rpath);
		rpath = ERR_PTR(rv);
	}
exit:
	mutex_unlock(&rfs_path_mutex);
	mutex_unlock(&info->dentry->d_inode->i_sb->s_vfs_rename_mutex);
	return rpath;
}

/**
 * redirfs_rem_path() - removes a path subtree form filter's path list
 * @filter:	pointer to the filter
 * @path:	pointer to the RedirFS path object
 *
 * Description:
 *     Removes a path subtree from filter's path list. path needs to be
 *     a pointer to a RedirFS path object returned by redirfs_add_path().
 **/
int redirfs_rem_path(redirfs_filter filter, redirfs_path path)
{
	struct rfs_path *rpath = (struct rfs_path *)path;
	int rv;

	if (!filter || !path)
		return -EINVAL;

	mutex_lock(&rpath->dentry->d_inode->i_sb->s_vfs_rename_mutex);
	mutex_lock(&rfs_path_mutex);

	if (rfs_chain_find(rpath->rinch, filter) != -1)
		rv = rfs_path_rem_include(path, filter);

	else if (rfs_chain_find(rpath->rexch, filter) != -1)
		rv = rfs_path_rem_exclude(path, filter);

	else
		rv = -EINVAL;

	rfs_path_rem(rpath);

	mutex_unlock(&rfs_path_mutex);
	mutex_unlock(&rpath->dentry->d_inode->i_sb->s_vfs_rename_mutex);

	return rv;
}

/**
 * redirfs_get_id_path() - retrieves path object id
 * @path:	RedirFS path object returned by redirfs_add_path()
 **/
int redirfs_get_id_path(redirfs_path path)
{
	struct rfs_path *rpath = path;

	if (!path || IS_ERR(path))
		return -EINVAL;

	return rpath->id;
}

/**
 * redirfs_get_path_id() - retrieves a path object by it's id
 * @id:		id of the path object we're looking for
 **/
redirfs_path redirfs_get_path_id(int id)
{
	return rfs_path_find_id(id);
}

/**
 * redirfs_get_path() - increases path object reference count
 * @path:	RedirFS path object returned by redirfs_add_path()
 **/
redirfs_path redirfs_get_path(redirfs_path path)
{
	return rfs_path_get(path);
}

/**
 * redirfs_put_path() - lowers path object reference count
 * @path:	RedirFS path object returned by redirfs_add_path()
 **/
void redirfs_put_path(redirfs_path path)
{
	rfs_path_put(path);
}

/**
 * redirfs_get_paths_root() - retrieves paths associated with root
 * @filter:	pointer to the filter, whose paths we're interested in
 * @root:	pointer to the root object containing the paths
 *
 * Description:
 *     Also increases the returned paths reference count. TODO look more
 *     into the inner workings of this function and complete the desc.
 **/
redirfs_path* redirfs_get_paths_root(redirfs_filter filter, redirfs_root root)
{
	struct rfs_root *rroot = (struct rfs_root *)root;
	struct rfs_path *rpath;
	redirfs_path *paths;
	int i = 0;

	if (!filter || !root)
		return ERR_PTR(-EINVAL);

	spin_lock(&rroot->lock);
	paths = kzalloc(sizeof(redirfs_path) * (rroot->paths_nr + 1),
			GFP_ATOMIC);
	if (!paths) {
		spin_unlock(&rroot->lock);
		return ERR_PTR(-ENOMEM);
	}

	list_for_each_entry(rpath, &rroot->rpaths, rroot_list) {
		if (rfs_chain_find(rroot->rinch, filter) != -1)
			paths[i++] = rfs_path_get(rpath);

		else if (rfs_chain_find(rroot->rexch, filter) != -1)
			paths[i++] = rfs_path_get(rpath);

	}

	spin_unlock(&rroot->lock);
	paths[i] = NULL;

	return paths;
}

/**
 * redirfs_get_paths() - retrieves paths in filter's path list
 * @filter:	pointer to the filter
 *
 * Description:
 *     Also increases the reference count of returned paths. TODO complete.
 **/
redirfs_path* redirfs_get_paths(redirfs_filter filter)
{
	struct rfs_flt *rflt = filter;
	struct rfs_path *rpath;
	redirfs_path *paths;
	int i = 0;

	if (!filter)
		return ERR_PTR(-EINVAL);

	mutex_lock(&rfs_path_mutex);

	paths = kzalloc(sizeof(redirfs_path) * (rflt->paths_nr + 1),
			GFP_KERNEL);
	if (!paths) {
		mutex_unlock(&rfs_path_mutex);
		return ERR_PTR(-ENOMEM);
	}

	list_for_each_entry(rpath, &rfs_path_list, list) {
		if (rfs_chain_find(rpath->rinch, filter) != -1)
			paths[i++] = rfs_path_get(rpath);

		else if (rfs_chain_find(rpath->rexch, filter) != -1)
			paths[i++] = rfs_path_get(rpath);
	}

	mutex_unlock(&rfs_path_mutex);
	paths[i] = NULL;

	return paths;
}

/**
 * redirfs_put_paths() - frees paths returned by redirfs_get_paths*
 * @paths:	paths returned by redirfs_get_paths* functions
 * 
 * Description:
 *     Also lowers the paths reference counts.
 **/
void redirfs_put_paths(redirfs_path *paths)
{
	int i = 0;

	if (!paths)
		return;

	while (paths[i]) {
		redirfs_put_path(paths[i]);
		i++;
	}

	kfree(paths);
}

/**
 * redirfs_get_path_info() - retrieves informations about path/filter
 * @filter:	pointer to the filter
 * @path:	pointer to the path object
 *
 * Description:
 *     Constructs a redirfs_path_info object based on filter and path.
 **/
struct redirfs_path_info *redirfs_get_path_info(redirfs_filter filter,
		redirfs_path path)
{
	struct rfs_path *rpath = path;
	struct redirfs_path_info *info;

	if (!filter || !path)
		return ERR_PTR(-EINVAL);

	info = kzalloc(sizeof(struct redirfs_path_info), GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);

	mutex_lock(&rfs_path_mutex);

	if (rfs_chain_find(rpath->rinch, filter) != -1)
		info->flags = REDIRFS_PATH_INCLUDE;

	else if (rfs_chain_find(rpath->rexch, filter) != -1)
		info->flags = REDIRFS_PATH_EXCLUDE;

	mutex_unlock(&rfs_path_mutex);

	if (!info->flags) {
		kfree(info);
		return ERR_PTR(-ENODATA);
	}

	info->mnt = mntget(rpath->mnt);
	info->dentry = dget(rpath->dentry);

	return info;
}

/**
 * redirfs_put_path_info() - frees path information
 * @info:	path information returned by redirfs_get_path_info()
 **/
void redirfs_put_path_info(struct redirfs_path_info *info)
{
	if (!info)
		return;

	mntput(info->mnt);
	dput(info->dentry);
	kfree(info);
}

/**
 * redirfs_rem_paths() - removes all paths for filter's path list
 * @filter:	pointer to the filter
 **/
int redirfs_rem_paths(redirfs_filter filter)
{
	redirfs_path *paths;
	int rv;
	int i = 0;

	if (!filter)
		return -EINVAL;

	paths = redirfs_get_paths(filter);
	if (IS_ERR(paths))
		return PTR_ERR(paths);

	while (paths[i]) {
		rv = redirfs_rem_path(filter, paths[i]);
		if (rv)
			break;
		i++;
	}

	redirfs_put_paths(paths);

	return rv;
}

int rfs_path_get_info(struct rfs_flt *rflt, char *buf, int size)
{
	struct rfs_path *rpath;
	char *path;
	char type;
	int len = 0;
	int rv;

	path = kzalloc(sizeof(char) * PAGE_SIZE, GFP_KERNEL);
	if (!path)
		return -ENOMEM;

	mutex_lock(&rfs_path_mutex);

	list_for_each_entry(rpath, &rfs_path_list, list) {
		if (rfs_chain_find(rpath->rinch, rflt) != -1)
			type = 'i';

		else if (rfs_chain_find(rpath->rexch, rflt) != -1)
			type = 'e';

		else
			continue;

		rv = redirfs_get_filename(rpath->mnt, rpath->dentry, path,
				PAGE_SIZE);

		if (rv) {
			mutex_unlock(&rfs_path_mutex);
			kfree(path);
			return rv;
		}

		len += snprintf(buf + len, size - len,"%c:%d:%s",
				type, rpath->id, path) + 1;

		if (len >= size) {
			len = size;
			break;
		}
	}

	mutex_unlock(&rfs_path_mutex);
	kfree(path);

	return len;
}

/**
 * redirfs_get_filename() - retrieve a filename based on dentry and vfsmount
 * @mnt:	mountpoint
 * @dentry:	dentry
 * @buf:	pointer to a user defined buffer to store the filename
 * @size:	size of the buffer in bytes
 **/
int redirfs_get_filename(struct vfsmount *mnt, struct dentry *dentry, char *buf,
		int size)
{
	struct dentry *dtmp = NULL;
	struct dentry *dmnt = NULL;
	struct vfsmount *mtmp = NULL;
	struct vfsmount *mmnt = NULL;
	char *end;
	int len;

	end = buf + size;
	len = size;

	if (size < 2)
		return -ENAMETOOLONG;

	*--end = '\0';
	size--;
again:
	spin_lock(&dcache_lock);

	while (dentry != mnt->mnt_root) {
		size -= dentry->d_name.len + 1; /* dentry name + slash */
		if (size < 0) {
			spin_unlock(&dcache_lock);
			return -ENAMETOOLONG;
		}

		end -= dentry->d_name.len;
		memcpy(end, dentry->d_name.name, dentry->d_name.len);
		*--end = '/';
		dentry = dentry->d_parent;
	}

	dtmp = dmnt;
	mtmp = mmnt;
	dmnt = dget(dentry);
	mmnt = mntget(mnt);
	spin_unlock(&dcache_lock);
	dput(dtmp);
	mntput(mtmp);

	if (follow_up(&mmnt, &dmnt)) {
		dentry = dmnt;
		mnt = mmnt;
		goto again;
	}

	dput(dmnt);
	mntput(mmnt);
	
	if (*end != '/') {
		*--end = '/';
		size--;
	}

	memmove(buf, end, len - size);
	return 0;
}

static int rfs_fsrename_rem_rroot(struct rfs_root *rroot,
		struct rfs_chain *rchain)
{
	int rv;
	int i;

	if (!rchain)
		return 0;

	for (i = 0; i < rchain->rflts_nr; i++) {
		rv = rfs_root_rem_flt(rroot, rchain->rflts[i]);
		if (rv)
			return rv;

		rv = rfs_root_walk(rfs_root_rem_flt, rchain->rflts[i]);
		if (rv)
			return rv;
	}

	return 0;
}

static int rfs_fsrename_rem_dentry(struct rfs_root *rroot,
		struct rfs_chain *rchain, struct dentry *dentry)
{
	struct rfs_chain *rchnew = NULL;
	struct rfs_chain *rchrem = NULL;
	struct rfs_info *rinfo = NULL;
	int rv = 0;
	int i;

	if (!rchain)
		return 0;

	rchrem = rfs_chain_get(rroot->rinfo->rchain);

	for (i = 0; i < rchain->rflts_nr; i++) {
		rchnew = rfs_chain_rem(rchrem, rchain->rflts[i]);
		if (IS_ERR(rchnew)) {
			rv = PTR_ERR(rchnew);
			goto exit;
		}

		rfs_chain_put(rchrem);
		rchrem = rchnew;
		rinfo = rfs_info_alloc(rroot, rchnew);
		if (IS_ERR(rinfo)) {
			rv = PTR_ERR(rinfo);
			goto exit;
		}

		rv = rfs_info_rem(dentry, rinfo, rchain->rflts[i]);
		rfs_info_put(rinfo);
		if (rv)
			goto exit;
	}
exit:
	rfs_chain_put(rchrem);
	return rv;
}

static int rfs_fsrename_rem(struct rfs_root *rroot_src,
		struct rfs_root *rroot_dst, struct dentry *dentry)
{
	struct rfs_chain *rchain = NULL;
	int rv;

	if (!rroot_src)
		return 0;

	if (!rroot_dst)
		rchain = rfs_chain_get(rroot_src->rinfo->rchain);
	else
		rchain = rfs_chain_diff(rroot_src->rinfo->rchain,
				rroot_dst->rinfo->rchain);

	if (IS_ERR(rchain))
		return PTR_ERR(rchain);

	if (rroot_src->dentry == dentry) 
		rv = rfs_fsrename_rem_rroot(rroot_src, rchain);
	else
		rv = rfs_fsrename_rem_dentry(rroot_src, rchain, dentry);

	rfs_chain_put(rchain);
	return rv;
}

static int rfs_fsrename_add_rroot(struct rfs_root *rroot,
		struct rfs_chain *rchain)
{
	int rv;
	int i;

	if (!rchain)
		return 0;

	for (i = 0; i < rchain->rflts_nr; i++) {
		rv = rfs_root_add_flt(rroot, rchain->rflts[i]);
		if (rv)
			return rv;

		rv = rfs_root_walk(rfs_root_add_flt, rchain->rflts[i]);
		if (rv)
			return rv;
	}

	return 0;
}

static int rfs_fsrename_add_dentry(struct rfs_root *rroot,
		struct rfs_chain *rchain, struct dentry *dentry)
{
	struct rfs_chain *rchnew = NULL;
	struct rfs_chain *rchadd = NULL;
	struct rfs_dentry *rdentry = NULL;
	struct rfs_info *rinfo = NULL;
	int rv = 0;
	int i;

	if (!rchain)
		return rfs_info_reset(dentry, rroot->rinfo);

	rdentry = rfs_dentry_find(dentry);
	if (rdentry)
		rchadd = rfs_chain_get(rdentry->rinfo->rchain);

	for (i = 0; i < rchain->rflts_nr; i++) {
		rchnew = rfs_chain_add(rchadd, rchain->rflts[i]);
		if (IS_ERR(rchnew)) {
			rv = PTR_ERR(rchnew);
			goto exit;
		}

		rfs_chain_put(rchadd);
		rchadd = rchnew;
		rinfo = rfs_info_alloc(rroot, rchnew);
		if (IS_ERR(rinfo)) {
			rv = PTR_ERR(rinfo);
			goto exit;
		}

		rv = rfs_info_add(dentry, rinfo, rchain->rflts[i]);
		rfs_info_put(rinfo);
		if (rv)
			goto exit;
	}

	rv = rfs_info_reset(dentry, rroot->rinfo);
exit:
	rfs_dentry_put(rdentry);
	rfs_chain_put(rchadd);
	return rv;
}

static int rfs_fsrename_add(struct rfs_root *rroot_src,
		struct rfs_root *rroot_dst, struct dentry *dentry)
{
	struct rfs_chain *rchain = NULL;
	int rv;

	if (!rroot_dst)
		return 0;

	if (!rroot_src)
		rchain = rfs_chain_get(rroot_dst->rinfo->rchain);
	else
		rchain = rfs_chain_diff(rroot_dst->rinfo->rchain,
				rroot_src->rinfo->rchain);

	if (IS_ERR(rchain))
		return PTR_ERR(rchain);

	if (rroot_src && rroot_src->dentry == dentry) 
		rv = rfs_fsrename_add_rroot(rroot_src, rchain);
	else
		rv = rfs_fsrename_add_dentry(rroot_dst, rchain, dentry);

	rfs_chain_put(rchain);
	return rv;
}

static int rfs_fsrename_set(struct rfs_root *rroot_src,
		struct rfs_root *rroot_dst, struct dentry *dentry)
{
	struct rfs_dentry *rdentry;
	struct rfs_chain *rchain_src;
	struct rfs_chain *rchain_dst;
	struct rfs_info *rinfo;
	int rv = 0;
	int i;

	if (!rroot_src || !rroot_dst)
		return 0;

	if (rroot_src->dentry == dentry) 
		return 0;

	rdentry = rfs_dentry_find(dentry);
	if (!rdentry)
		return 0;

	rchain_src = rdentry->rinfo->rchain;
	rchain_dst = rroot_dst->rinfo->rchain;

	for (i = 0; i < rchain_src->rflts_nr; i++) {
		if (rfs_chain_find(rchain_dst, rchain_src->rflts[i]) == -1)
			continue;

		rinfo = rfs_info_alloc(rroot_dst, rchain_src);
		if (IS_ERR(rinfo)) {
			rv = PTR_ERR(rinfo);
			goto exit;
		}

		rv = rfs_info_set(dentry, rinfo, rchain_src->rflts[i]);
		rfs_info_put(rinfo);
		if (rv)
			goto exit;
	}
exit:
	rfs_dentry_put(rdentry);
	return rv;
}

int rfs_fsrename(struct inode *old_dir, struct dentry *old_dentry,
		struct inode *new_dir, struct dentry *new_dentry)
{
	struct rfs_root *rroot_src = NULL;
	struct rfs_root *rroot_dst = NULL;
	struct rfs_inode *rinode = NULL;
	struct rfs_dentry *rdentry = NULL;
	int rv = 0;

	if (old_dir == new_dir)
		return 0;

	mutex_lock(&rfs_path_mutex);

	rinode = rfs_inode_find(new_dir);
	rdentry = rfs_dentry_find(old_dentry);

	if (rinode->rinfo->rchain)
		rroot_dst = rfs_root_get(rinode->rinfo->rroot);

	if (rdentry && rdentry->rinfo->rchain)
		rroot_src = rfs_root_get(rdentry->rinfo->rroot);

	if (rroot_src == rroot_dst) 
		goto exit;

	rv = rfs_fsrename_rem(rroot_src, rroot_dst, old_dentry);
	if (rv)
		goto exit;

	rv = rfs_fsrename_set(rroot_src, rroot_dst, old_dentry);
	if (rv)
		goto exit;

	rv = rfs_fsrename_add(rroot_src, rroot_dst, old_dentry);
exit:
	mutex_unlock(&rfs_path_mutex);
	rfs_root_put(rroot_src);
	rfs_root_put(rroot_dst);
	rfs_inode_put(rinode);
	rfs_dentry_put(rdentry);
	return rv;
}

EXPORT_SYMBOL(redirfs_get_path);
EXPORT_SYMBOL(redirfs_put_path);
EXPORT_SYMBOL(redirfs_get_paths);
EXPORT_SYMBOL(redirfs_get_paths_root);
EXPORT_SYMBOL(redirfs_put_paths);
EXPORT_SYMBOL(redirfs_get_path_info);
EXPORT_SYMBOL(redirfs_put_path_info);
EXPORT_SYMBOL(redirfs_add_path);
EXPORT_SYMBOL(redirfs_rem_path);
EXPORT_SYMBOL(redirfs_rem_paths);
EXPORT_SYMBOL(redirfs_get_filename);
EXPORT_SYMBOL(redirfs_get_id_path);
EXPORT_SYMBOL(redirfs_get_path_id);
