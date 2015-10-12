/*
 * linux/drivers/video/owl/owlfb-sysfs.c
 *
 * Copyright (C) 2014 Actions Corporation
 * Author: Hui Wang  <wanghui@actions-semi.com>
 *
 * Some code and ideas taken from drivers/video/owl/ driver
 * by leopard.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <mach/bootdev.h>
#include <linux/fb.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/mm.h>

#include <video/owldss.h>
#include <video/owlfb.h>

#include "owlfb.h"

extern int dss_mgr_enable(struct owl_overlay_manager *mgr);
extern int dss_mgr_disable(struct owl_overlay_manager *mgr);

static ssize_t show_mirror_to_hdmi(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct owlfb_info *ofbi = FB2OFB(fbi);

	return snprintf(buf, PAGE_SIZE, "%d\n", ofbi->mirror_to_hdmi);
}

static ssize_t store_mirror_to_hdmi(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct owlfb_info *ofbi = FB2OFB(fbi);
	struct owl_overlay *ovl= NULL;
	bool mirror_to_hdmi;
	int r;
	int i = 0;
	int vid = 1;
	
	struct owl_overlay_manager *tv_mgr = owl_dss_get_overlay_manager(OWL_DSS_OVL_MGR_TV);
	struct owl_dss_device *dssdev = tv_mgr->device;
	
	if (owl_get_boot_mode() == OWL_BOOT_MODE_UPGRADE) {
		printk("upgrade process hdmi disabled!!\n");
		return -ENODEV;
	}
	
	r = strtobool(buf, &mirror_to_hdmi);
	if (r)
		return r;

	if (!lock_fb_info(fbi))
		return -ENODEV;

	ofbi->mirror_to_hdmi = mirror_to_hdmi;

	owlfb_get_mem_region(ofbi->region);
	printk(KERN_ERR "store_mirror_to_hdmi %d\n",mirror_to_hdmi);
	if(mirror_to_hdmi){
		dss_mgr_enable(tv_mgr);
		
		for(i = 0 ; i < ofbi->num_overlays ; i++){
			ovl = owl_dss_get_overlay(3 - i);
			if(ovl->manager->id != OWL_DSS_CHANNEL_DIGIT){
				ovl->disable(ovl);
				ovl->unset_manager(ovl);
				ovl->set_manager(ovl,tv_mgr); 
			}
		}
		
		//dssdev->driver->enable_hpd(dssdev, 0);		
		dssdev->driver->get_vid(dssdev,&vid);
		dssdev->driver->set_vid(dssdev,vid);
		
		dssdev->driver->enable(dssdev);		
		
    }else{
    	
    	dssdev->driver->disable(dssdev);
    	
    	dss_mgr_disable(tv_mgr);
    	
    	//dssdev->driver->enable_hpd(dssdev, 1);	
    }   
    
	r = owlfb_apply_changes(fbi, 0);
	if (r)
		goto out;

	r = count;
out:
	owlfb_put_mem_region(ofbi->region);

	unlock_fb_info(fbi);

	return r;
}

static ssize_t show_overlays(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct owlfb_info *ofbi = FB2OFB(fbi);
	struct owlfb_device *fbdev = ofbi->fbdev;
	ssize_t l = 0;
	int t;

	if (!lock_fb_info(fbi))
		return -ENODEV;
	owlfb_lock(fbdev);

	for (t = 0; t < ofbi->num_overlays; t++) {
		struct owl_overlay *ovl = ofbi->overlays[t];
		int ovlnum;

		for (ovlnum = 0; ovlnum < fbdev->num_overlays; ++ovlnum)
			if (ovl == fbdev->overlays[ovlnum])
				break;

		l += snprintf(buf + l, PAGE_SIZE - l, "%s%d",
				t == 0 ? "" : ",", ovlnum);
	}

	l += snprintf(buf + l, PAGE_SIZE - l, "\n");

	owlfb_unlock(fbdev);
	unlock_fb_info(fbi);

	return l;
}

static struct owlfb_info *get_overlay_fb(struct owlfb_device *fbdev,
		struct owl_overlay *ovl)
{
	int i, t;

	for (i = 0; i < fbdev->num_fbs; i++) {
		struct owlfb_info *ofbi = FB2OFB(fbdev->fbs[i]);

		for (t = 0; t < ofbi->num_overlays; t++) {
			if (ofbi->overlays[t] == ovl)
				return ofbi;
		}
	}

	return NULL;
}

static ssize_t store_overlays(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct owlfb_info *ofbi = FB2OFB(fbi);
	struct owlfb_device *fbdev = ofbi->fbdev;
	struct owl_overlay *ovls[OWLFB_MAX_OVL_PER_FB];
	struct owl_overlay *ovl;
	int num_ovls, r, i;
	int len;
	bool added = false;

	num_ovls = 0;

	len = strlen(buf);
	if (buf[len - 1] == '\n')
		len = len - 1;

	if (!lock_fb_info(fbi))
		return -ENODEV;
	owlfb_lock(fbdev);

	if (len > 0) {
		char *p = (char *)buf;
		int ovlnum;

		while (p < buf + len) {
			int found;
			if (num_ovls == OWLFB_MAX_OVL_PER_FB) {
				r = -EINVAL;
				goto out;
			}

			ovlnum = simple_strtoul(p, &p, 0);
			if (ovlnum > fbdev->num_overlays) {
				r = -EINVAL;
				goto out;
			}

			found = 0;
			for (i = 0; i < num_ovls; ++i) {
				if (ovls[i] == fbdev->overlays[ovlnum]) {
					found = 1;
					break;
				}
			}

			if (!found)
				ovls[num_ovls++] = fbdev->overlays[ovlnum];

			p++;
		}
	}

	for (i = 0; i < num_ovls; ++i) {
		struct owlfb_info *ofbi2 = get_overlay_fb(fbdev, ovls[i]);
		if (ofbi2 && ofbi2 != ofbi) {
			dev_err(fbdev->dev, "overlay already in use\n");
			r = -EINVAL;
			goto out;
		}
	}

	/* detach unused overlays */
	for (i = 0; i < ofbi->num_overlays; ++i) {
		int t, found;

		ovl = ofbi->overlays[i];

		found = 0;

		for (t = 0; t < num_ovls; ++t) {
			if (ovl == ovls[t]) {
				found = 1;
				break;
			}
		}

		if (found)
			continue;

		DBG("detaching %d\n", ofbi->overlays[i]->id);

		owlfb_get_mem_region(ofbi->region);

		owlfb_overlay_enable(ovl, 0);

		if (ovl->manager)
			ovl->manager->apply(ovl->manager);

		owlfb_put_mem_region(ofbi->region);

		for (t = i + 1; t < ofbi->num_overlays; t++) {
			ofbi->rotation[t-1] = ofbi->rotation[t];
			ofbi->overlays[t-1] = ofbi->overlays[t];
		}

		ofbi->num_overlays--;
		i--;
	}

	for (i = 0; i < num_ovls; ++i) {
		int t, found;

		ovl = ovls[i];

		found = 0;

		for (t = 0; t < ofbi->num_overlays; ++t) {
			if (ovl == ofbi->overlays[t]) {
				found = 1;
				break;
			}
		}

		if (found)
			continue;
		ofbi->rotation[ofbi->num_overlays] = 0;
		ofbi->overlays[ofbi->num_overlays++] = ovl;

		added = true;
	}

	if (added) {
		owlfb_get_mem_region(ofbi->region);

		r = owlfb_apply_changes(fbi, 0);

		owlfb_put_mem_region(ofbi->region);

		if (r)
			goto out;
	}

	r = count;
out:
	owlfb_unlock(fbdev);
	unlock_fb_info(fbi);

	return r;
}

static ssize_t show_overlays_rotate(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct owlfb_info *ofbi = FB2OFB(fbi);
	ssize_t l = 0;
	int t;

	if (!lock_fb_info(fbi))
		return -ENODEV;

	for (t = 0; t < ofbi->num_overlays; t++) {
		l += snprintf(buf + l, PAGE_SIZE - l, "%s%d",
				t == 0 ? "" : ",", ofbi->rotation[t]);
	}

	l += snprintf(buf + l, PAGE_SIZE - l, "\n");

	unlock_fb_info(fbi);

	return l;
}

static ssize_t store_overlays_rotate(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct owlfb_info *ofbi = FB2OFB(fbi);
	int num_ovls = 0, r, i;
	int len;
	bool changed = false;
	u8 rotation[OWLFB_MAX_OVL_PER_FB];

	len = strlen(buf);
	if (buf[len - 1] == '\n')
		len = len - 1;

	if (!lock_fb_info(fbi))
		return -ENODEV;

	if (len > 0) {
		char *p = (char *)buf;

		while (p < buf + len) {
			int rot;

			if (num_ovls == ofbi->num_overlays) {
				r = -EINVAL;
				goto out;
			}

			rot = simple_strtoul(p, &p, 0);
			if (rot < 0 || rot > 3) {
				r = -EINVAL;
				goto out;
			}

			if (ofbi->rotation[num_ovls] != rot)
				changed = true;

			rotation[num_ovls++] = rot;

			p++;
		}
	}

	if (num_ovls != ofbi->num_overlays) {
		r = -EINVAL;
		goto out;
	}

	if (changed) {
		for (i = 0; i < num_ovls; ++i)
			ofbi->rotation[i] = rotation[i];

		owlfb_get_mem_region(ofbi->region);

		r = owlfb_apply_changes(fbi, 0);

		owlfb_put_mem_region(ofbi->region);

		if (r)
			goto out;

		/* FIXME error handling? */
	}

	r = count;
out:
	unlock_fb_info(fbi);

	return r;
}

static ssize_t show_size(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct owlfb_info *ofbi = FB2OFB(fbi);

	return snprintf(buf, PAGE_SIZE, "%lu\n", ofbi->region->size);
}

static ssize_t store_size(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct owlfb_info *ofbi = FB2OFB(fbi);
	struct owlfb_device *fbdev = ofbi->fbdev;
	struct owlfb_mem_region *rg;
	unsigned long size;
	int r;
	int i;

	r = kstrtoul(buf, 0, &size);
	if (r)
		return r;

	size = PAGE_ALIGN(size);

	if (!lock_fb_info(fbi))
		return -ENODEV;

	rg = ofbi->region;

	down_write_nested(&rg->lock, rg->id);
	atomic_inc(&rg->lock_count);

	if (atomic_read(&rg->map_count)) {
		r = -EBUSY;
		goto out;
	}

	for (i = 0; i < fbdev->num_fbs; i++) {
		struct owlfb_info *ofbi2 = FB2OFB(fbdev->fbs[i]);
		int j;

		if (ofbi2->region != rg)
			continue;

		for (j = 0; j < ofbi2->num_overlays; j++) {
			struct owl_overlay *ovl;
			ovl = ofbi2->overlays[j];
			if (ovl->is_enabled(ovl)) {
				r = -EBUSY;
				goto out;
			}
		}
	}

	if (size != ofbi->region->size) {
		r = owlfb_realloc_fbmem(fbi, size, ofbi->region->type);
		if (r) {
			dev_err(dev, "realloc fbmem failed\n");
			goto out;
		}
	}

	r = count;
out:
	atomic_dec(&rg->lock_count);
	up_write(&rg->lock);

	unlock_fb_info(fbi);

	return r;
}

static ssize_t show_phys(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct owlfb_info *ofbi = FB2OFB(fbi);
	short offset = 0; 

	offset += snprintf(buf, PAGE_SIZE, "%0x\n", ofbi->region->paddr);
	return offset;
}

static ssize_t show_virt(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct owlfb_info *ofbi = FB2OFB(fbi);

	return snprintf(buf, PAGE_SIZE, "%p\n", ofbi->region->vaddr);
}

static ssize_t show_upd_mode(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	enum owlfb_update_mode mode;
	int r;

	r = owlfb_get_update_mode(fbi, &mode);

	if (r)
		return r;

	return snprintf(buf, PAGE_SIZE, "%u\n", (unsigned)mode);
}

static ssize_t store_upd_mode(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	unsigned mode;
	int r;

	r = kstrtouint(buf, 0, &mode);
	if (r)
		return r;

	r = owlfb_set_update_mode(fbi, mode);
	if (r)
		return r;

	return count;
}

static struct device_attribute owlfb_attrs[] = {
	__ATTR(mirror_to_hdmi, S_IRUGO | S_IWUSR, show_mirror_to_hdmi, store_mirror_to_hdmi),
	__ATTR(size, S_IRUGO | S_IWUSR, show_size, store_size),
	__ATTR(overlays, S_IRUGO | S_IWUSR, show_overlays, store_overlays),
	__ATTR(overlays_rotate, S_IRUGO | S_IWUSR, show_overlays_rotate,
			store_overlays_rotate),
	__ATTR(phys_addr, S_IRUGO, show_phys, NULL),
	__ATTR(virt_addr, S_IRUGO, show_virt, NULL),
	__ATTR(update_mode, S_IRUGO | S_IWUSR, show_upd_mode, store_upd_mode),
};

int owlfb_create_sysfs(struct owlfb_device *fbdev)
{
	int i;
	int r;

	DBG("create sysfs for fbs\n");
	for (i = 0; i < fbdev->num_fbs; i++) {
		int t;
		for (t = 0; t < ARRAY_SIZE(owlfb_attrs); t++) {
			r = device_create_file(fbdev->fbs[i]->dev,
					&owlfb_attrs[t]);

			if (r) {
				dev_err(fbdev->dev, "failed to create sysfs "
						"file\n");
				return r;
			}
			
			#ifdef CONFIG_FB_DEFAULT_MIRROR_TO_HDMI
			store_mirror_to_hdmi(fbdev->fbs[i]->dev,
						&owlfb_attrs[0], "1", sizeof("1"));			
			#endif
		}
	}

	return 0;
}

void owlfb_remove_sysfs(struct owlfb_device *fbdev)
{
	int i, t;

	DBG("remove sysfs for fbs\n");
	for (i = 0; i < fbdev->num_fbs; i++) {
		for (t = 0; t < ARRAY_SIZE(owlfb_attrs); t++)
			device_remove_file(fbdev->fbs[i]->dev,
					&owlfb_attrs[t]);
	}
}

