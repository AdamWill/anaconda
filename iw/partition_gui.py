#
# partition_gui.py: allows the user to choose how to partition their disks
#
# Matt Wilson <msw@redhat.com>
# Michael Fulbright <msf@redhat.com>
#
# Copyright 2001-2002 Red Hat, Inc.
#
# This software may be freely redistributed under the terms of the GNU
# library public license.
#
# You should have received a copy of the GNU Library Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
#

import gobject
import gtk
import gnome.canvas
import pango
import autopart
import gui
import parted
import string
import copy
import types
import raid
from iw_gui import *

import lvm_dialog_gui
import raid_dialog_gui
import partition_dialog_gui

from rhpl.translate import _, N_
from partitioning import *
from partIntfHelpers import *
from partedUtils import *
from fsset import *
from partRequests import *
from constants import *
from partition_ui_helpers_gui import *

STRIPE_HEIGHT = 32.0
LOGICAL_INSET = 3.0
CANVAS_WIDTH_800 = 500
CANVAS_WIDTH_640 = 400
CANVAS_HEIGHT = 200
TREE_SPACING = 2

MODE_ADD = 1
MODE_EDIT = 2

class DiskStripeSlice:
    def eventHandler(self, widget, event):
        if event.type == gtk.gdk.BUTTON_PRESS:
            if event.button == 1:
                self.parent.selectSlice(self.partition, 1)
        elif event.type == gtk.gdk._2BUTTON_PRESS:
            self.editCb()
                
        return gtk.TRUE

    def shutDown(self):
        self.parent = None
        if self.group:
            self.group.destroy()
            self.group = None
        del self.partition

    def select(self):
        if self.partition.type != parted.PARTITION_EXTENDED:
            self.group.raise_to_top()
        self.box.set(outline_color="red")
        self.box.set(fill_color=self.selectColor())

    def deselect(self):
        self.box.set(outline_color="black", fill_color=self.fillColor())

    def getPartition(self):
        return self.partition

    def fillColor(self):
        if self.partition.type & parted.PARTITION_FREESPACE:
            return "grey88"
        return "white"

    def selectColor(self):
        if self.partition.type & parted.PARTITION_FREESPACE:
            return "cornsilk2"
        return "cornsilk1"

    def hideOrShowText(self):
        return
        if self.box.get_bounds()[2] < self.text.get_bounds()[2]:
            self.text.hide()
        else:
            self.text.show()

    def sliceText(self):
        if self.partition.type & parted.PARTITION_EXTENDED:
            return ""
        if self.partition.type & parted.PARTITION_FREESPACE:
            rc = "Free\n"
        else:
            rc = "%s\n" % (get_partition_name(self.partition),)
        rc = rc + "%d MB" % (getPartSizeMB(self.partition),)
        return rc

    def getDeviceName(self):
        return get_partition_name(self.partition)

    def update(self):
        disk = self.parent.getDisk()
        totalSectors = float(disk.dev.heads
                             * disk.dev.sectors
                             * disk.dev.cylinders)

        # XXX hack but will work for now
        if gtk.gdk.screen_width() > 640:
            width = CANVAS_WIDTH_800
        else:
            width = CANVAS_WIDTH_640

        xoffset = self.partition.geom.start / totalSectors * width
        xlength = self.partition.geom.length / totalSectors * width
        if self.partition.type & parted.PARTITION_LOGICAL:
            yoffset = 0.0 + LOGICAL_INSET
            yheight = STRIPE_HEIGHT - (LOGICAL_INSET * 2)
            texty = 0.0
        else:
            yoffset = 0.0
            yheight = STRIPE_HEIGHT
            texty = LOGICAL_INSET
        self.group.set(x=xoffset, y=yoffset)
        self.box.set(x1=0.0, y1=0.0, x2=xlength,
                     y2=yheight, fill_color=self.fillColor(),
                     outline_color='black', width_units=1.0)
        self.text.set(x=2.0, y=texty + 2.0, text=self.sliceText(),
                      fill_color='black',
                      anchor=gtk.ANCHOR_NW, clip=gtk.TRUE,
                      clip_width=xlength-1, clip_height=yheight-1)
        self.hideOrShowText()
       
    def __init__(self, parent, partition, treeView, editCb):
        self.text = None
        self.partition = partition
        self.parent = parent
        self.treeView = treeView
        self.editCb = editCb
        pgroup = parent.getGroup()

        self.group = pgroup.add(gnome.canvas.CanvasGroup)
        self.box = self.group.add(gnome.canvas.CanvasRect)
        self.group.connect("event", self.eventHandler)
        self.text = self.group.add(gnome.canvas.CanvasText,
                                    font="helvetica", size_points=8)
        self.update()

class DiskStripe:
    def __init__(self, drive, disk, group, tree, editCb):
        self.disk = disk
        self.group = group
        self.tree = tree
        self.drive = drive
        self.slices = []
        self.hash = {}
        self.editCb = editCb
        self.selected = None

        # XXX hack but will work for now
        if gtk.gdk.screen_width() > 640:
            width = CANVAS_WIDTH_800
        else:
            width = CANVAS_WIDTH_640
        
        group.add(gnome.canvas.CanvasRect, x1=0.0, y1=10.0, x2=width,
                  y2=STRIPE_HEIGHT, fill_color='green',
                  outline_color='grey71', width_units=1.0)
        group.lower_to_bottom()

    def shutDown(self):
        while self.slices:
            slice = self.slices.pop()
            slice.shutDown()
        if self.group:
            self.group.destroy()
            self.group = None
        del self.disk

    def holds(self, partition):
        return self.hash.has_key(partition)

    def getSlice(self, partition):
        return self.hash[partition]
   
    def getDisk(self):
        return self.disk

    def getDrive(self):
        return self.drive

    def getGroup(self):
        return self.group

    def selectSlice(self, partition, updateTree=0):
        self.deselect()
        slice = self.hash[partition]
        slice.select()

        # update selection of the tree
        if updateTree:
            self.tree.selectPartition(partition)
        self.selected = slice

    def deselect(self):
        if self.selected:
            self.selected.deselect()
        self.selected = None
    
    def add(self, partition):
        stripe = DiskStripeSlice(self, partition, self.tree, self.editCb)
        self.slices.append(stripe)
        self.hash[partition] = stripe

class DiskStripeGraph:
    def __init__(self, tree, editCb):
        self.canvas = gnome.canvas.Canvas()
        self.diskStripes = []
        self.textlabels = []
        self.tree = tree
        self.editCb = editCb
        self.next_ypos = 0.0

    def __del__(self):
        self.shutDown()
        
    def shutDown(self):
        # remove any circular references so we can clean up
        while self.diskStripes:
            stripe = self.diskStripes.pop()
            stripe.shutDown()

        while self.textlabels:
            lab = self.textlabels.pop()
            lab.destroy()

        self.next_ypos = 0.0

    def getCanvas(self):
        return self.canvas

    def selectSlice(self, partition):
        for stripe in self.diskStripes:
            stripe.deselect()
            if stripe.holds(partition):
                stripe.selectSlice(partition)

    def getSlice(self, partition):
        for stripe in self.diskStripes:
            if stripe.holds(partition):
                return stripe.getSlice(partition)

    def getDisk(self, partition):
        for stripe in self.diskStripes:
            if stripe.holds(partition):
                return stripe.getDisk()

    def add(self, drive, disk):
#        yoff = len(self.diskStripes) * (STRIPE_HEIGHT + 5)
        yoff = self.next_ypos
        text = self.canvas.root().add(gnome.canvas.CanvasText,
                                      x=0.0, y=yoff,
                                      font="sans",
                                      size_points=9)
        drivetext = ("Drive %s (Geom: %s/%s/%s) "
                     "(Model: %s)") % ('/dev/' + drive,
                                       disk.dev.cylinders,
                                       disk.dev.heads,
                                       disk.dev.sectors,
                                       disk.dev.model)
        text.set(text=drivetext, fill_color='black', anchor=gtk.ANCHOR_NW,
                 weight=pango.WEIGHT_BOLD)
        (xxx1, yyy1, xxx2, yyy2) =  text.get_bounds()
        textheight = yyy2 - yyy1
        self.textlabels.append(text)
        group = self.canvas.root().add(gnome.canvas.CanvasGroup,
                                       x=0, y=yoff+textheight)
        stripe = DiskStripe(drive, disk, group, self.tree, self.editCb)
        self.diskStripes.append(stripe)
        self.next_ypos = self.next_ypos + STRIPE_HEIGHT+textheight+10
        return stripe

class DiskTreeModelHelper:
    def __init__(self, model, columns, iter):
        self.model = model
        self.iter = iter
        self.columns = columns

    def __getitem__(self, key):
        if type(key) == types.StringType:
            key = self.columns[key]
        try:
            return self.model.get_value(self.iter, key)
        except:
            return None

    def __setitem__(self, key, value):
        if type(key) == types.StringType:
            key = self.columns[key]
        self.model.set_value(self.iter, key, value)

class DiskTreeModel(gtk.TreeStore):
    isLeaf = -3
    isFormattable = -2
    
    # format: column header, type, x alignment, hide?, visibleKey
    titles = ((N_("Device"), gobject.TYPE_STRING, 0.0, 0, 0),
              (N_("Mount Point"), gobject.TYPE_STRING, 0.0, 0, isLeaf),
              (N_("Type"), gobject.TYPE_STRING, 0.0, 0, 0),
              (N_("Format"), gobject.TYPE_BOOLEAN, 0.0, 0, isFormattable),
              (N_("Size (MB)"), gobject.TYPE_STRING, 1.0, 0, isLeaf),
              (N_("Start"), gobject.TYPE_STRING, 1.0, 0, 1),
              (N_("End"), gobject.TYPE_STRING, 1.0, 0, 1),
              # the following must be the last two
              ("IsLeaf", gobject.TYPE_BOOLEAN, 0.0, 1, 0),
              ("IsFormattable", gobject.TYPE_BOOLEAN, 0.0, 1, 0),
              ("PyObject", gobject.TYPE_PYOBJECT, 0.0, 1, 0))
    
    def __init__(self):
        self.titleSlot = {}
        i = 0
        types = [self]
        self.columns = []
        for title, kind, alignment, hide, key in self.titles:
            self.titleSlot[title] = i
            types.append(kind)
            if hide:
                i += 1
                continue
            elif kind == gobject.TYPE_BOOLEAN:
                renderer = gtk.CellRendererToggle()
                propertyMapping = {'active': i}
            elif (kind == gobject.TYPE_STRING or
                  kind == gobject.TYPE_INT):
                renderer = gtk.CellRendererText()
                propertyMapping = {'text': i}

            # wire in the cells that we want only visible on leaf nodes to
            # the special leaf node column.
            if key < 0:
                propertyMapping['visible'] = len(self.titles) + key
                
            renderer.set_property('xalign', alignment)
            col = apply(gtk.TreeViewColumn, (_(title), renderer),
                        propertyMapping)
            self.columns.append(col)
            i += 1

        apply(gtk.TreeStore.__init__, types)

        self.view = gtk.TreeView(self)
        # append all of the columns
        map(self.view.append_column, self.columns)

    def getTreeView(self):
        return self.view

    def selectPartition(self, partition):
        pyobject = self.titleSlot['PyObject']
        iter = self.get_iter_first()
        next = 1
	parentstack = []
	parent = None
        # iterate over the list, looking for the current mouse selection
        while next:
            # if this is a parent node, get the first child and iter over them
            if self.iter_has_child(iter):
		parent = iter
                parentstack.append(parent)
                iter = self.iter_children(parent)
                continue
            # if it's not a parent node and the mouse matches, select it.
            elif self.get_value(iter, pyobject) == partition:
                path = self.get_path(parent)
                self.view.expand_row(path, gtk.TRUE)
                selection = self.view.get_selection()
                selection.unselect_all()
                selection.select_iter(iter)
                path = self.get_path(iter)
                col = self.view.get_column(0)
                self.view.set_cursor(path, col, gtk.FALSE)
                self.view.scroll_to_cell(path, col, gtk.TRUE, 0.5, 0.5)
                return
            # get the next row.
            next = self.iter_next(iter)
            # if there isn't a next row and we had a parent, go to the node
            # after the parent we've just gotten the children of.
            if not next and parent:
		while not next and parent:
		    next = self.iter_next(parent)
		    iter = parent
		    if len(parentstack) > 0:
			parent = parentstack.pop()

    def getCurrentPartition(self):
        selection = self.view.get_selection()
        rc = selection.get_selected()
        if rc:
            model, iter = rc
        else:
            return None

        pyobject = self.titleSlot['PyObject']
	try:
	    return self.get_value(iter, pyobject)
        except:
            return None

    def resetSelection(self):
        pass
##         selection = self.view.get_selection()
##         selection.set_mode(gtk.SELECTION_SINGLE)
##         selection.set_mode(gtk.SELECTION_BROWSE)

    def clear(self):
        selection = self.view.get_selection()
        selection.unselect_all()
        gtk.TreeStore.clear(self)
        
    def __getitem__(self, iter):
        if type(iter) == gtk.TreeIter:
            return DiskTreeModelHelper(self, self.titleSlot, iter)
        raise KeyError, iter


class PartitionWindow(InstallWindow):
    def __init__(self, ics):
	InstallWindow.__init__(self, ics)
        ics.setTitle(_("Disk Setup"))
        ics.setNextEnabled(gtk.TRUE)
        if iutil.getArch() == "s390":
            ics.readHTML("dasd-s390")
        else:
            ics.readHTML("partition")
        self.parent = ics.getICW().window

    def quit(self):
        pass

    def presentPartitioningComments(self,title, labelstr1, labelstr2, comments,
				    type="ok", custom_buttons=None):
        win = gtk.Dialog(title)
        gui.addFrame(win)
        
        if type == "ok":
            win.add_button('gtk-ok', 1)
	    defaultchoice = 0
        elif type == "yesno":
            win.add_button('gtk-no', 2)
            win.add_button('gtk-yes', 1)
	    defaultchoice = 1
	elif type == "continue":
            win.add_button('gtk-cancel', 0)
            win.add_button(_("Continue"), 1)
	    defaultchoice = 1
	elif type == "custom":
	    rid=0

	    for button in custom_buttons:
		widget = win.add_button(button, rid)
		rid = rid + 1

            defaultchoice = rid - 1
	    

        image = gtk.Image()
        image.set_from_stock('gtk-dialog-warning', gtk.ICON_SIZE_DIALOG)
        hbox = gtk.HBox(gtk.FALSE)
        hbox.pack_start(image, gtk.FALSE)

        buffer = gtk.TextBuffer(None)
        buffer.set_text(comments)
        text = gtk.TextView()
        text.set_buffer(buffer)
        text.set_property("editable", gtk.FALSE)
        text.set_property("cursor_visible", gtk.FALSE)
        text.set_wrap_mode(gtk.WRAP_WORD)
        
        sw = gtk.ScrolledWindow()
        sw.add(text)
        sw.set_policy(gtk.POLICY_AUTOMATIC, gtk.POLICY_AUTOMATIC)
        sw.set_shadow_type(gtk.SHADOW_IN)
        
        info1 = gtk.Label(labelstr1)
        info1.set_line_wrap(gtk.TRUE)
#        info1.set_size_request(300, -1)

        info2 = gtk.Label(labelstr2)
        info2.set_line_wrap(gtk.TRUE)
#        info2.set_size_request(300, -1)
        
        vbox = gtk.VBox(gtk.FALSE)
        vbox.pack_start(info1, gtk.FALSE)
        vbox.pack_start(sw, gtk.TRUE)
        vbox.pack_start(info2, gtk.FALSE)
        hbox.pack_start(vbox, gtk.FALSE)

        win.vbox.pack_start(hbox)
#        win.set_size_request(400,300)
        win.set_position(gtk.WIN_POS_CENTER)
        win.set_default_response(defaultchoice)
        win.show_all()
        rc = win.run()
        win.destroy()
        return rc
        
    def getNext(self):
        (errors, warnings) = self.partitions.sanityCheckAllRequests(self.diskset)

        if errors:
            labelstr1 =  _("The following critical errors exist "
                           "with your requested partitioning "
                           "scheme.")
            labelstr2 = _("These errors must be corrected prior "
                          "to continuing with your install of "
                          "%s.") % (productName,)

            commentstr = string.join(errors, "\n\n")
            
            self.presentPartitioningComments(_("Partitioning Errors"),
                                             labelstr1, labelstr2,
                                             commentstr, type="ok")
            raise gui.StayOnScreen
        
        if warnings:
            labelstr1 = _("The following warnings exist with "
                         "your requested partition scheme.")
            labelstr2 = _("Would you like to continue with "
                         "your requested partitioning "
                         "scheme?")
            
            commentstr = string.join(warnings, "\n\n")
            rc = self.presentPartitioningComments(_("Partitioning Warnings"),
                                                  labelstr1, labelstr2,
                                                  commentstr,
						  type="yesno")
            if rc != 1:
                raise gui.StayOnScreen

        formatWarnings = getPreExistFormatWarnings(self.partitions,
                                                   self.diskset)
        if formatWarnings:
            labelstr1 = _("The following pre-existing partitions have been "
                          "selected to be formatted, destroying all data.")

#            labelstr2 = _("Select 'Yes' to continue and format these "
#                          "partitions, or 'No' to go back and change these "
#                          "settings.")
            labelstr2 = ""
            commentstr = ""
            for (dev, type, mntpt) in formatWarnings:
                commentstr = commentstr + \
                        "/dev/%s         %s         %s\n" % (dev,type,mntpt)

            rc = self.presentPartitioningComments(_("Format Warnings"),
                                                  labelstr1, labelstr2,
                                                  commentstr,
						  type="custom",
						  custom_buttons=["gtk-cancel",
								  _("Format")])
            if rc != 1:
                raise gui.StayOnScreen

        
        self.diskStripeGraph.shutDown()
        self.tree.clear()
        del self.parent
        return None

    def getPrev(self):
        self.diskStripeGraph.shutDown()
        self.tree.clear()
        del self.parent
        return None
    
    def populate(self, initial = 0):
        drives = self.diskset.disks.keys()
        drives.sort()

        self.tree.resetSelection()
        
	# first do LVM
        lvmrequests = self.partitions.getLVMRequests()
        if lvmrequests:
	    lvmparent = self.tree.append(None)
	    self.tree[lvmparent]['Device'] = _("LVM Volume Groups")
            for vgname in lvmrequests.keys():
		vgrequest = self.partitions.getRequestByVolumeGroupName(vgname)
		rsize = vgrequest.getActualSize(self.partitions, self.diskset)

                vgparent = self.tree.append(lvmparent)
		self.tree[vgparent]['Device'] = _("LVM: %s") % (vgname,)
		self.tree[vgparent]['Mount Point'] = ""
		self.tree[vgparent]['Start'] = ""
		self.tree[vgparent]['End'] = ""
		self.tree[vgparent]['Size (MB)'] = "%8.0f" % (rsize,)
		self.tree[vgparent]['Type'] = _("LVM Volume Group")
                self.tree[vgparent]['PyObject'] = str(vgrequest.uniqueID)
		for lvrequest in lvmrequests[vgname]:
		    iter = self.tree.append(vgparent)
		    self.tree[iter]['Device'] = lvrequest.logicalVolumeName
		    if lvrequest.fstype and lvrequest.mountpoint:
			self.tree[iter]['Mount Point'] = lvrequest.mountpoint
		    else:
			self.tree[iter]['Mount Point'] = ""
		    self.tree[iter]['Size (MB)'] = "%g" % (lvrequest.getActualSize(self.partitions, self.diskset),)
		    self.tree[iter]['PyObject'] = str(lvrequest.uniqueID)
		
                    ptype = lvrequest.fstype.getName()
                    self.tree[iter]['Format'] = lvrequest.format
                    self.tree[iter]['IsFormattable'] = lvrequest.fstype.isFormattable()
		    self.tree[iter]['IsLeaf'] = gtk.TRUE
		    self.tree[iter]['Type'] = ptype
		    self.tree[iter]['Start'] = ""
		    self.tree[iter]['End'] = ""
#		    self.tree[iter]['Start'] = _("N/A")
#		    self.tree[iter]['End'] = _("N/A")

        # handle RAID next
        raidrequests = self.partitions.getRaidRequests()
        if raidrequests:
	    raidparent = self.tree.append(None)
	    self.tree[raidparent]['Device'] = _("RAID Devices")
            for request in raidrequests:
                iter = self.tree.append(raidparent)

                if request and request.mountpoint:
                    self.tree[iter]["Mount Point"] = request.mountpoint
                if request.fstype:
                    ptype = request.fstype.getName()
                    
                    self.tree[iter]['Format'] = request.format
                    self.tree[iter]['IsFormattable'] = request.fstype.isFormattable()
                else:
                    ptype = _("None")
                    self.tree[iter]['IsFormattable'] = gtk.FALSE

                device = "/dev/md%d" % (request.raidminor,)
                self.tree[iter]['IsLeaf'] = gtk.TRUE
                self.tree[iter]['Device'] = device
                self.tree[iter]['Type'] = ptype
#		self.tree[iter]['Start'] = _("N/A")
#		self.tree[iter]['End'] = _("N/A")
                self.tree[iter]['Start'] = ""
                self.tree[iter]['End'] = ""
                self.tree[iter]['Size (MB)'] = "%g" % (request.getActualSize(self.partitions, self.diskset),)
                self.tree[iter]['PyObject'] = str(request.uniqueID)
                
	# now normal partitions
	drvparent = self.tree.append(None)
	self.tree[drvparent]['Device'] = _("Hard Drives")
        for drive in drives:
            disk = self.diskset.disks[drive]

            # add a disk stripe to the graph
            stripe = self.diskStripeGraph.add(drive, disk)

            # add a parent node to the tree
            parent = self.tree.append(drvparent)
            self.tree[parent]['Device'] = '/dev/%s' % (drive,)
            sectorsPerCyl = disk.dev.heads * disk.dev.sectors

            extendedParent = None
            part = disk.next_partition()
            while part:
                if part.type & parted.PARTITION_METADATA:
                    part = disk.next_partition(part)
                    continue

                stripe.add(part)
                device = get_partition_name(part)
                request = self.partitions.getRequestByDeviceName(device)

                if part.type == parted.PARTITION_EXTENDED:
                    if extendedParent:
                        raise RuntimeError, ("can't handle more than "
                                             "one extended partition per disk")
                    extendedParent = self.tree.append(parent)
                    iter = extendedParent
                elif part.type & parted.PARTITION_LOGICAL:
                    if not extendedParent:
                        raise RuntimeError, ("crossed logical partition "
                                             "before extended")
                    iter = self.tree.append(extendedParent)
                    self.tree[iter]['IsLeaf'] = gtk.TRUE
                else:
                    iter = self.tree.append(parent)
                    self.tree[iter]['IsLeaf'] = gtk.TRUE
                    
                if request and request.mountpoint:
                    self.tree[iter]['Mount Point'] = request.mountpoint
                else:
                    self.tree[iter]['Mount Point'] = ""

                if request and request.fstype:
                    self.tree[iter]['IsFormattable'] = request.fstype.isFormattable()
                
                if part.type & parted.PARTITION_FREESPACE:
                    ptype = _("Free space")
                elif part.type == parted.PARTITION_EXTENDED:
                    ptype = _("Extended")
                elif part.get_flag(parted.PARTITION_RAID) == 1:
                    ptype = _("software RAID")
                elif part.fs_type:
                    if request and request.fstype != None:
                        ptype = request.fstype.getName()
                        if ptype == "foreign":
                            ptype = map_foreign_to_fsname(part.native_type)
                    else:
                        ptype = part.fs_type.name
                    self.tree[iter]['Format'] = request.format
                else:
                    if request and request.fstype != None:
                        ptype = request.fstype.getName()
                        
                        if ptype == "foreign":
                            ptype = map_foreign_to_fsname(part.native_type)
                    else:
                        ptype = _("None")
                if part.type & parted.PARTITION_FREESPACE:
                    devname = _("Free")
                else:
                    devname = '/dev/%s' % (device,)
                self.tree[iter]['Device'] = devname
                self.tree[iter]['Type'] = ptype
                self.tree[iter]['Start'] = str(start_sector_to_cyl(disk.dev,
                                                                   part.geom.start))
                self.tree[iter]['End'] = str(end_sector_to_cyl(disk.dev,
                                                               part.geom.end))
                size = getPartSizeMB(part)
                if size < 1.0:
                    sizestr = "< 1"
                else:
                    sizestr = "%8.0f" % (size)
                self.tree[iter]['Size (MB)'] = sizestr
                self.tree[iter]['PyObject'] = part
                
                part = disk.next_partition(part)

        canvas = self.diskStripeGraph.getCanvas()
        apply(canvas.set_scroll_region, canvas.root().get_bounds())
        self.treeView.expand_all()

    def treeActivateCb(self, view, path, col):
        if self.tree.getCurrentPartition():
            self.editCb()
        
    def treeSelectCb(self, selection, *args):
        rc = selection.get_selected()
        if rc:
            model, iter = rc
        else:
            return
        partition = model[iter]['PyObject']
        if partition:
            self.diskStripeGraph.selectSlice(partition)

    def newCB(self, widget):
        # create new request of size 1M
        request = NewPartitionSpec(fileSystemTypeGetDefault(), size = 100)

        self.editPartitionRequest(request, isNew = 1)

    def deleteCb(self, widget):
        curselection = self.tree.getCurrentPartition()

        if (iutil.getArch() == "s390":
            and type(partition) != type("RAID"):
            self.intf.messageWindow(_("Error"),
                                    _("DASD partitions can only be deleted with fdasd"))
            return
        if doDeletePartitionByRequest(self.intf, self.partitions, curselection):
            self.refresh()
            
    def resetCb(self, *args):
        if not confirmResetPartitionState(self.intf):
            return
        
        self.diskStripeGraph.shutDown()
        self.newFsset = self.fsset.copy()
        self.diskset.refreshDevices()
        self.partitions.setFromDisk(self.diskset)
        self.tree.clear()
        self.populate()

    def refresh(self):
        self.diskStripeGraph.shutDown()
        self.tree.clear()
        try:
            autopart.doPartitioning(self.diskset, self.partitions)
            rc = 0
        except PartitioningError, msg:
            self.intf.messageWindow(_("Error Partitioning"),
                   _("Could not allocate requested partitions: %s.") % (msg))
            rc = -1
        except PartitioningWarning, msg:
            # XXX somebody other than me should make this look better
            # XXX this doesn't handle the 'delete /boot partition spec' case
            #     (it says 'add anyway')
            dialog = gtk.MessageDialog(self.parent, 0, gtk.MESSAGE_ERROR,
                                       gtk.BUTTONS_NONE,
                                       _("Warning: %s.") % (msg))
            gui.addFrame(dialog)
            button = gtk.Button(_("_Modify Partition"))
            dialog.add_action_widget(button, 1)
            button = gtk.Button(_("_Continue"))
            dialog.add_action_widget(button, 2)
            dialog.set_position(gtk.WIN_POS_CENTER)

            dialog.show_all()
            rc = dialog.run()
            dialog.destroy()
            
            if rc == 1:
                rc = -1
            else:
                rc = 0
                req = self.partitions.getBootableRequest()
                if req:
                    req.ignoreBootConstraints = 1

        self.populate()
        return rc

    def editCb(self, *args):
        part = self.tree.getCurrentPartition()

        (type, request) = doEditPartitionByRequest(self.intf, self.partitions,
                                                   part)
        if request:
            if type == "RAID":
                self.editRaidRequest(request)
	    elif type == "LVMVG":
		self.editLVMVolumeGroup(request)
	    elif type == "LVMLV":
		vgrequest = self.partitions.getRequestByID(request.volumeGroup)
		self.editLVMVolumeGroup(vgrequest)
            elif type == "NEW":
		self.editPartitionRequest(request, isNew = 1)
            else:
                self.editPartitionRequest(request)

    # isNew implies that this request has never been successfully used before
    def editRaidRequest(self, raidrequest, isNew = 0):
	raideditor = raid_dialog_gui.RaidEditor(self.partitions,
						     self.diskset, self.intf,
						     self.parent, raidrequest,
						     isNew)
	
	while 1:
	    request = raideditor.run()

	    if request is None:
		return

	    if not isNew:
		self.partitions.removeRequest(raidrequest)

	    self.partitions.addRequest(request)

	    if self.refresh():
		# how can this fail?  well, if it does, do the remove new,
		# add old back in dance
		self.partitions.removeRequest(request)
		if not isNew:
		    self.partitions.addRequest(raidrequest)
		if self.refresh():
		    raise RuntimeError, ("Returning partitions to state "
					 "prior to RAID edit failed")
	    else:
		break

	raideditor.destroy()		


    def editPartitionRequest(self, origrequest, isNew = 0):
	parteditor = partition_dialog_gui.PartitionEditor(self.partitions,
							  self.diskset,
							  self.intf,
							  self.parent,
							  origrequest,
							  isNew)

	while 1:
	    request = parteditor.run()
	    print request

	    if request is None:
		return

            if not isNew:
                self.partitions.removeRequest(origrequest)

            self.partitions.addRequest(request)
            if self.refresh():
                # the add failed; remove what we just added and put
                # back what was there if we removed it
		print "failed"
                self.partitions.removeRequest(request)
                if not isNew:
                    self.partitions.addRequest(origrequest)
                if self.refresh():
                    # this worked before and doesn't now...
                    raise RuntimeError, ("Returning partitions to state "
                                         "prior to edit failed")
            else:
		break

	parteditor.destroy()


    def editLVMVolumeGroup(self, origvgrequest, isNew = 0):
	vgeditor = lvm_dialog_gui.VolumeGroupEditor(self.partitions,
						    self.diskset,
						    self.intf, self.parent,
						    origvgrequest, isNew)
	
	origpartitions = self.partitions.copy()
	origvolreqs = origpartitions.getLVMLVForVG(origvgrequest)

	while (1):
	    rc = vgeditor.run()

	    #
	    # return code is either None or a tuple containing
	    # volume group request and logical volume requests
	    #
	    if rc is None:
		return

	    (vgrequest, logvolreqs) = rc

	    # first add the volume group
	    if not isNew:
                # if an lv was preexisting and isn't in the new lv requests,
                # we need to add a delete for it.  
                for lv in origvolreqs:
                    if not lv.getPreExisting():
                        continue
                    found = 0
                    for newlv in logvolreqs:
                        if (newlv.getPreExisting() and
                            newlv.logicalVolumeName == lv.logicalVolumeName):
                            found = 1
                            break
                    if found == 0:
                        delete = partRequests.DeleteLogicalVolumeSpec(lv.logicalVolumeName,
                                                                      origvgrequest.volumeGroupName)
                        self.partitions.addDelete(delete)
                        
		for lv in origvolreqs:
		    self.partitions.removeRequest(lv)

		self.partitions.removeRequest(origvgrequest)

	    vgID = self.partitions.addRequest(vgrequest)

	    # now add the logical volumes
	    for lv in logvolreqs:
		lv.volumeGroup = vgID
                if not lv.getPreExisting():
                    lv.format = 1
		self.partitions.addRequest(lv)

	    if self.refresh():
		if not isNew:
		    self.partitions = origpartitions.copy()
		    if self.refresh():
			raise RuntimeError, ("Returning partitions to state "
					     "prior to edit failed")
		continue
	    else:
		break

	vgeditor.destroy()



    def makeLvmCB(self, widget):
        request = VolumeGroupRequestSpec()
        self.editLVMVolumeGroup(request, isNew = 1)

	return

    def makeraidCB(self, widget):
        request = RaidRequestSpec(fileSystemTypeGetDefault())
        self.editRaidRequest(request, isNew = 1)

    def getScreen(self, fsset, diskset, partitions, intf):
        self.fsset = fsset
        self.diskset = diskset
        self.intf = intf
        
        self.diskset.openDevices()
        self.partitions = partitions

        checkForSwapNoMatch(self.intf, self.diskset, self.partitions)

        # XXX PartitionRequests() should already exist and
        # if upgrade or going back, have info filled in
#        self.newFsset = self.fsset.copy()

        # operational buttons
        buttonBox = gtk.HButtonBox()
        buttonBox.set_layout(gtk.BUTTONBOX_SPREAD)

        if iutil.getArch() == "s390":
            ops = ((_("_Edit"), self.editCb),
                   (_("_Delete"), self.deleteCb),
                   (_("_Reset"), self.resetCb),
                   (_("Make _RAID"), self.makeraidCB),)
        else:
            ops = ((_("_New"), self.newCB),
                   (_("_Edit"), self.editCb),
                   (_("_Delete"), self.deleteCb),
                   (_("Re_set"), self.resetCb),
                   (_("_RAID"), self.makeraidCB),
                   (_("_LVM"), self.makeLvmCB))
        
        for label, cb in ops:
            button = gtk.Button(label)
            buttonBox.add (button)
            button.connect ("clicked", cb)

        self.tree = DiskTreeModel()
        self.treeView = self.tree.getTreeView()
        self.treeView.connect('row-activated', self.treeActivateCb)
        self.treeViewSelection = self.treeView.get_selection()
        self.treeViewSelection.connect("changed", self.treeSelectCb)

        # set up the canvas
        self.diskStripeGraph = DiskStripeGraph(self.tree, self.editCb)
        
        # do the initial population of the tree and the graph
        self.populate(initial = 1)

	vpaned = gtk.VPaned()

        sw = gtk.ScrolledWindow()
        sw.add(self.diskStripeGraph.getCanvas())
        sw.set_policy(gtk.POLICY_AUTOMATIC, gtk.POLICY_AUTOMATIC)
	sw.set_shadow_type(gtk.SHADOW_IN)

        frame = gtk.Frame()
        frame.add(sw)
	vpaned.add1(frame)

        box = gtk.VBox(gtk.FALSE, 5)
        box.pack_start(buttonBox, gtk.FALSE)
        sw = gtk.ScrolledWindow()
        sw.add(self.treeView)
        sw.set_policy(gtk.POLICY_AUTOMATIC, gtk.POLICY_AUTOMATIC)
	sw.set_shadow_type(gtk.SHADOW_IN)
	
        box.pack_start(sw, gtk.TRUE)
	vpaned.add2(box)

	# XXX should probably be set according to height 
	vpaned.set_position(170)

	return vpaned

class AutoPartitionWindow(InstallWindow):
    def __init__(self, ics):
    	InstallWindow.__init__(self, ics)
        ics.setTitle(_("Automatic Partitioning"))
        ics.setNextEnabled(gtk.TRUE)
        ics.readHTML("autopart")
        self.parent = ics.getICW().window

    def getNext(self):
        if self.clearLinuxRB.get_active():
            self.partitions.autoClearPartType = autopart.CLEARPART_TYPE_LINUX
        elif self.clearAllRB.get_active():
            self.partitions.autoClearPartType = autopart.CLEARPART_TYPE_ALL
        else:
            self.partitions.autoClearPartType = autopart.CLEARPART_TYPE_NONE

        allowdrives = []
	model = self.drivelist.get_model()
	iter = model.get_iter_first()
	next = 1
	while next:
	    val   = model.get_value(iter, 0)
	    drive = model.get_value(iter, 1)

	    if val:
		allowdrives.append(drive)

	    next = model.iter_next(iter)

        if len(allowdrives) < 1:
            dlg = gtk.MessageDialog(self.parent, 0, gtk.MESSAGE_ERROR,
                                    gtk.BUTTONS_OK,
                                    _("You need to select at least one "
                                      "drive to have %s installed "
                                      "onto.") % (productName,))
            gui.addFrame(dlg)
            dlg.show_all()
            rc = dlg.run()
            dlg.destroy()
            raise gui.StayOnScreen

        self.partitions.autoClearPartDrives = allowdrives

        if not autopart.queryAutoPartitionOK(self.intf, self.diskset,
                                             self.partitions):
            raise gui.StayOnScreen
        
        if self.inspect.get_active():
            self.dispatch.skipStep("partition", skip = 0)
        else:
            self.dispatch.skipStep("partition")

        return None


    def getScreen(self, diskset, partitions, intf, dispatch):
        
        self.diskset = diskset
        self.partitions = partitions
        self.intf = intf
        self.dispatch = dispatch
        
        type = partitions.autoClearPartType
        cleardrives = partitions.autoClearPartDrives
        
        box = gtk.VBox(gtk.FALSE)
        box.set_border_width(5)

        label = gui.WrappingLabel(_(autopart.AUTOPART_DISK_CHOICE_DESCR_TEXT))
        label.set_alignment(0.0, 0.0)
        box.pack_start(label, gtk.FALSE, gtk.FALSE)

        # what partition types to remove
        clearbox = gtk.VBox(gtk.FALSE)
        label = gui.WrappingLabel(_("I want to have automatic partitioning:"))
        label.set_alignment(0.0, 0.0)
        clearbox.pack_start(label, gtk.FALSE, gtk.FALSE, 10)
        
        radioBox = gtk.VBox(gtk.FALSE)
        self.clearLinuxRB = gtk.RadioButton(
            None, _(autopart.CLEARPART_TYPE_LINUX_DESCR_TEXT))
	radioBox.pack_start(self.clearLinuxRB, gtk.FALSE, gtk.FALSE)
        self.clearAllRB = gtk.RadioButton(
            self.clearLinuxRB, _(autopart.CLEARPART_TYPE_ALL_DESCR_TEXT))
	radioBox.pack_start(self.clearAllRB, gtk.FALSE, gtk.FALSE)
        self.clearNoneRB = gtk.RadioButton(
            self.clearLinuxRB, _(autopart.CLEARPART_TYPE_NONE_DESCR_TEXT))
	radioBox.pack_start(self.clearNoneRB, gtk.FALSE, gtk.FALSE)

        if type == autopart.CLEARPART_TYPE_LINUX:
            self.clearLinuxRB.set_active(1)
        elif type == autopart.CLEARPART_TYPE_ALL:
            self.clearAllRB.set_active(1)
        else:
            self.clearNoneRB.set_active(1)
           
	align = gtk.Alignment()
	align.add(radioBox)
	align.set(0.5, 0.5, 0.0, 0.0)
	clearbox.pack_start(align, gtk.FALSE, gtk.FALSE)

        box.pack_start(clearbox, gtk.FALSE, gtk.FALSE, 10)

        # which drives to use?
        drivesbox = gtk.VBox(gtk.FALSE)
        label = gui.WrappingLabel(_("Which drive(s) do you want to use for "
                                    "this installation?"))
        label.set_alignment(0.0, 0.0)
        drivesbox.pack_start(label, gtk.FALSE, gtk.FALSE, 10)
        self.drivelist = createAllowedDrivesList(diskset.disks, cleardrives)

        # XXX bad use of usize
        self.drivelist.set_size_request(375, 80)

        sw = gtk.ScrolledWindow()
        sw.add(self.drivelist)
        sw.set_policy(gtk.POLICY_NEVER, gtk.POLICY_AUTOMATIC)
	sw.set_shadow_type(gtk.SHADOW_IN)
        
	align = gtk.Alignment()
	align.add(sw)
	align.set(0.5, 0.5, 0.0, 0.0)
        
        drivesbox.pack_start(align, gtk.FALSE, gtk.FALSE)

        box.pack_start(drivesbox, gtk.FALSE, gtk.FALSE)

        self.inspect = gtk.CheckButton()
        gui.widgetExpander(self.inspect)
        label = gtk.Label(_("Review (allows you to see and change the "
                            "automatic partitioning results)"))
        label.set_line_wrap(gtk.TRUE)
        gui.widgetExpander(label, self.inspect)
        label.set_alignment(0.0, 1.0)
        self.inspect.add(label)

        self.inspect.set_active(not dispatch.stepInSkipList("partition"))

	box.pack_start(self.inspect, gtk.TRUE, gtk.TRUE, 10)

        self.ics.setNextEnabled(gtk.TRUE)

	align = gtk.Alignment()
	align.add(box)
	align.set(0.5, 0.5, 0.0, 0.0)

	return align
        
