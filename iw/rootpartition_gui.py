from gtk import *
from iw_gui import *
from thread import *
import isys
from translate import _
import gui
from fdisk_gui import *
import isys
import iutil
from log import log

CHOICE_FDISK = 1
CHOICE_DDRUID = 2
CHOICE_AUTOPART = 3

# 12-12-2000 msf - this is no longer used - leaving just in case I'm wrong
#class ConfirmPartitionWindow (InstallWindow):
#    def __init__ (self, ics):
#	InstallWindow.__init__ (self, ics)
#
#        self.todo = ics.getToDo ()
#        ics.setTitle (_("Confirm Partitioning Selection"))
#        ics.readHTML ("partition")
#	ics.setNextEnabled (TRUE)
#
#    # ConfirmPartitionWindow tag="partition"
#    def getScreen (self):
#        return self.window
#
#    def getPrev (self):
#        return PartitionWindow

class PartitionWindow (InstallWindow):
    swapon = 0
    def __init__ (self, ics):
	InstallWindow.__init__ (self, ics)

        self.todo = ics.getToDo ()
        ics.setTitle (_("Disk Druid"))
        ics.readHTML ("partition")
	ics.setNextEnabled (FALSE)
	self.skippedScreen = 0
        self.swapon = 0

    def checkSwap (self):
        if PartitionWindow.swapon or (iutil.memInstalled() > 
					isys.EARLY_SWAP_RAM):
	    return 1

        threads_leave ()
	message = gui.MessageWindow(_("Low Memory"),
		   _("As you don't have much memory in this machine, we "
		     "need to turn on swap space immediately. To do this "
		     "we'll have to write your new partition table to the "
		     "disk immediately. Is that okay?"), "okcancel")

	if (message.getrc () == 1):
	    threads_enter ()
	    return 0

	self.todo.fstab.savePartitions()
	self.todo.fstab.turnOnSwap(self.todo.instPath)
	self.todo.ddruidAlreadySaved = 1
	PartitionWindow.swapon = 1

        threads_enter ()

        return 1


    def lba32Check (self):
        # check if boot partition is above 1024 cyl limit
        if iutil.getArch() != "i386":
            return 0

        maxcyl = self.todo.fstab.getBootPartitionMaxCylFromDesired()
        log("Maximum cylinder is %s" % maxcyl)

        if maxcyl > 1024:
            if not self.todo.fstab.edd:
                rc = self.todo.intf.messageWindow(_("Warning"), 
                    _("You have put the partition containing the kernel (the "
                      "boot partition) above the 1024 cylinder limit, and "
                      "it appears that this systems BIOS does not support "
                      "booting from above this limit. Proceeding will "
                      "most likely make the system unable to reboot into "
                      "Linux.\n\n"
                      "If you choose to proceed, it is HIGHLY recommended "
                      "you make a boot floppy when asked. This will "
                      "guarantee you have a way to boot into the system "
                      "after installation.\n\n"
                      "Press OK to proceed, or Cancel to go back and "
                      "reassign the boot partition."),
                                   type = "okcancel").getrc()
                return rc
            else:
                self.todo.intf.messageWindow(_("Warning"), 
                    _("You have put the partition containing the kernel (the "
                      "boot partition) above the 1024 cylinder limit. "
                      "It appears that this systems BIOS supports "
                      "booting from above this limit. \n\n"
                      "It is HIGHLY recommended you make a boot floppy when "
                      "asked by the installer, as this is a new feature in "
                      "recent motherboards and is not always reliable. "
                      "Making a boot disk will guarantee you can boot "
                      "your system once installed."),
                                   type = "ok")
                return 0

        return 0

    def getNext (self):
	if not self.running: return 0
	self.todo.fstab.runDruidFinished()

        # FIXME
	#if not self.skippedScreen:
	    #win = self.todo.ddruid.getConfirm ()
	    #if win:
		#bin = GtkFrame (None, _obj = win)
		#bin.set_shadow_type (SHADOW_NONE)
		#window = ConfirmPartitionWindow
		#window.window = bin
		#return window

	bootPartition = None
	rootPartition = None

        threads_leave()
        rc = self.lba32Check ()
        threads_enter()
        if rc:
            raise gui.StayOnScreen

        if not self.checkSwap ():
            return PartitionWindow

        if self.todo.fstab.rootOnLoop():
            return LoopSizeWindow

        return None

    def enableCallback (self, value):
        self.ics.setNextEnabled (value)

    # PartitionWindow tag="partition"
    def getScreen (self):
	self.running = 0
	if not self.todo.fstab.getRunDruid(): return None
	self.running = 1
	return self.todo.fstab.runDruid(self.enableCallback)

class LoopSizeWindow(InstallWindow):
    def __init__ (self, ics):
	InstallWindow.__init__ (self, ics)
        ics.readHTML ("loopback")

    def getNext (self):
        fsSize = int(self.sizeAdj.value)
        swapSize = int(self.swapAdj.value)
        self.todo.fstab.setLoopbackSize (fsSize, swapSize)

    def Spinchanged(self, *args):
        swapsize = self.swapAdj.value
        rootsize = self.sizeAdj.value
        totalmax = self.upper
        
        if swapsize + rootsize > totalmax:
            rootsize = totalmax - swapsize
            
        if rootsize < 0:
            rootsize = self.sizeAdj.lower
                
        # set adjustment, trigger clipping and read new value
        self.sizeAdj.set_value(rootsize)
        rootsize = self.sizeAdj.value

        # if still too big try smallest swap and loopback size possible
        if rootsize + swapsize > totalmax:
            swapsize = self.swapAdj.lower
                    
        self.swapAdj.set_value(swapsize)

    # LoopSizeWindow tag="loopback"
    def getScreen (self):
        # XXX error check mount that this check tries
        if self.todo.setupFilesystems:
            rootdev = self.todo.fstab.getRootDevice()
            avail = apply(isys.spaceAvailable, rootdev)
            
            # add in size of loopback files if they exist
            extra = 0
            try:
                import os, stat
                isys.mount(rootdev[0], "/mnt/space", fstype = rootdev[1])
                extra = extra + os.stat("/mnt/space/redhat.img")[stat.ST_SIZE]
                extra = extra + os.stat("/mnt/space/rh-swap.img")[stat.ST_SIZE]
                isys.umount("/mnt/space")
            except:
                pass

            # do this separate since we dont know when above failed
            try:
                isys.umount("/mnt/space")
            except:
                pass

            extra = extra / 1024 / 1024
            avail = avail + extra
        else:
            # test mode
            avail = 5000
	(size, swapSize) = self.todo.fstab.getLoopbackSize()
	if not size:
	    size = avail / 2
	    swapSize = 128

        vbox = GtkVBox (FALSE, 5)
        
        label = GtkLabel (
		_("You've chosen to put your root filesystem in a file on "
		  "an already-existing DOS or Windows filesystem. How large, "
		  "in megabytes, should would you like the root filesystem "
		  "to be, and how much swap space would you like? They must "
		  "total less then %d megabytes in size.") % (avail, ))
        label.set_usize (400, -1)
        label.set_line_wrap (TRUE)
        vbox.pack_start (label, FALSE, FALSE)

	self.upper = avail
	if avail > 2000:
	    self.upper = 2000
            size = 2000


        # XXX lower is 150
        self.sizeAdj = GtkAdjustment (value = size, lower = 150, upper = self.upper, step_incr = 1)
        self.sizeSpin = GtkSpinButton (self.sizeAdj, digits = 0)
        self.sizeSpin.set_usize (100, -1)
        self.sizeSpin.set_numeric(1)
        self.sizeSpin.connect("changed", self.Spinchanged)
        
        self.swapAdj = GtkAdjustment (value = swapSize, lower = 16, upper = self.upper, step_incr = 1)
        self.swapSpin = GtkSpinButton (self.swapAdj, digits = 0)
        self.swapSpin.set_usize (100, -1)
        self.swapSpin.set_numeric(1)
        self.swapSpin.connect("changed", self.Spinchanged)
        
        table = GtkTable ()

        label = GtkLabel (_("Root filesystem size:"))
        label.set_alignment (1.0, 0.5)
        table.attach (label, 0, 1, 0, 1, xpadding=5, ypadding=5)
        table.attach (self.sizeSpin, 1, 2, 0, 1, xpadding=5, ypadding=5)

        label = GtkLabel (_("Swap space size:"))
        label.set_alignment (1.0, 0.5)
        table.attach (label, 0, 1, 1, 2, xpadding=5, ypadding=5)
        table.attach (self.swapSpin, 1, 2, 1, 2, xpadding=5, ypadding=5)

        align = GtkAlignment ()
        align.add (table)
        align.set (0, 0, 0.5, 0.5)
        vbox.pack_start (align, FALSE, FALSE)

	self.ics.setNextEnabled (TRUE)

        return vbox
        
class AutoPartitionWindow(InstallWindow):
    def getPrev(self):
	self.druid = None

        # probably wrong, but necessary so we don't remember previous
        # editting user did in disk druid screen
        self.todo.fstab.rescanPartitions(clearFstabCache=1)
	self.beingDisplayed = 0

    def getNext(self):
	if not self.beingDisplayed: return
###
###    msf - 05/11/2000 - removed this block shouldnt be needed with changes
###
#
#	if not self.__dict__.has_key("manuallyPartitionddruid"):
#            # if druid wasn't running, must have been in autopartition mode
#            # clear fstab cache so we don't get junk from attempted
#            # autopartitioning
#            print "number 1"
#            clearcache = not self.todo.fstab.getRunDruid()
#	    self.todo.fstab.setRunDruid(1)
#            self.todo.fstab.setReadonly(0)
#            #print "Rescanning partitions 1 - ", clearcache
#            self.todo.fstab.rescanPartitions(clearcache)
#	    self.todo.instClass.removeFromSkipList("format")
	if AutoPartitionWindow.manuallyPartitionddruid.get_active():
            if self.druid:
                del self.druid
            # see comment above about clearing cache

            if self.lastChoice != CHOICE_DDRUID:
                clearcache = 1
            else:
                clearcache = 0
#            clearcache = not self.todo.fstab.getRunDruid()
	    self.todo.fstab.setRunDruid(1)
            self.todo.fstab.setReadonly(0)
            #print "Rescanning partitions 2 - ", clearcache
	    self.todo.fstab.rescanPartitions(clearcache)
	    self.todo.instClass.removeFromSkipList("format")
            self.lastChoice = CHOICE_DDRUID
        elif AutoPartitionWindow.manuallyPartitionfdisk.get_active():
            self.todo.fstab.setRunDruid(1)
            self.todo.fstab.setReadonly(1)
            if self.druid:
                del self.druid
                
            self.lastChoice = CHOICE_FDISK
	else:
	    self.todo.fstab.setRunDruid(0)
	    self.todo.fstab.setDruid(self.druid, self.todo.instClass.raidList)
	    self.todo.fstab.formatAllFilesystems()
	    self.todo.instClass.addToSkipList("format")
            self.lastChoice = CHOICE_AUTOPART

	self.beingDisplayed = 0
	return None

    def __init__(self, ics):
	InstallWindow.__init__(self, ics)
        ics.setTitle (_("Automatic Partitioning"))
	self.druid = None
	self.beingDisplayed = 0
        self.lastChoice = None

    # AutoPartitionWindow tag="wkst" or "svr", in installclass.py:setClearParts
    def getScreen (self):   

        # XXX hack
        if self.todo.instClass.clearType:
            self.ics.readHTML (self.todo.instClass.clearType)
	else:
	    self.ics.readHTML (None)
	todo = self.todo
	self.druid = None

# user selected an install type which had predefined partitioning
# attempt to automatically allocate these partitions.
#
# if this fails we drop them into disk druid
#
        attemptedPartitioningandFailed = 0
	if self.todo.instClass.partitions:
	    self.druid = \
                       todo.fstab.attemptPartitioning(todo.instClass.partitions,
                                                      todo.instClass.fstab,
                                                      todo.instClass.clearParts)

            if not self.druid:
                attemptedPartitioningandFailed = 1

#
# if no warning text means we have carte blanc to blow everything away
# without telling user
#
	if not todo.getPartitionWarningText() and self.druid:

            self.ics.setNextEnabled (TRUE)

	    self.todo.fstab.setRunDruid(0)
	    self.todo.fstab.setDruid(self.druid)
	    self.todo.fstab.formatAllFilesystems()
	    self.todo.instClass.addToSkipList("format")
	    return

#
# see what means the user wants to use to partition
#
        self.todo.fstab.setRunDruid(1)
        self.todo.fstab.setReadonly(0)

        if self.druid:
            self.ics.setTitle (_("Disk Partitioning"))
            label = \
                  GtkLabel(_("Please select the type of disk partitioning you would like to use."
                             "\n\n%s"
                             "\n\nSelecting manual partitioning allows you to create the partitions by hand.") % 
                           (_(todo.getPartitionWarningText()), ))
        else:
            if attemptedPartitioningandFailed:
                self.ics.setTitle (_("Automatic Partitioning Failed"))
                label = GtkLabel(_("\nThere is not sufficient disk space in "
                                   "order to automatically partition your disk. "
                                   "You will need to manually partition your "
                                   "disks for Red Hat Linux to install."
                                   "\n\nPlease choose the tool you would like to "
                                   "use to partition your system for Red Hat Linux."))
            else:
                self.ics.setTitle (_("Manual Partitioning"))
                label = GtkLabel(_("\nPlease choose the tool you would like to "
                                   "use to partition your system for Red Hat Linux."))
            
        label.set_line_wrap(TRUE)
        label.set_alignment(0.0, 0.0)
        label.set_usize(380, -1)
            
        box = GtkVBox (FALSE)
	box.pack_start(label, FALSE)
        box.set_border_width (5)

        radioBox = GtkVBox (FALSE)

        if self.druid:
            self.continueChoice = GtkRadioButton (None, _("Automatically partition and REMOVE DATA"))
            radioBox.pack_start(self.continueChoice, FALSE)
            firstbutton = self.continueChoice
        else:
            firstbutton = None
        
	AutoPartitionWindow.manuallyPartitionddruid = GtkRadioButton(
		firstbutton, _("Manually partition with Disk Druid"))

        if self.lastChoice == CHOICE_DDRUID:
            AutoPartitionWindow.manuallyPartitionddruid.set_active(1)

        if firstbutton == None:
            secondbutton = AutoPartitionWindow.manuallyPartitionddruid
        else:
            secondbutton = firstbutton
            
	radioBox.pack_start(AutoPartitionWindow.manuallyPartitionddruid, FALSE)
	AutoPartitionWindow.manuallyPartitionfdisk = GtkRadioButton(
		secondbutton, _("Manually partition with fdisk [experts only]"))
	radioBox.pack_start(AutoPartitionWindow.manuallyPartitionfdisk, FALSE)

        if self.lastChoice == CHOICE_FDISK:
            AutoPartitionWindow.manuallyPartitionfdisk.set_active(1)
            
	align = GtkAlignment()
	align.add(radioBox)
	align.set(0.5, 0.5, 0.0, 0.0)

	box.pack_start(align, TRUE, TRUE)
	box.set_border_width (5)

        self.ics.setNextEnabled (TRUE)

	self.beingDisplayed = 1
	return box



class LBA32WarningWindow(InstallWindow):
    def __init__ (self, ics):
        InstallWindow.__init__ (self, ics)
        ics.setTitle (_("Boot Partition Location Warning"))
        ics.readHTML ("lba32warning")
        self.showing = 0

    def proceedChanged(self, widget, *args):
        if self.proceed.get_active():
            self.ics.setNextEnabled (TRUE)
        else:
            self.ics.setNextEnabled (FALSE)

    def getScreen (self):

        # check if boot partition is above 1024 cyl limit
        if iutil.getArch() != "i386":
            return None

        maxcyl = self.todo.fstab.getBootPartitionMaxCylFromDesired()
        log("Maximum cylinder is %s" % maxcyl)
        
        if maxcyl > 1024:
            vbox = GtkVBox (FALSE, 5)

            if not self.todo.fstab.edd:
                label = GtkLabel (
                    _("You have put the partition containing the kernel (the "
                      "boot partition) above the 1024 cylinder limit, and "
                      "it appears that this systems BIOS does not support "
                      "booting from above this limit. Proceeding will "
                      "most likely make the system unable to reboot into "
                      "Linux.\n\n"
                      "If you choose to proceed, it is HIGHLY recommended "
                      "you make a boot floppy when asked. This will "
                      "guarantee you have a way to boot into the system "
                      "after installation.\n\n"
                      "Are you sure you want to proceed?"))
            else:
                label = GtkLabel (
                    _("You have put the partition containing the kernel (the "
                      "boot partition) above the 1024 cylinder limit. "
                      "It appears that this systems BIOS supports "
                      "booting from above this limit. \n\n"
                      "It is HIGHLY recommended you make a boot floppy when "
                      "asked by the installer, as this is a new feature in "
                      "recent motherboards and is not always reliable. "
                      "Making a boot disk will guarantee you can boot "
                      "your system once installed."))

            label.set_usize (400, -1)
            label.set_line_wrap (TRUE)
            label.set_alignment(0.0, 0.0)
            vbox.pack_start (label, FALSE, FALSE)

            if not self.todo.fstab.edd:
                vbox2 = GtkVBox (FALSE, 5)
                
                self.proceed = GtkRadioButton (None, _("Yes"))
                self.proceed.connect("toggled", self.proceedChanged)
                self.dontproceed = GtkRadioButton (self.proceed, _("No"))
                self.dontproceed.set_active(1)
                self.dontproceed.connect("toggled", self.proceedChanged)
                
                vbox2.pack_start (self.proceed, FALSE)
                vbox2.pack_start (self.dontproceed, FALSE)
                vbox2.set_border_width (25)
                
                vbox.pack_start (vbox2, TRUE)

                self.ics.setNextEnabled (FALSE)
            else:
                self.ics.setNextEnabled (TRUE)
                
            vbox.set_border_width (5)
            return vbox
        else:
            return None




