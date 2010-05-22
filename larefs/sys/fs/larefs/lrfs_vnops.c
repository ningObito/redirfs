/*-
 * Copyright (c) 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * John Heidemann of the UCLA Ficus project.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This software was derived form nullfs implementation in FreeBSD-8
 *
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>

#include <fs/larefs/lrfs.h>
#include <fs/larefs/larefs.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_object.h>
#include <vm/vnode_pager.h>

static int lrfs_bug_bypass = 0;   /* for debugging: enables bypass printf'ing */
SYSCTL_INT(_debug, OID_AUTO, lrfs_bug_bypass, CTLFLAG_RW, 
	&lrfs_bug_bypass, 0, "");

static int do_lrfs_lookup(struct vop_lookup_args *);
static int do_lrfs_inactive(struct vop_inactive_args *);

/*
 * Proceed filter pre/post operation and pass the operation
 * to lower layer between them.
 */
int
lrfs_proceed_oper(struct vop_generic_args *ap, int op_id) 
{
	int ret, proceed;
	struct vnode **first_vp;
	struct lrfs_filter_chain *chain;
	struct vnodeop_desc *descp = ap->a_desc;

	first_vp = VOPARG_OFFSETTO(struct vnode**, descp->vdesc_vp_offsets[0],ap);
	chain = LRFSGETCHAIN((*first_vp));
	
	/*
	 * Lock the chain so nobody can alter it while performing
	 * operations.
	 */
	sx_slock(&chain->chainlck);
	proceed = lrfs_precallbacks_chain(ap, chain, op_id);
	proceed = chain->count - proceed;

	ret = lrfs_bypass(ap);	

	lrfs_postcallbacks_chain(ap, chain, op_id, proceed);
	sx_sunlock(&chain->chainlck);

	return ret;
}


/*
 * This is the 10-Apr-92 bypass routine.
 *    This version has been optimized for speed, throwing away some
 * safety checks.  It should still always work, but it's not as
 * robust to programmer errors.
 *
 * In general, we map all vnodes going down and unmap them on the way back.
 * As an exception to this, vnodes can be marked "unmapped" by setting
 * the Nth bit in operation's vdesc_flags.
 *
 * Also, some BSD vnode operations have the side effect of vrele'ing
 * their arguments.  With stacking, the reference counts are held
 * by the upper node, not the lower one, so we must handle these
 * side-effects here.  This is not of concern in Sun-derived systems
 * since there are no such side-effects.
 *
 * This makes the following assumptions:
 * - only one returned vpp
 * - no INOUT vpp's (Sun's vop_open has one of these)
 * - the vnode operation vector of the first vnode should be used
 *   to determine what implementation of the op should be invoked
 * - all mapped vnodes are of our vnode-type (NEEDSWORK:
 *   problems on rmdir'ing mount points and renaming?)
 */
int
lrfs_bypass(struct vop_generic_args *ap)
{
	struct vnode **this_vp_p;
	int error;
	struct vnode *old_vps[VDESC_MAX_VPS];
	struct vnode **vps_p[VDESC_MAX_VPS];
	struct vnode ***vppp;
	struct vnodeop_desc *descp = ap->a_desc;
	int reles, i;

	if (lrfs_bug_bypass)
		printf ("lrfs_bypass: %s\n", descp->vdesc_name);

#ifdef DIAGNOSTIC
	/*
	 * We require at least one vp.
	 */
	if (descp->vdesc_vp_offsets == NULL ||
	    descp->vdesc_vp_offsets[0] == VDESC_NO_OFFSET)
		panic ("lrfs_bypass: no vp's in map");
#endif

	/*
	 * Map the vnodes going in.
	 * Later, we'll invoke the operation based on
	 * the first mapped vnode's operation vector.
	 */
	reles = descp->vdesc_flags;
	for (i = 0; i < VDESC_MAX_VPS; reles >>= 1, i++) {
		if (descp->vdesc_vp_offsets[i] == VDESC_NO_OFFSET)
			break;   /* bail out at end of list */
		vps_p[i] = this_vp_p =
			VOPARG_OFFSETTO(struct vnode**,descp->vdesc_vp_offsets[i],ap);
		/*
		 * We're not guaranteed that any but the first vnode
		 * are of our type.  Check for and don't map any
		 * that aren't.  (We must always map first vp or vclean fails.)
		 */
		if (i && (*this_vp_p == NULLVP ||
		    (*this_vp_p)->v_op != &lrfs_vnodeops)) {
			old_vps[i] = NULLVP;
		} else {
			old_vps[i] = *this_vp_p;
			*(vps_p[i]) = LRFSVPTOLOWERVP(*this_vp_p);
			/*
			 * XXX - Several operations have the side effect
			 * of vrele'ing their vp's.  We must account for
			 * that.  (This should go away in the future.)
			 */
			if (reles & VDESC_VP0_WILLRELE)
				VREF(*this_vp_p);
		}

	}

	/*
	 * Call the operation on the lower layer
	 * with the modified argument structure.
	 */
	if (vps_p[0] && *vps_p[0])
		error = VCALL(ap);
	else {
		printf("lrfs_bypass: no map for %s\n", descp->vdesc_name);
		error = EINVAL;
	}

	/*
	 * Maintain the illusion of call-by-value
	 * by restoring vnodes in the argument structure
	 * to their original value.
	 */
	reles = descp->vdesc_flags;
	for (i = 0; i < VDESC_MAX_VPS; reles >>= 1, i++) {
		if (descp->vdesc_vp_offsets[i] == VDESC_NO_OFFSET)
			break;   /* bail out at end of list */
		if (old_vps[i]) {
			*(vps_p[i]) = old_vps[i];
#if 0
			if (reles & VDESC_VP0_WILLUNLOCK)
				VOP_UNLOCK(*(vps_p[i]), 0);
#endif
			if (reles & VDESC_VP0_WILLRELE)
				vrele(*(vps_p[i]));
		}
	}

	/*
	 * Map the possible out-going vpp
	 * (Assumes that the lower layer always returns
	 * a VREF'ed vpp unless it gets an error.)
	 */
	if (descp->vdesc_vpp_offset != VDESC_NO_OFFSET &&
	    !(descp->vdesc_flags & VDESC_NOMAP_VPP) &&
	    !error) {
		/*
		 * XXX - even though some ops have vpp returned vp's,
		 * several ops actually vrele this before returning.
		 * We must avoid these ops.
		 * (This should go away when these ops are regularized.)
		 */
		if (descp->vdesc_flags & VDESC_VPP_WILLRELE)
			goto out;
		vppp = VOPARG_OFFSETTO(struct vnode***,
				 descp->vdesc_vpp_offset,ap);
		if (*vppp)
			error = lrfs_nodeget(old_vps[0]->v_mount, **vppp, *vppp);
	}

 out:
	return (error);
}

static int
lrfs_lookup(struct vop_lookup_args *ap)
{
	int ret, proceed;
	struct vnode **first_vp;
	struct lrfs_filter_chain *chain;
	struct vop_generic_args *ga = &ap->a_gen;
	struct vnodeop_desc *descp = ga->a_desc;

	first_vp = VOPARG_OFFSETTO(struct vnode**, descp->vdesc_vp_offsets[0],ap);
	chain = LRFSGETCHAIN((*first_vp));
	
	/*
	 * Lock the chain so nobody can alter it while performing
	 * operations.
	 */
	sx_slock(&chain->chainlck);
	proceed = lrfs_precallbacks_chain(ga, chain, LAREFS_LOOKUP);
	proceed = chain->count - proceed;

	ret = do_lrfs_lookup(ap);	

	lrfs_postcallbacks_chain(ga, chain, LAREFS_LOOKUP, proceed);
	sx_sunlock(&chain->chainlck);

	return ret;
}

/*
 * We have to carry on the locking protocol on the lrfs layer vnodes
 * as we progress through the tree. We also have to enforce read-only
 * if this layer is mounted read-only.
 */
static int
do_lrfs_lookup(struct vop_lookup_args *ap)
{
	struct componentname *cnp = ap->a_cnp;
	struct vnode *dvp = ap->a_dvp;
	int flags = cnp->cn_flags;
	struct vnode *vp, *ldvp, *lvp;
	int error;

	if ((flags & ISLASTCN) && (dvp->v_mount->mnt_flag & MNT_RDONLY) &&
	    (cnp->cn_nameiop == DELETE || cnp->cn_nameiop == RENAME))
		return (EROFS);
	/*
	 * Although it is possible to call lrfs_bypass(), we'll do
	 * a direct call to reduce overhead
	 */
	ldvp = LRFSVPTOLOWERVP(dvp);
	vp = lvp = NULL;
	error = VOP_LOOKUP(ldvp, &lvp, cnp);
	if (error == EJUSTRETURN && (flags & ISLASTCN) &&
	    (dvp->v_mount->mnt_flag & MNT_RDONLY) &&
	    (cnp->cn_nameiop == CREATE || cnp->cn_nameiop == RENAME))
		error = EROFS;

	if ((error == 0 || error == EJUSTRETURN) && lvp != NULL) {
		if (ldvp == lvp) {
			*ap->a_vpp = dvp;
			VREF(dvp);
			vrele(lvp);
		} else {
			error = lrfs_nodeget(dvp->v_mount, lvp, &vp);
			if (error)
				vput(lvp);
			else
				*ap->a_vpp = vp;
		}
	}
	return (error);
}

static int
lrfs_open(struct vop_open_args *ap)
{
	int retval;
	struct vnode *vp, *ldvp;

	vp = ap->a_vp;
	ldvp = LRFSVPTOLOWERVP(vp);
	retval = lrfs_proceed_oper(&ap->a_gen, LAREFS_OPEN);
	if (retval == 0)
		vp->v_object = ldvp->v_object;
	return (retval);
}

/*
 * Setattr call. Disallow write attempts if the layer is mounted read-only.
 */
static int
lrfs_setattr(struct vop_setattr_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vattr *vap = ap->a_vap;

  	if ((vap->va_flags != VNOVAL || vap->va_uid != (uid_t)VNOVAL ||
	    vap->va_gid != (gid_t)VNOVAL || vap->va_atime.tv_sec != VNOVAL ||
	    vap->va_mtime.tv_sec != VNOVAL || vap->va_mode != (mode_t)VNOVAL) &&
	    (vp->v_mount->mnt_flag & MNT_RDONLY))
		return (EROFS);
	if (vap->va_size != VNOVAL) {
 		switch (vp->v_type) {
 		case VDIR:
 			return (EISDIR);
 		case VCHR:
 		case VBLK:
 		case VSOCK:
 		case VFIFO:
			if (vap->va_flags != VNOVAL)
				return (EOPNOTSUPP);
			return (0);
		case VREG:
		case VLNK:
 		default:
			/*
			 * Disallow write attempts if the filesystem is
			 * mounted read-only.
			 */
			if (vp->v_mount->mnt_flag & MNT_RDONLY)
				return (EROFS);
		}
	}

	return (lrfs_proceed_oper((struct vop_generic_args *)ap, LAREFS_SETATTR));
}

/*
 *  We handle getattr only to change the fsid.
 */
static int
lrfs_getattr(struct vop_getattr_args *ap)
{
	int error;

	if ((error = lrfs_proceed_oper((struct vop_generic_args *)ap, LAREFS_GETATTR)) != 0)
		return (error);

	ap->a_vap->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	return (0);
}

/*
 * Handle to disallow write access if mounted read-only.
 */
static int
lrfs_access(struct vop_access_args *ap)
{
	struct vnode *vp = ap->a_vp;
	accmode_t accmode = ap->a_accmode;

	/*
	 * Disallow write attempts on read-only layers;
	 * unless the file is a socket, fifo, or a block or
	 * character device resident on the filesystem.
	 */
	if (accmode & VWRITE) {
		switch (vp->v_type) {
		case VDIR:
		case VLNK:
		case VREG:
			if (vp->v_mount->mnt_flag & MNT_RDONLY)
				return (EROFS);
			break;
		default:
			break;
		}
	}
	return (lrfs_proceed_oper((struct vop_generic_args *)ap, LAREFS_ACCESS));
}

static int
lrfs_accessx(struct vop_accessx_args *ap)
{
	struct vnode *vp = ap->a_vp;
	accmode_t accmode = ap->a_accmode;

	/*
	 * Disallow write attempts on read-only layers;
	 * unless the file is a socket, fifo, or a block or
	 * character device resident on the filesystem.
	 */
	if (accmode & VWRITE) {
		switch (vp->v_type) {
		case VDIR:
		case VLNK:
		case VREG:
			if (vp->v_mount->mnt_flag & MNT_RDONLY)
				return (EROFS);
			break;
		default:
			break;
		}
	}
	return (lrfs_proceed_oper((struct vop_generic_args *)ap, LAREFS_ACCESSX));
}

/*
 * We handle this to eliminate lrfs FS to lower FS
 * file moving. Don't know why we don't allow this,
 * possibly we should.
 */
static int
lrfs_rename(struct vop_rename_args *ap)
{
	struct vnode *tdvp = ap->a_tdvp;
	struct vnode *fvp = ap->a_fvp;
	struct vnode *fdvp = ap->a_fdvp;
	struct vnode *tvp = ap->a_tvp;

	/* Check for cross-device rename. */
	if ((fvp->v_mount != tdvp->v_mount) ||
	    (tvp && (fvp->v_mount != tvp->v_mount))) {
		if (tdvp == tvp)
			vrele(tdvp);
		else
			vput(tdvp);
		if (tvp)
			vput(tvp);
		vrele(fdvp);
		vrele(fvp);
		return (EXDEV);
	}
	
	return (lrfs_proceed_oper((struct vop_generic_args *)ap, LAREFS_RENAME));
}

/*
 * We need to process our own vnode lock and then clear the
 * interlock flag as it applies only to our vnode, not the
 * vnodes below us on the stack.
 */
static int
lrfs_lock(struct vop_lock1_args *ap)
{
	struct vnode *vp = ap->a_vp;
	int flags = ap->a_flags;
	struct lrfs_node *nn;
	struct vnode *lvp;
	int error;


	if ((flags & LK_INTERLOCK) == 0) {
		VI_LOCK(vp);
		ap->a_flags = flags |= LK_INTERLOCK;
	}
	nn = VTOLRFS(vp);
	/*
	 * If we're still active we must ask the lower layer to
	 * lock as ffs has special lock considerations in it's
	 * vop lock.
	 */
	if (nn != NULL && (lvp = LRFSVPTOLOWERVP(vp)) != NULL) {
		VI_LOCK_FLAGS(lvp, MTX_DUPOK);
		VI_UNLOCK(vp);
		/*
		 * We have to hold the vnode here to solve a potential
		 * reclaim race.  If we're forcibly vgone'd while we
		 * still have refs, a thread could be sleeping inside
		 * the lowervp's vop_lock routine.  When we vgone we will
		 * drop our last ref to the lowervp, which would allow it
		 * to be reclaimed.  The lowervp could then be recycled,
		 * in which case it is not legal to be sleeping in it's VOP.
		 * We prevent it from being recycled by holding the vnode
		 * here.
		 */
		vholdl(lvp);
		error = VOP_LOCK(lvp, flags);

		/*
		 * We might have slept to get the lock and someone might have
		 * clean our vnode already, switching vnode lock from one in
		 * lowervp to v_lock in our own vnode structure.  Handle this
		 * case by reacquiring correct lock in requested mode.
		 */
		if (VTOLRFS(vp) == NULL && error == 0) {
			ap->a_flags &= ~(LK_TYPE_MASK | LK_INTERLOCK);
			switch (flags & LK_TYPE_MASK) {
			case LK_SHARED:
				ap->a_flags |= LK_SHARED;
				break;
			case LK_UPGRADE:
			case LK_EXCLUSIVE:
				ap->a_flags |= LK_EXCLUSIVE;
				break;
			default:
				panic("Unsupported lock request %d\n",
				    ap->a_flags);
			}
			VOP_UNLOCK(lvp, 0);
			error = vop_stdlock(ap);
		}
		vdrop(lvp);
	} else
		error = vop_stdlock(ap);

	return (error);
}

/*
 * We need to process our own vnode unlock and then clear the
 * interlock flag as it applies only to our vnode, not the
 * vnodes below us on the stack.
 */
static int
lrfs_unlock(struct vop_unlock_args *ap)
{
	struct vnode *vp = ap->a_vp;
	int flags = ap->a_flags;
	int mtxlkflag = 0;
	struct lrfs_node *nn;
	struct vnode *lvp;
	int error;

	if ((flags & LK_INTERLOCK) != 0)
		mtxlkflag = 1;
	else if (mtx_owned(VI_MTX(vp)) == 0) {
		VI_LOCK(vp);
		mtxlkflag = 2;
	}
	nn = VTOLRFS(vp);
	if (nn != NULL && (lvp = LRFSVPTOLOWERVP(vp)) != NULL) {
		VI_LOCK_FLAGS(lvp, MTX_DUPOK);
		flags |= LK_INTERLOCK;
		vholdl(lvp);
		VI_UNLOCK(vp);
		error = VOP_UNLOCK(lvp, flags);
		vdrop(lvp);
		if (mtxlkflag == 0)
			VI_LOCK(vp);
	} else {
		if (mtxlkflag == 2)
			VI_UNLOCK(vp);
		error = vop_stdunlock(ap);
	}

	return (error);
}


static int
lrfs_inactive(struct vop_inactive_args *ap)
{
	int ret, proceed;
	struct vnode **first_vp;
	struct lrfs_filter_chain *chain;
	struct vop_generic_args *ga = &ap->a_gen;
	struct vnodeop_desc *descp = ga->a_desc;

	first_vp = VOPARG_OFFSETTO(struct vnode**, descp->vdesc_vp_offsets[0],ap);
	chain = LRFSGETCHAIN((*first_vp));
	
	/*
	 * Lock the chain so nobody can alter it while performing
	 * operations.
	 */
	sx_slock(&chain->chainlck);
	proceed = lrfs_precallbacks_chain(ga, chain, LAREFS_LOOKUP);
	proceed = chain->count - proceed;

	ret = do_lrfs_inactive(ap);	

	lrfs_postcallbacks_chain(ga, chain, LAREFS_LOOKUP, proceed);
	sx_sunlock(&chain->chainlck);

	return ret;
}

/*
 * There is no way to tell that someone issued remove/rmdir operation
 * on the underlying filesystem. For now we just have to release lowervp
 * as soon as possible.
 *
 * Note, we can't release any resources nor remove vnode from hash before 
 * appropriate VXLOCK stuff is is done because other process can find this
 * vnode in hash during inactivation and may be sitting in vget() and waiting
 * for lrfs_inactive to unlock vnode. Thus we will do all those in VOP_RECLAIM.
 */
static int
do_lrfs_inactive(struct vop_inactive_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct thread *td = ap->a_td;

	vp->v_object = NULL;

	/*
	 * If this is the last reference, then free up the vnode
	 * so as not to tie up the lower vnodes.
	 */
	vrecycle(vp, td);

	return (0);
}

/*
 * Now, the VXLOCK is in force and we're free to destroy the lrfs vnode.
 */
static int
lrfs_reclaim(struct vop_reclaim_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct lrfs_node *xp = VTOLRFS(vp);
	struct vnode *lowervp = xp->lrfs_lowervp;

	if (lowervp)
		lrfs_hashrem(xp);
	/*
	 * Use the interlock to protect the clearing of v_data to
	 * prevent faults in lrfs_lock().
	 */
	lockmgr(&vp->v_lock, LK_EXCLUSIVE, NULL);
	VI_LOCK(vp);
	vp->v_data = NULL;
	vp->v_object = NULL;
	vp->v_vnlock = &vp->v_lock;
	VI_UNLOCK(vp);
	if (lowervp)
		vput(lowervp);
	else
		panic("lrfs_reclaim: reclaiming a node with no lowervp");
	free(xp, M_LRFSNODE);

	return (0);
}

static int
lrfs_print(struct vop_print_args *ap)
{
	struct vnode *vp = ap->a_vp;

	printf("\tvp=%p, lowervp=%p\n", vp, LRFSVPTOLOWERVP(vp));
	return (0);
}

/* ARGSUSED */
static int
lrfs_getwritemount(struct vop_getwritemount_args *ap)
{
	struct lrfs_node *xp;
	struct vnode *lowervp;
	struct vnode *vp;

	vp = ap->a_vp;
	VI_LOCK(vp);
	xp = VTOLRFS(vp);
	if (xp && (lowervp = xp->lrfs_lowervp)) {
		VI_LOCK_FLAGS(lowervp, MTX_DUPOK);
		VI_UNLOCK(vp);
		vholdl(lowervp);
		VI_UNLOCK(lowervp);
		VOP_GETWRITEMOUNT(lowervp, ap->a_mpp);
		vdrop(lowervp);
	} else {
		VI_UNLOCK(vp);
		*(ap->a_mpp) = NULL;
	}
	return (0);
}

static int
lrfs_vptofh(struct vop_vptofh_args *ap)
{
	struct vnode *lvp;

	lvp = LRFSVPTOLOWERVP(ap->a_vp);
	return VOP_VPTOFH(lvp, ap->a_fhp);
}

static int
lrfs_vptocnp(struct vop_vptocnp_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vnode **dvp = ap->a_vpp;
	struct vnode *lvp, *ldvp;
	struct ucred *cred = ap->a_cred;
	int error, locked;

	if (vp->v_type == VDIR)
		return (vop_stdvptocnp(ap));

	locked = VOP_ISLOCKED(vp);
	lvp = LRFSVPTOLOWERVP(vp);
	vhold(lvp);
	VOP_UNLOCK(vp, 0); /* vp is held by vn_vptocnp_locked that called us */
	ldvp = lvp;
	error = vn_vptocnp(&ldvp, cred, ap->a_buf, ap->a_buflen);
	vdrop(lvp);
	if (error != 0) {
		vn_lock(vp, locked | LK_RETRY);
		return (ENOENT);
	}

	/*
	 * Exclusive lock is required by insmntque1 call in
	 * lrfs_nodeget()
	 */
	error = vn_lock(ldvp, LK_EXCLUSIVE);
	if (error != 0) {
		vn_lock(vp, locked | LK_RETRY);
		vdrop(ldvp);
		return (ENOENT);
	}
	vref(ldvp);
	vdrop(ldvp);
	error = lrfs_nodeget(vp->v_mount, ldvp, dvp);
	if (error == 0) {
#ifdef DIAGNOSTIC
		LRFSVPTOLOWERVP(*dvp);
#endif
		vhold(*dvp);
		vput(*dvp);
	} else
		vput(ldvp);

	vn_lock(vp, locked | LK_RETRY);
	return (error);
}

static int
lrfs_ioctl(struct vop_ioctl_args *ap)
{
	int retval = ENOTSUP;
	struct vnode *vp, *ldvp;

	vp = ap->a_vp;
	ldvp = LRFSVPTOLOWERVP(vp);

	switch (ap->a_command) {

	case LRFS_ATTACH: {
		struct larefs_attach_info *ainfo;
		struct larefs_filter_t *filter;

		if (vp->v_type != VDIR)
			return (ENOTDIR);

		ainfo = (struct larefs_attach_info *)ap->a_data;
		if (!ainfo)
			return (EINVAL);

		filter = find_filter_inlist(ainfo->name);
		if (!filter)
			return (EINVAL);

		retval = attach_filter(filter, vp, ainfo->priority);
		if (!retval)
			LRFSDEBUG("Filter %s attached\n", filter->name);
		break;
	}
	case LRFS_DETACH: {
		char *name;
		
		if (vp->v_type != VDIR)
			return (ENOTDIR);

		name = (char *)ap->a_data;
		if (!name)
			return (EINVAL);

		retval = try_detach_filter(name, vp);
		if (!retval)
			LRFSDEBUG("Filter %s detached\n", name);
		break;
	}

	case LRFS_TGLACT: {
		char *buffer;
		
		if (vp->v_type != VDIR)
			return (ENOTDIR);

		buffer = (char *)ap->a_data;
		retval = toggle_filter_active(buffer, vp);

		break;
	}

	case LRFS_CHPRIO: {
		struct larefs_prior_info *pinfo;

		if (vp->v_type != VDIR)
			return (ENOTDIR);

		pinfo = (struct larefs_prior_info *)ap->a_data;
		if (!pinfo)
			return (EINVAL);

		retval = try_change_fltpriority(pinfo, vp);
		if (!retval)
			LRFSDEBUG("Priority of filter %s changed to %d\n",
				pinfo->name, pinfo->priority);
		break;
	}

	default:
		retval = lrfs_proceed_oper(&ap->a_gen, LAREFS_IOCTL);
		break;
	}



	return (retval);
}

/*
 * Global vfs data structures
 */
struct vop_vector lrfs_vnodeops = {
	.vop_bypass =		lrfs_bypass,
	.vop_access =		lrfs_access,
	.vop_accessx =		lrfs_accessx,
	.vop_bmap =		VOP_EOPNOTSUPP,
	.vop_getattr =		lrfs_getattr,
	.vop_getwritemount =	lrfs_getwritemount,
	.vop_inactive =		lrfs_inactive,
	.vop_islocked =		vop_stdislocked,
	.vop_lock1 =		lrfs_lock,
	.vop_lookup =		lrfs_lookup,
	.vop_open =		lrfs_open,
	.vop_print =		lrfs_print,
	.vop_reclaim =		lrfs_reclaim,
	.vop_rename =		lrfs_rename,
	.vop_setattr =		lrfs_setattr,
	.vop_strategy =		VOP_EOPNOTSUPP,
	.vop_unlock =		lrfs_unlock,
	.vop_vptocnp =		lrfs_vptocnp,
	.vop_vptofh =		lrfs_vptofh,
	.vop_ioctl =		lrfs_ioctl,
};
