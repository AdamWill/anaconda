#
# isys.py - installer utilitiy functions and glue for C module
#
# Matt Wilson <msw@redhat.com>
# Erik Troan <ewt@redhat.com>
#
# Copyright 2001 Red Hat, Inc.
#
# This software may be freely redistributed under the terms of the GNU
# library public license.
#
# You should have received a copy of the GNU Library Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
#

import _isys
import string
import os
import os.path

from rhpl.log import log

mountCount = {}
raidCount = {}

MIN_RAM = _isys.MIN_RAM
MIN_GUI_RAM = _isys.MIN_GUI_RAM
EARLY_SWAP_RAM = _isys.EARLY_SWAP_RAM

def pathSpaceAvailable(path, fsystem = "ext2"):
    return _isys.devSpaceFree(path)

def spaceAvailable(device, fsystem = "ext2"):
    mount(device, "/mnt/space", fstype = fsystem)
    space = _isys.devSpaceFree("/mnt/space/.")
    umount("/mnt/space")
    return space

def fsSpaceAvailable(fsystem):
    return _isys.devSpaceFree(fsystem)

def raidstop(mdDevice):
    if raidCount.has_key (mdDevice):
        if raidCount[mdDevice] > 1:
            raidCount[mdDevice] = raidCount[mdDevice] - 1
            return
        del raidCount[mdDevice]

    makeDevInode(mdDevice, "/tmp/md")
    fd = os.open("/tmp/md", os.O_RDONLY)
    os.remove("/tmp/md")
    try:
        _isys.raidstop(fd)
    except:
        pass
    os.close(fd)

def raidstart(mdDevice, aMember):
    if raidCount.has_key(mdDevice) and raidCount[mdDevice]:
	raidCount[mdDevice] = raidCount[mdDevice] + 1
	return

    raidCount[mdDevice] = 1

    makeDevInode(mdDevice, "/tmp/md")
    makeDevInode(aMember, "/tmp/member")
    fd = os.open("/tmp/md", os.O_RDONLY)
    os.remove("/tmp/md")
    try:
        _isys.raidstart(fd, "/tmp/member")
    except:
        pass
    os.close(fd)
    os.remove("/tmp/member")

def raidsb(mdDevice):
    makeDevInode(mdDevice, "/tmp/md")
    return raidsbFromDevice("/tmp/md")

def raidsbFromDevice(device):
    fd = os.open(device, os.O_RDONLY)
    rc = 0
    try:
        rc = _isys.getraidsb(fd)
    finally:
        os.close(fd)
    return rc

def losetup(device, file, readOnly = 0):
    if readOnly:
	mode = os.O_RDONLY
    else:
	mode = os.O_RDWR
    targ = os.open(file, mode)
    loop = os.open(device, mode)
    try:
        _isys.losetup(loop, targ, file)
    finally:
        os.close(loop)
        os.close(targ)

def lochangefd(device, file):
    loop = os.open(device, os.O_RDONLY)
    targ = os.open(file, os.O_RDONLY)
    try:
        _isys.lochangefd(loop, targ)
    finally:
        os.close(loop)
        os.close(targ)

def unlosetup(device):
    loop = os.open(device, os.O_RDONLY)
    try:
        _isys.unlosetup(loop)
    finally:
        os.close(loop)

def ddfile(file, megs, pw = None):
    fd = os.open("/dev/zero", os.O_RDONLY);
    buf = os.read(fd, 1024 * 256)
    os.close(fd)

    fd = os.open(file, os.O_RDWR | os.O_CREAT)

    total = megs * 4	    # we write out 1/4 of a meg each time through

    if pw:
	(fn, title, text) = pw
	win = fn(title, text, total - 1)

    for n in range(total):
	os.write(fd, buf)
	if pw:
	    win.set(n)

    if pw:
	win.pop()

    os.close(fd)

def mount(device, location, fstype = "ext2", readOnly = 0, bindMount = 0):
    location = os.path.normpath(location)

    #
    # Apparently we don't need to create to create device nodes for
    # a device that starts with a '/' (like '/usbdevfs').
    # We note whether or not we created a node so we can cleanup later.
    #
    createdNode = 0
    if device and device != "none" and device[0] != "/":
	devName = "/tmp/%s" % device
	makeDevInode(device, devName)
	device = devName
	createdNode = 1

    if mountCount.has_key(location) and mountCount[location] > 0:
	mountCount[location] = mountCount[location] + 1
	return

    log("going to mount %s on %s" %(device, location))
    rc = _isys.mount(fstype, device, location, readOnly, bindMount)

    if not rc:
	mountCount[location] = 1

    # did we create a node, if so remove
    if createdNode:
	os.unlink(device)

    return rc

def umount(what, removeDir = 1):
    what = os.path.normpath(what)

    if not os.path.isdir(what):
	raise ValueError, "isys.umount() can only umount by mount point"

    if mountCount.has_key(what) and mountCount[what] > 1:
	mountCount[what] = mountCount[what] - 1
	return

    rc = _isys.umount(what)

    if removeDir and os.path.isdir(what):
	os.rmdir(what)

    if not rc and mountCount.has_key(what):
	del mountCount[what]

    return rc

def smpAvailable():
    return _isys.smpavailable()

htavailable = _isys.htavailable

def summitavailable():
    try:
        f = open("/proc/cmdline")
        line = f.readline()
        if string.find(line, " summit") != -1:
            return 1
        del f
    except:
        pass
    
    return _isys.summitavailable()

def chroot (path):
    return os.chroot (path)

def checkBoot (path):
    return _isys.checkBoot (path)

def swapoff (path):
    return _isys.swapoff (path)

def swapon (path):
    return _isys.swapon (path)

def fbconProbe(path):
    return _isys.fbconprobe (path)

def loadFont():
    return _isys.loadFont ()

def loadKeymap(keymap):
    return _isys.loadKeymap (keymap)

probedList = None

def flushDriveDict():
    global probedList
    probedList = None

def driveDict(klassArg):
    global probedList
    if not probedList:
        probedList = _isys.ProbedList()
        probedList.updateIde()
        probedList.updateScsi()
        probedList.updateDasd()

    dict = {}
    for (klass, dev, descr) in probedList:
	if (klass == klassArg):
	    dict[dev] = descr
    return dict

def isEMDLoaded():
    f = open("/proc/modules")
    lines = f.readlines()
    f.close()
    for l in lines:
        if l.startswith("emd"):
            return 1
    return 0

def hardDriveDict():
    import parted

    dict = driveDict("disk")

    # this is kind of ugly, but it's much easier to do this from python
    for (dev, descr) in dict.items():
        # emd ddf/adaptec raid stuff
        if isEMDLoaded():
            try:
                if _isys.inEMDDevice(dev):
                    log("%s is in EMD device, ignoring" %(dev,))
                    del dict[dev]
                    continue
            except Exception, e:
                log("error checking if %s is emd: %s" %(dev, e))
                pass
        
        # the only raid devs like this are ide, so only worry about them
        if not dev.startswith("hd"):
            continue
        ret = _isys.hasIdeRaidMagic(dev)
        if ret is None:
            continue
        found = 0
        try:
            devName = "/tmp/%s" % dev
            makeDevInode(dev, devName)

            # ugh, this is basically copy&paste of other anaconda code, but
            # it kind of needs to be here and isys should stay isolated
            peddev = parted.PedDevice.get(devName)
            disk = parted.PedDisk.new(peddev)
            part = disk.next_partition()
            while part:
                if (part.fs_type and
                    part.fs_type.name in ("FAT", "fat16", "fat32",
                                          "ntfs", "hpfs")):
                    # this disk has a fat partition on it, we have to use
                    # it as an ataraid device
                    found = 1
                part = disk.next_partition(part)
            del disk
            del peddev

            os.unlink(devName)
        except Exception, e:
            print e
            # what can I really do here?
            pass

        if found == 1:
            log("%s has a %s raid signature and windows parts" %(dev, ret))
            del dict[dev]
        else:
            log("%s has a %s raid signature but no windows parts" %(dev, ret))
        
    return dict

def floppyDriveDict():
    return driveDict("floppy")

def cdromList():
    list = driveDict("cdrom").keys()
    list.sort()
    return list

def getDasdPorts():
    return _isys.getDasdPorts()

def isUsableDasd(device):
    return _isys.isUsableDasd(device)

def isLdlDasd(device):
    return _isys.isLdlDasd(device)

# read /proc/dasd/devices and get a mapping between devs and the dasdnum
def getDasdDevPort():
    ret = {}
    f = open("/proc/dasd/devices", "r")
    lines = f.readlines()
    f.close()

    for line in lines:
        index = line.index("(")
        dasdnum = line[:index]
        
        start = line[index:].find("dasd")
        end = line[index + start:].find(":")
        dev = line[index + start:end + start + index].strip()
        
        ret[dev] = dasdnum

    return ret

# get active/ready state of a dasd device
# returns 0 if we're fine, 1 if not
def getDasdState(dev):
    devs = getDasdDevPort()
    if not devs.has_key(dev):
        log("don't have %s in /dev/dasd/devices!" %(dev,))
        return 0

    f = open("/proc/dasd/devices", "r")
    lines = f.readlines()
    f.close()

    for line in lines:
        if not line.startswith(devs[dev]):
            continue
        if line.find(" ready") != -1:
            return 1

    return 0
    

def makeDevInode(name, fn=None):
    if fn:
        _isys.mkdevinode(name, fn)
        return fn
    path = '/dev/%s' % (name,)
    try:
        os.stat(path)
    except OSError:
        path = '/tmp/%s' % (name,)
        _isys.mkdevinode(name, path)
    return path

def makedev(major, minor):
    return _isys.makedev(major, minor)

def mknod(pathname, mode, dev):
    return _isys.mknod(pathname, mode, dev)

def inet_ntoa (addr):
    return "%d.%d.%d.%d" % ((addr >> 24) & 0x000000ff,
                            (addr >> 16) & 0x000000ff,
                            (addr >> 8) & 0x000000ff,
                            addr & 0x000000ff)
    
def inet_aton (addr):
    quad = string.splitfields (addr, ".")
    try: 
        rc = ((string.atoi (quad[0]) << 24) +
              (string.atoi (quad[1]) << 16) +
              (string.atoi (quad[2]) << 8) +
              string.atoi (quad[3]))
    except IndexError:
        raise ValueError
    return rc

def inet_calcNetmask (ip):
    if isinstance (ip, type (0)):
        addr = inet_ntoa (ip)
    else:
        addr = ip
    quad = string.splitfields (addr, ".")
    if len (quad) > 0:
        klass = string.atoi (quad[0])
        if klass <= 127:
            mask = "255.0.0.0";
        elif klass <= 191:
            mask = "255.255.0.0";
        else:
            mask = "255.255.255.0";
    return mask
    
def inet_calcNetBroad (ip, nm):
    if isinstance (ip, type ("")):
        ipaddr = inet_aton (ip)
    else:
        ipaddr = ip

    if isinstance (nm, type ("")):
        nmaddr = inet_aton (nm)
    else:
        nmaddr = nm

    netaddr = ipaddr & nmaddr
    bcaddr = netaddr | (~nmaddr);
            
    return (inet_ntoa (netaddr), inet_ntoa (bcaddr))

def inet_calcGateway (bc):
    if isinstance (bc, type ("")):
        bcaddr = inet_aton (bc)
    else:
        bcaddr = bc

    return inet_ntoa (bcaddr - 1)

def inet_calcNS (net):
    if isinstance (net, type ("")):
        netaddr = inet_aton (net)
    else:
        netaddr = net

    return inet_ntoa (netaddr + 1)

def parseArgv(str):
    return _isys.poptParseArgv(str)

def getopt(*args):
    return apply(_isys.getopt, args)

def compareDrives(first, second):
    type1 = first[0:2]
    type2 = second[0:2]

    if type1 == "hd":
	type1 = 0
    elif type1 == "sd":
	type1 = 1
    else:
	type1 = 2

    if type2 == "hd":
	type2 = 0
    elif type2 == "sd":
	type2 = 1
    else:
	type2 = 2

    if (type1 < type2):
	return -1
    elif (type1 > type2):
	return 1
    elif first < second:
	return -1
    elif first > second:
	return 1

    return 0

def configNetDevice(device, ip, netmask, gw):
    return _isys.confignetdevice(device, ip, netmask, gw)

def resetResolv():
    return _isys.resetresolv()

def setResolvRetry(count):
    return _isys.setresretry(count)

def pumpNetDevice(device):
    # returns None on failure, "" if no nameserver is found, nameserver IP
    # otherwise
    return _isys.pumpnetdevice(device)

def readExt2Label(device):
    makeDevInode(device, "/tmp/disk")
    label = _isys.e2fslabel("/tmp/disk");
    os.unlink("/tmp/disk")
    return label

def ext2IsDirty(device):
    makeDevInode(device, "/tmp/disk")
    label = _isys.e2dirty("/tmp/disk");
    os.unlink("/tmp/disk")
    return label

def ext2HasJournal(device, makeDevNode = 1):
    if makeDevNode:
        makeDevInode(device, "/tmp/disk")
        hasjournal = _isys.e2hasjournal("/tmp/disk");
        os.unlink("/tmp/disk")
    else:
        hasjournal = _isys.e2hasjournal(device);
    return hasjournal

def ejectCdrom(device, makeDevice = 1):
    if makeDevice:
        makeDevInode(device, "/tmp/cdrom")
        fd = os.open("/tmp/cdrom", os.O_RDONLY)
    else:
        fd = os.open(device, os.O_RDONLY)

    # this is a best effort
    try:
	_isys.ejectcdrom(fd)
    except SystemError:
	pass

    os.close(fd)

    if makeDevice:
        os.unlink("/tmp/cdrom")

def driveIsRemovable(device):
    # assume ide if starts with 'hd', and we don't have to create
    # device beforehand since it just reads /proc/ide
    if device[:2] == "hd":
        rc = (_isys.isIdeRemovable("/dev/"+device) == 1)
    else:
        makeDevInode(device, "/tmp/disk")
        rc = (_isys.isScsiRemovable("/tmp/disk") == 1)

        # need to test if its USB or IEEE1394 even if it doesnt look removable
        if not rc:
            if os.access("/tmp/scsidisks", os.R_OK):
                sdlist=open("/tmp/scsidisks", "r")
                sdlines = sdlist.readlines()
                sdlist.close()
                for l in sdlines:
                    try:
                        # each line has format of:  <device>  <module>
                        (sddev, sdmod) = string.split(l)

                        if sddev == device:
                            if sdmod in ['sbp2', 'usb-storage']:
                                rc = 1
                            else:
                                rc = 0
                            break
                    except:
                        pass
                  
        os.unlink("/tmp/disk")

    return rc

def vtActivate (num):
    _isys.vtActivate (num)

def isPsudoTTY (fd):
    return _isys.isPsudoTTY (fd)

def sync ():
    return _isys.sync ()

def isIsoImage(file):
    return _isys.isisoimage(file)

def getGeometry(device):
    makeDevInode(device, "/tmp/disk")
    rc = _isys.getGeometry("/tmp/disk")
    os.unlink("/tmp/disk")
    return rc

def getLength(device):
    return _isys.getLength(device)

def fbinfo():
    return _isys.fbinfo()

def cdRwList():
    if not os.access("/proc/sys/dev/cdrom/info", os.R_OK): return []

    f = open("/proc/sys/dev/cdrom/info", "r")
    lines = f.readlines()
    f.close()

    driveList = []
    finalDict = {}

    for line in lines:
	line = string.split(line, ':', 1)

	if (line and line[0] == "drive name"):
	    line = string.split(line[1])
	    # no CDROM drives
	    if not line:  return []

	    for device in line:
		if device[0:2] == 'sr':
		    device = "scd" + device[2:]
		driveList.append(device)
	elif ((line and line[0] == "Can write CD-R") or
	      (line and line[0] == "Can write CD-RW")):
	    line = string.split(line[1])
	    field = 0
	    for ability in line:
		if ability == "1":
		    finalDict[driveList[field]] = 1
		field = field + 1

    l = finalDict.keys()
    l.sort()
    return l

def ideCdRwList():
    newList = []
    for dev in cdRwList():
	if dev[0:2] == 'hd': newList.append(dev)

    return newList

def getpagesize():
    return _isys.getpagesize()

def getLinkStatus(dev):
    return _isys.getLinkStatus(dev)

def getMacAddress(dev):
    return _isys.getMacAddress(dev)

def getIPAddress(dev):
    return _isys.getIPAddress(dev)

def startBterm():
    return _isys.startBterm()

printObject = _isys.printObject
bind_textdomain_codeset = _isys.bind_textdomain_codeset
isVioConsole = _isys.isVioConsole

