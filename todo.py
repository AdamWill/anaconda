import rpm, os
import iutil, isys
from lilo import LiloConfiguration
arch = iutil.getArch ()
if arch == "sparc":
    from silo import SiloInstall
elif arch == "alpha":
    from milo import MiloInstall, onMILO
import string
import socket
import crypt
import whrandom
import pcmcia
import _balkan
import kudzu
from kbd import Keyboard
from simpleconfig import SimpleConfigFile
from mouse import Mouse
from xf86config import XF86Config
import errno

def _(x):
    return x

class FakeDDruid:
    """A disk druid looking thing for upgrades"""
    def partitionList (self):
        return (self.partitions, None)
        
    def append (self, name, table):
        for i in range (len (table)):
            (type, sector, size) = table[i]
            if size and type != -1:
                self.partitions.append ((name + str (i + 1)),
                                        "Existing000" + str(len (self.partitions)),
                                        type, sector, size)
    def __init__ (self):
        self.partitions = []
        
class LogFile:
    def __init__ (self, serial):
	if serial:
	    self.logFile = open("/tmp/install.log", "w")
	else:
	    self.logFile = open("/dev/tty3", "w")

    def __call__ (self, format, *args):
        if args:
            self.logFile.write ("* %s\n" % (format % args))
        else:
            self.logFile.write ("* %s\n" % format)

    def getFile (self):
        return self.logFile.fileno ()
            
class NetworkDevice (SimpleConfigFile):
    def __str__ (self):
        s = ""
        s = s + "DEVICE=" + self.info["DEVICE"] + "\n"
        keys = self.info.keys ()
        keys.sort ()
        keys.remove ("DEVICE")
        for key in keys:
            s = s + key + "=" + self.info[key] + "\n"
        return s

    def __init__ (self, dev):
        self.info = { "DEVICE" : dev, "ONBOOT" : "yes" }
        self.hostname = ""

class Network:
    def __init__ (self):
        self.netdevices = {}
        self.gateway = ""
        self.primaryNS = ""
        self.secondaryNS = ""
        self.ternaryNS = ""
        self.domains = []
        self.readData = 0
        self.hostname = "localhost.localdomain"
        try:
            f = open ("/tmp/netinfo", "r")
        except:
            pass
        else:
            lines = f.readlines ()
	    f.close ()
            info = {}
            for line in lines:
                netinf = string.splitfields (line, '=')
                info [netinf[0]] = string.strip (netinf[1])
            self.netdevices [info["DEVICE"]] = NetworkDevice (info["DEVICE"])
            if info.has_key ("IPADDR"):
                self.netdevices [info["DEVICE"]].set (("IPADDR", info["IPADDR"]))
            if info.has_key ("NETMASK"):
                self.netdevices [info["DEVICE"]].set (("NETMASK", info["NETMASK"]))
            if info.has_key ("BOOTPROTO"):
                self.netdevices [info["DEVICE"]].set (("BOOTPROTO", info["BOOTPROTO"]))
            if info.has_key ("GATEWAY"):
                self.gateway = info["GATEWAY"]
            if info.has_key ("DOMAIN"):
                self.domains.append(info["DOMAIN"])
            if info.has_key ("HOSTNAME"):
                self.hostname = info["HOSTNAME"]
            
            self.readData = 1
	try:
	    f = open ("/etc/resolv.conf", "r")
	except:
	    pass
	else:
	    lines = f.readlines ()
	    f.close ()
	    for line in lines:
		resolv = string.split (line)
		if resolv[0] == 'nameserver':
		    if self.primaryNS == "":
			self.primaryNS = resolv[1]
		    elif self.secondaryNS == "":
			self.secondaryNS = resolv[1]
		    elif self.ternaryNS == "":
			self.ternaryNS = resolv[1]
    
    def available (self):
        f = open ("/proc/net/dev")
        lines = f.readlines()
        f.close ()
        # skip first two lines, they are header
        lines = lines[2:]
        for line in lines:
            dev = string.strip (line[0:6])
            if dev != "lo" and not self.netdevices.has_key (dev):
                self.netdevices[dev] = NetworkDevice (dev)
        return self.netdevices

    def guessHostnames (self):
        # guess the hostname for the first device with an IP
        # XXX fixme - need to set up resolv.conf
        self.domains = []
        for dev in self.netdevices.values ():
            ip = dev.get ("ipaddr")
            if ip:
                try:
                    (hostname, aliases, ipaddrs) = socket.gethostbyaddr (ip)
                except socket.error:
                    hostname = ""
                if hostname:
                    dev.hostname = hostname
                    if '.' in hostname:
                        # chop off everything before the leading '.'
                        self.domains.append (hostname[(string.find (hostname, '.') + 1):])
                #if self.hostname == "localhost.localdomain":
                    self.hostname = hostname
            else:
                dev.hostname = "localhost.localdomain"
        if not self.domains:
            self.domains = [ "localdomain" ]

    def nameservers (self):
        return [ self.primaryNS, self.secondaryNS, self.ternaryNS ]

class Password:
    def __init__ (self):
        self.crypt = ""
	self.pure = None

    def getPure(self):
	return self.pure

    def set (self, password, isCrypted = 0):
        if not isCrypted:
            salt = (whrandom.choice (string.letters +
                                     string.digits + './') + 
                    whrandom.choice (string.letters +
                                     string.digits + './'))
            self.crypt = crypt.crypt (password, salt)
	    self.pure = password
        else:
            self.crypt = password

    def get (self):
        return self.crypt

class Desktop (SimpleConfigFile):
    def __init__ (self):
        SimpleConfigFile.__init__ (self)

    def set (self, desktop):
        self.info ['DESKTOP'] = desktop

class Language (SimpleConfigFile):
    def __init__ (self):
        self.info = {}
        self.langs = {
            "Czech"	 : "cs_CZ" ,
            "English"	 : "en_US" ,
            "French"	 : "fr_FR" ,
            "German"	 : "de_DE" ,
            "Hungarian"	 : "hu_HU" ,
            "Icelandic"	 : "is_IS" ,
            "Indonesian" : "id_ID" ,
            "Italian"	 : "it_IT" ,
            "Japanese"	 : "ja_JP.ujis" ,
            "Norwegian"	 : "no_NO" ,
            "Polish"	 : "pl_PL" ,
            "Romanian"	 : "ro_RO" ,
            "Slovak"	 : "sk_SK" ,
	    "Slovenian"	 : "sl_SI" ,
            "Spanish"	 : "es_MX" ,
            "Russian"	 : "ru_RU.KOI8-R" ,
            "Ukrainian"	 : "uk_UA" ,
            }
        
        # kickstart needs this
        self.abbrevMap = {}
        for (key, value) in self.langs.items ():
            self.abbrevMap[value] = key

	self.setByAbbrev("en_US")

    def available (self):
        return self.langs

    def setByAbbrev(self, lang):
	self.set(self.abbrevMap[lang])
    
    def set (self, lang):
        self.lang = self.langs[lang]
        self.info["LANG"] = self.langs[lang]
        os.environ["LANG"] = self.langs[lang]
        self.info["LINGUAS"] = self.langs[lang]
        os.environ["LINGUAS"] = self.langs[lang]        
        self.info["LC_ALL"] = self.langs[lang]
        os.environ["LC_ALL"] = self.langs[lang]
        
    def get (self):
	return self.lang

class Authentication:
    def __init__ (self):
        self.useShadow = 1
        self.useMD5 = 1
        self.useNIS = 0
        self.domain = ""
        self.useBroadcast = 1
        self.server = ""

class Drives:
    def available (self):
        return isys.hardDriveList ()

class InstSyslog:
    def __init__ (self, root, log):
        self.pid = os.fork ()
        if not self.pid:
            if os.access ("./anaconda", os.X_OK):
                path = "./anaconda"
            elif os.access ("/usr/bin/anaconda.real", os.X_OK):
                path = "/usr/bin/anaconda.real"
            else:
                path = "/usr/bin/anaconda"
            os.execv (path, ("syslogd", "--syslogd", root, log))

    def __del__ (self):
        os.kill (self.pid, 15)
        
class ToDo:
    def __init__(self, intf, method, rootPath, setupFilesystems = 1,
		 installSystem = 1, mouse = None, instClass = None, x = None,
		 expert = 0, serial = 0, extraModules = []):
	self.intf = intf
	self.method = method
	self.mounts = {}
	self.hdList = None
	self.comps = None
	self.instPath = rootPath
	self.setupFilesystems = setupFilesystems
	self.installSystem = installSystem
        self.language = Language ()
	self.serial = serial
        self.log = LogFile (serial)
        self.network = Network ()
        self.rootpassword = Password ()
        self.extraModules = extraModules
        if mouse:
            self.mouse = Mouse (xmouseType = mouse)
        else:
            self.mouse = Mouse ()
        self.keyboard = Keyboard ()
        self.auth = Authentication ()
        self.desktop = Desktop ()
        self.ddruid = None
        self.ddruidReadOnly = 0
        self.drives = Drives ()
        self.badBlockCheck = 0
        self.bootdisk = 0
	self.liloImages = {}
        self.liloDevice = None
        self.liloLinear = 0
        self.liloAppend = None
        arch = iutil.getArch ()
	if arch == "sparc":
	    self.silo = SiloInstall (self)
        elif arch == "alpha":
            self.milo = MiloInstall (self)
	self.timezone = None
        self.upgrade = 0
	self.ddruidAlreadySaved = 0
	self.initrdsMade = {}
        self.initlevel = 3
	self.expert = expert
        self.progressWindow = None
	self.swapCreated = 0
	self.fdDevice = None
	if (not instClass):
	    raise TypeError, "installation class expected"
        if x:
            self.x = x
        else:
            if mouse:
                self.x = XF86Config (mouse = mouse)
            else:
                self.x = XF86Config ()

	# This absolutely, positively MUST BE LAST
	self.setClass(instClass)

        if self.setupFilesystems:
            try:
                f = open("/dev/tty5")
                f.close ()
            except:
                pass

    def setFdDevice(self):
	if self.fdDevice:
	    return
	self.fdDevice = "/dev/fd0"
	if iutil.getArch() == "sparc":
	    try:
		f = open(self.fdDevice, "r")
	    except IOError, (errnum, msg):
		if errno.errorcode[errnum] == 'ENXIO':
		    self.fdDevice = "/dev/fd1"
	    else:
		f.close()

    def umountFilesystems(self):
	if (not self.setupFilesystems): return 

	isys.umount(self.instPath + '/proc')

        keys = self.mounts.keys ()
	keys.sort()
	keys.reverse()
	for n in keys:
            (device, fsystem, format) = self.mounts[n]
            if fsystem != "swap":
		try:
		    mntPoint = self.instPath + n
		    self.log("unmounting " + mntPoint)
                    isys.umount(mntPoint)
		except SystemError, (errno, msg):
		    self.intf.messageWindow(_("Error"), 
			_("Error unmounting %s: %s") % (device, msg))

    def writeTimezone(self):
	if (self.timezone):
	    (timezone, asUtc, asArc) = self.timezone
	    iutil.copyFile(self.instPath + "/usr/share/zoneinfo/" + timezone, 
		       self.instPath + "/etc/localtime")
	else:
	    asUtc = 0
	    asArc = 0

	f = open(self.instPath + "/etc/sysconfig/clock", "w")
	f.write("UTC=")
	if (asUtc):
	    f.write("true\n")
	else:
	    f.write("false\n")

	f.write("ARC=")
	if (asArc):
	    f.write("true\n")
	else:
	    f.write("false\n")
	f.close()

    def getTimezoneInfo(self):
	return self.timezone

    def setTimezoneInfo(self, timezone, asUtc = 0, asArc = 0):
	self.timezone = (timezone, asUtc, asArc)

    def getLiloOptions(self):
        if self.mounts.has_key ('/boot'):
            bootpart = self.mounts['/boot'][0]
        else:
            bootpart = self.mounts['/'][0]
        i = len (bootpart) - 1
        while i < 0 and bootpart[i] in digits:
            i = i - 1
	drives = self.drives.available().keys()
	drives.sort(isys.compareDrives)
        boothd = drives[0]

	return (bootpart, boothd)

    def setLiloImages(self, images):
	self.liloImages = images

    def getLiloImages(self):
        if not self.ddruid:
            raise RuntimeError, "No disk druid object"

        (drives, raid) = self.ddruid.partitionList()

	# rearrange the fstab so it's indexed by device
	mountsByDev = {}
	for loc in self.mounts.keys():
	    (device, fsystem, reformat) = self.mounts[loc]
	    mountsByDev[device] = loc

	oldImages = {}
	for dev in self.liloImages.keys():
	    oldImages[dev] = self.liloImages[dev]

	self.liloImages = {}
        foundDos = 0
	for (dev, devName, type, start, size) in drives:
	    # ext2 partitions get listed if 
	    #	    1) they're /
	    #	    2) they're not mounted

            # only list dos and ext2 partitions
            if type != 1 and type != 2:
                continue

	    if (mountsByDev.has_key(dev)):
		if mountsByDev[dev] == '/':
		    self.liloImages[dev] = ("linux", 2)
	    else:
		if not oldImages.has_key(dev):
		    self.liloImages[dev] = ("", type)
		else:
		    self.liloImages[dev] = oldImages[dev]
            if type == 1:
		if foundDos: continue
		foundDos = 1
		self.liloImages[dev] = ("dos", type)

	return self.liloImages

    def createRaidTab(self, file, devPrefix, createDevices = 0):
        (devices, raid) = self.ddruid.partitionList()
	if not raid: return

	deviceDict = {}
	for (device, name, type, start, size) in devices:
	    deviceDict[name] = device

	rt = open(file, "w")
	for (mntpoint, device, fstype, raidType, start, size, makeup) in raid:

	    if createDevices:
		isys.makeDevInode(device, devPrefix + '/' + device)

	    rt.write("raiddev		    %s/%s\n" % (devPrefix, device,))
	    rt.write("raid-level		    %d\n" % (raidType,))
	    rt.write("nr-raid-disks		    %d\n" % (len(makeup),))
	    rt.write("chunk-size		    64k\n")
	    rt.write("persistent-superblock	    1\n");
	    rt.write("#nr-spare-disks	    0\n")
	    i = 0
	    for subDevName in makeup:
		isys.makeDevInode(deviceDict[subDevName], '%s/%s' % 
			    (devPrefix, deviceDict[subDevName]))
		rt.write("    device	    %s/%s\n" % 
		    (devPrefix, deviceDict[subDevName],))
		rt.write("    raid-disk     %d\n" % (i,))
		i = i + 1

	rt.write("\n")
	rt.close()

    def mountFilesystems(self):
	if (not self.setupFilesystems): return 

        keys = self.mounts.keys ()
	keys.sort()
        for mntpoint in keys:
            (device, fsystem, format) = self.mounts[mntpoint]
            if fsystem == "swap":
		continue
	    elif fsystem == "ext2":
		try:
		    iutil.mkdirChain(self.instPath + mntpoint)
		    isys.makeDevInode(device, '/tmp/' + device)
		    isys.mount('/tmp/' + device, 
				self.instPath + mntpoint)
		    os.remove( '/tmp/' + device);
		except SystemError, (errno, msg):
		    self.intf.messageWindow(_("Error"), 
			_("Error mounting %s: %s") % (device, msg))

        try:
            os.mkdir (self.instPath + '/proc')
        except:
            pass
            
	isys.mount('/proc', self.instPath + '/proc', 'proc')

    def makeFilesystems(self, createSwap = 1, createFs = 1):
        if not self.setupFilesystems: return

	# let's make the RAID devices first -- the fstab will then proceed
	# naturally
        (devices, raid) = self.ddruid.partitionList()

	if raid:
	    self.createRaidTab("/tmp/raidtab", "/tmp", createDevices = 1)

	    w = self.intf.waitWindow(_("Creating"),
			  _("Creating RAID devices..."))

	    for (mntpoint, device, fsType, raidType, start, size, makeup) in raid:
                iutil.execWithRedirect ("/usr/sbin/mkraid", 
			[ 'mkraid', '--really-force', '--configfile', 
			  '/tmp/raidtab', '/tmp/' + device ])

	    w.pop()
        
	    # XXX remove extraneous inodes here

        keys = self.mounts.keys ()

	keys.sort()

        arch = iutil.getArch ()

        if arch == "alpha":
            if '/boot' in keys:
                kernelPart = '/boot'
            else:
                kernelPart = '/'
        
	for mntpoint in keys:
	    (device, fsystem, format) = self.mounts[mntpoint]
	    if not format: continue
	    isys.makeDevInode(device, '/tmp/' + device)
            if fsystem == "ext2" and createFs:
                args = [ "mke2fs", '/tmp/' + device ]
                # FORCE the partition that MILO has to read
                # to have 1024 block size.  It's the only
                # thing that our milo seems to read.
                #    --- ALSO ---
                # if you turn on revision 1 ext2 then the
                # file_type flag (high 8 bits of the name_len
                # field) will get set, throwing off both MILO
                # and aboot.  I've fixed aboot to read these
                # correctly, but MILO is _such_ a pain to build
                # that I don't want to deal with it right now.
                if arch == "alpha" and mntpoint == kernelPart:
                    args = args + ["-b", "1024", '-r', '-O', 'none']
                # set up raid options for md devices.
                if device[:2] == 'md':
                    for (rmnt, rdevice, fsType, raidType, start, size, makeup) in raid:
                        if rdevice == device:
                            rtype = raidType
                            rdisks = len (makeup)
                    if rtype == 5:
                        rdisks = rdisks - 1
                        args = args + [ '-R', 'stride=%d' % (rdisks * 16) ]
                    elif rtype == 0:
                        args = args + [ '-R', 'stride=%d' % (rdisks * 16) ]                        
                        
                if self.badBlockCheck:
                    args.append ("-c")

		w = self.intf.waitWindow(_("Formatting"),
			      _("Formatting %s filesystem...") % (mntpoint,))

		if self.serial:
		    messages = "/tmp/mke2fs.log"
		else:
		    messages = "/dev/tty5"
                iutil.execWithRedirect ("/usr/sbin/mke2fs",
                                        args,
                                        stdout = messages, stderr = messages,
                                        searchPath = 1)
		w.pop()
            elif fsystem == "swap" and createSwap:
		w = self.intf.waitWindow(_("Formatting"),
			      _("Formatting %s filesystem...") % (mntpoint,))

                rc = iutil.execWithRedirect ("/usr/sbin/mkswap",
                                             [ "mkswap", '-v1', '/tmp/' + device ],
                                             stdout = None, stderr = None,
                                             searchPath = 1)
                if rc:
                    raise RuntimeError, "error making swap on " + device
		isys.swapon ('/tmp/' + device)
		w.pop()
            else:
                pass

            os.remove('/tmp/' + device)

	if createSwap:
	    self.swapCreated = 1

    def addMount(self, device, location, fsystem, reformat = 1):
        if fsystem == "swap":
            ufs = 0
            try:
                isys.makeDevInode(device, '/tmp/' + device)
            except:
                pass
            try:
                ufs = isys.checkUFS ('/tmp/' + device)
            except:
                pass
            if not ufs:
                location = "swap"
                reformat = 1
        self.mounts[location] = (device, fsystem, reformat)

    def resetMounts(self):
	self.mounts = {}

    def writeFstab(self):
	format = "%-23s %-23s %-7s %-15s %d %d\n";

	f = open (self.instPath + "/etc/fstab", "w")
        keys = self.mounts.keys ()
	keys.sort ()
	self.setFdDevice ()
	for mntpoint in keys: 
	    (dev, fs, reformat) = self.mounts[mntpoint]
            if fs != "swap":
                iutil.mkdirChain(self.instPath + mntpoint)
	    if (mntpoint == '/'):
		f.write (format % ( '/dev/' + dev, mntpoint, fs, 'defaults', 1, 1))
	    else:
                if (fs == "ext2"):
                    f.write (format % ( '/dev/' + dev, mntpoint, fs, 'defaults', 1, 2))
                elif fs == "iso9660":
                    f.write (format % ( '/dev/' + dev, mntpoint, fs, 'noauto,owner,ro', 0, 0))
                else:
                    f.write (format % ( '/dev/' + dev, mntpoint, fs, 'defaults', 0, 0))
	f.write (format % (self.fdDevice, "/mnt/floppy", 'ext2', 'noauto,owner', 0, 0))
	f.write (format % ("none", "/proc", 'proc', 'defaults', 0, 0))
	f.write (format % ("none", "/dev/pts", 'devpts', 'gid=5,mode=620', 0, 0))
	f.close ()
        # touch mtab
        open (self.instPath + "/etc/mtab", "w+")
        f.close ()

	self.createRaidTab("/mnt/sysimage/etc/raidtab", "/dev")

    def readFstab (self, path):
        f = open (path, "r")
        lines = f.readlines ()
        f.close
        fstab = {}
        for line in lines:
            fields = string.split (line)
            # skip comments
            if fields and fields[0][0] == '#':
                continue
	    if not fields: continue
            # all valid fstab entries have 6 fields
            if len (fields) < 4 or len (fields) > 6: continue
 
 	    if fields[2] != "ext2" and fields[2] != "swap": continue
 	    if string.find(fields[3], "noauto") != -1: continue
 	    if (fields[0][0:7] != "/dev/hd" and 
 		fields[0][0:7] != "/dev/sd" and
 		fields[0][0:8] != "/dev/rd/" and
 		fields[0][0:9] != "/dev/ida/"): continue
            
 	    format = 0
 	    # XXX always format swap. 
 	    if fields[2] == "swap": format = 1
 	    fstab[fields[1]] = (fields[0][5:], fields[2], format)
        return fstab

    def writeLanguage(self):
	f = open(self.instPath + "/etc/sysconfig/i18n", "w")
	f.write(str (self.language))
	f.close()

    def writeMouse(self):
	if self.serial: return
	f = open(self.instPath + "/etc/sysconfig/mouse", "w")
	f.write(str (self.mouse))
	f.close()
	self.mouse.makeLink(self.instPath)

    def writeDesktop(self):
	f = open(self.instPath + "/etc/sysconfig/desktop", "w")
	f.write(str (self.desktop))
	f.close()

    def writeKeyboard(self):
	if self.serial: return
	f = open(self.instPath + "/etc/sysconfig/keyboard", "w")
	f.write(str (self.keyboard))
	f.close()

    def makeInitrd (self, kernelTag):
	initrd = "/boot/initrd%s.img" % (kernelTag, )
	if not self.initrdsMade.has_key(initrd):
            iutil.execWithRedirect("/sbin/mkinitrd",
                                  [ "/sbin/mkinitrd",
				    "--ifneeded",
                                    initrd,
                                    kernelTag[1:] ],
                                  stdout = None, stderr = None, searchPath = 1,
                                  root = self.instPath)
	    self.initrdsMade[kernelTag] = 1
	return initrd

    def makeBootdisk (self):
	kernel = self.hdList['kernel']
        kernelTag = "-%s-%s" % (kernel['version'], kernel['release'])

        self.makeInitrd (kernelTag)
        w = self.intf.waitWindow (_("Creating"), _("Creating boot disk..."))
	self.setFdDevice ()
        rc = iutil.execWithRedirect("/sbin/mkbootdisk",
                                    [ "/sbin/mkbootdisk",
                                      "--noprompt",
                                      "--device",
                                      self.fdDevice,
                                      kernelTag[1:] ],
                                    stdout = None, stderr = None, 
				    searchPath = 1, root = self.instPath)
        w.pop()
        if rc:
            raise RuntimeError, "boot disk creation failed"

    def installLilo (self):
	lilo = LiloConfiguration ()

	if not self.liloImages:
	    self.setLiloImages(self.getLiloImages())

        # OK - for this release we need to just blow away the old lilo.conf
        # just like we used to.
##         # on upgrade read in the lilo config file
##         if os.access (self.instPath + '/etc/lilo.conf', os.R_OK):
##             lilo.read (self.instPath + '/etc/lilo.conf')
##         elif not self.liloDevice: return

        if os.access (self.instPath + '/etc/lilo.conf', os.R_OK):
	    os.rename(self.instPath + '/etc/lilo.conf',
		      self.instPath + '/etc/lilo.conf.rpmsave')

        if os.access (self.instPath + '/etc/lilo.conf', os.R_OK):
	    os.rename(self.instPath + '/etc/lilo.conf',
		      self.instPath + '/etc/lilo.conf.rpmsave')

	(bootpart, boothd) = self.getLiloOptions()
	if (type((1,)) == type(bootpart)):
	    (kind, self.liloDevice) = bootpart
	elif (self.liloDevice == "mbr"):
	    self.liloDevice = boothd
	else:
	    self.liloDevice = bootpart

        if self.liloDevice:
            lilo.addEntry("boot", '/dev/' + self.liloDevice)
	lilo.addEntry("map", "/boot/map")
	lilo.addEntry("install", "/boot/boot.b")
	lilo.addEntry("prompt")
	lilo.addEntry("timeout", "50")
	if self.liloLinear:
	    lilo.addEntry("linear")

	smpInstalled = (self.hdList.has_key('kernel-smp') and 
                        self.hdList['kernel-smp'].selected)
	if (self.upgrade and not isys.smpAvailable()):
	    self.log("upgrading to a smp kernel on a up system; configuring "
		     "up in lilo")
	    smpInstalled = 0

        if self.mounts.has_key ('/'):
            (dev, fstype, format) = self.mounts['/']
            rootDev = dev
        else:
            raise RuntimeError, "Installing lilo, but there is no root device"

        kernelList = []
        otherList = []

        main = "linux"

        for (drive, (label, liloType)) in self.liloImages.items ():
            if (drive == rootDev) and label:
                main = label
            elif label:
                otherList.append (label, "/dev/" + drive)

        lilo.addEntry("default", main)        

	label = main
	if (smpInstalled):
	    kernelList.append((main, self.hdList['kernel-smp'], "smp"))
	    label = main + "-up"

	kernelList.append((label, self.hdList['kernel'], ""))

	for (label, kernel, tag) in kernelList:
	    kernelTag = "-%s-%s%s" % (kernel['version'], kernel['release'], tag)
	    initrd = self.makeInitrd (kernelTag)

	    sl = LiloConfiguration()

	    sl.addEntry("label", label)
	    if os.access (self.instPath + initrd, os.R_OK):
		sl.addEntry("initrd", initrd)

	    sl.addEntry("read-only")
	    sl.addEntry("root", '/dev/' + rootDev)
	    kernelFile = "/boot/vmlinuz" + kernelTag

	    if self.liloAppend:
		sl.addEntry('append', '"%s"' % (self.liloAppend,))
		
	    lilo.addImage ("image", kernelFile, sl)

	for (label, device) in otherList:
	    sl = LiloConfiguration()
	    sl.addEntry("label", label)
	    lilo.addImage ("other", device, sl)

        for (liloType, name, config) in lilo.images:
            # remove entries for missing kernels (upgrade)
            if liloType == "image":
                if not os.access (self.instPath + name, os.R_OK):
                    lilo.delImage (name)
            # remove entries for unbootable partitions
            elif liloType == "other":
                device = name[5:]
                isys.makeDevInode(device, '/tmp/' + device)
                if not isys.checkBoot ('/tmp/' + device):
                    lilo.delImage (name)
                os.remove ('/tmp/' + device)

        # pass 2, remove duplicate entries
        labels = []

        for (liloType, name, config) in lilo.images:
            if not name in labels:
                labels.append (name)
            else: # duplicate entry, first entry wins
                lilo.delImage (name)                

	lilo.write(self.instPath + "/etc/lilo.conf")

        # XXX make me "not test mode"
        if self.setupFilesystems:
            iutil.execWithRedirect(self.instPath + '/sbin/lilo' ,
                                   [ "lilo", "-r", self.instPath ],
                                   stdout = None)

    def freeHeaderList(self):
	if (self.hdList):
	    self.hdList = None

    def getHeaderList(self):
	if (not self.hdList):
	    w = self.intf.waitWindow(_("Reading"),
                                     _("Reading package information..."))
	    self.hdList = self.method.readHeaders()
	    w.pop()
	return self.hdList

    def setLiloLocation(self, location):
	self.liloDevice = location

    def getLiloLocation (self):
        return self.liloDevice

    def getCompsList(self):
	if (not self.comps):
	    self.getHeaderList()
	    self.comps = self.method.readComps(self.hdList)
	    for comp in self.comps:
		if comp.selected:
		    comp.select (1)
	self.comps['Base'].select(1)

	self.updateInstClassComps()
            
	return self.comps

    def updateInstClassComps(self):
	# don't load it just for this
	if (not self.comps): return
	group = self.instClass.getGroups()
	packages = self.instClass.getPackages()
	if (group == None and packages == None): return 0
	for n in self.comps.keys():
	    self.comps[n].unselect(0)

	self.comps['Base'].select(1)
	if group:
	    for n in group:
		self.comps[n].select(1)

	if packages:
	    for n in packages:
		self.selectPackage(n)

	if self.x.server:
	    self.selectPackage('XFree86-' + self.x.server)

    def selectPackage(self, package):
	self.hdList.packages[package].selected = 1

    def writeNetworkConfig (self):
        # /etc/sysconfig/network-scripts/ifcfg-*
        for dev in self.network.netdevices.values ():
            device = dev.get ("device")
            f = open (self.instPath + "/etc/sysconfig/network-scripts/ifcfg-" + device, "w")
            f.write (str (dev))
            f.close ()

        # /etc/sysconfig/network

        for dev in self.network.netdevices.values ():
            if dev.hostname:
                hostname = dev.hostname
                break
        
        f = open (self.instPath + "/etc/sysconfig/network", "w")
        f.write ("NETWORKING=yes\n"
                 "FORWARD_IPV4=false\n"
                 "HOSTNAME=" + self.network.hostname + "\n"
                 "GATEWAY=" + self.network.gateway + "\n")
        f.close ()

        # /etc/hosts
        f = open (self.instPath + "/etc/hosts", "w")
        localline = "127.0.0.1\t\t"
        if self.network.hostname != "localhost.localdomain":
            localline = localline + self.network.hostname + " "
	localline = localline + "localhost.localdomain localhost\n"
        f.write (localline)
        for dev in self.network.netdevices.values ():
            ip = dev.get ("ipaddr")
            if dev.hostname and ip and dev.hostname != "localhost.localdomain":
                f.write ("%s\t\t%s\n" % (ip, dev.hostname))
        f.close ()

	# If the hostname was not looked up, but typed in by the user,
	# domain might not be computed, so do it now.
	if self.network.domains == [ "localdomain" ] or not self.network.domains:
	    if self.network.hostname != "localhost.localdomain":
		if '.' in self.network.hostname:
		    # chop off everything before the leading '.'
		    domain = self.network.hostname[(string.find(self.network.hostname, '.') + 1):]
		    self.network.domains = [ domain ]

        # /etc/resolv.conf
        f = open (self.instPath + "/etc/resolv.conf", "w")
        f.write ("search " + string.joinfields (self.network.domains, ' ') + "\n")
        for ns in self.network.nameservers ():
            if ns:
                f.write ("nameserver " + ns + "\n")
        f.close ()

    def writeRootPassword (self):
        f = open (self.instPath + "/etc/passwd", "r")
        lines = f.readlines ()
        f.close ()
        index = 0
        for line in lines:
            if line[0:4] == "root":
                entry = string.splitfields (line, ':')
                entry[1] = self.rootpassword.get ()
                lines[index] = string.joinfields (entry, ':')
                break
            index = index + 1
        f = open (self.instPath + "/etc/passwd", "w")
        f.writelines (lines)
        f.close ()

    def setupAuthentication (self):
        args = [ "/usr/sbin/authconfig", "--kickstart", "--nostart" ]
        if self.auth.useShadow:
            args.append ("--useshadow")
        if self.auth.useMD5:
            args.append ("--enablemd5")
        if self.auth.useNIS:
            args.append ("--enablenis")
            args.append ("--nisdomain")
            args.append (self.auth.domain)
            if not self.auth.useBroadcast:
                args.append ("--nisserver")
                args.append (self.auth.server)
        iutil.execWithRedirect(args[0], args,
                              stdout = None, stderr = None, searchPath = 1,
                              root = self.instPath)

    def copyConfModules (self):
        try:
            inf = open ("/tmp/conf.modules", "r")
        except:
            pass
        else:
            out = open (self.instPath + "/etc/conf.modules", "a")
            out.write (inf.read ())

    def verifyDeps (self):
	self.getCompsList()
	ts = rpm.TransactionSet()
        self.comps['Base'].select (1)

	for p in self.hdList.packages.values ():
            if p.selected:
                ts.add(p.h, (p.h, p.h[rpm.RPMTAG_NAME]))
            else:
                ts.add(p.h, (p.h, p.h[rpm.RPMTAG_NAME]), "a")

	ts.order()
        deps = ts.depcheck()
        rc = []
        if deps:
            for ((name, version, release),
                 (reqname, reqversion),
                 flags, suggest, sense) in deps:
                if sense == rpm.RPMDEP_SENSE_REQUIRES:
                    if suggest:
                        (header, sugname) = suggest
                    else:
                        sugname = _("no suggestion")
                    if not (name, sugname) in rc:
                        rc.append ((name, sugname))
            return rc
        else:
            return None

    def selectDeps (self, deps):
        if deps:
            for (who, dep) in deps:
                if dep != _("no suggestion"):
                    self.hdList[dep].selected = 1

    def upgradeFindRoot (self):
        rootparts = []
        if not self.setupFilesystems: return [ self.instPath ]
        win = self.intf.waitWindow (_("Searching"),
                                    _("Searching for Red Hat Linux installations..."))
        
        drives = self.drives.available ().keys ()
        self.ddruid = FakeDDruid ()
        for drive in drives:
            isys.makeDevInode(drive, '/tmp/' + drive)
            
            try:
                table = _balkan.readTable ('/tmp/' + drive)
            except SystemError:
                pass
            else:
                self.ddruid.append (drive, table)
                for i in range (len (table)):
                    (type, sector, size) = table[i]
                    # 2 is ext2 in balkan speek
                    if size and type == 2:
			# for RAID arrays of format c0d0p1
			if drive [:3] == "rd/" or drive [:4] == "ida/":
                            dev = drive + 'p' + str (i + 1)
			else:
                            dev = drive + str (i + 1)
                        isys.makeDevInode(dev, '/tmp/' + dev)
                        try:
                            isys.mount('/tmp/' + dev, '/mnt/sysimage')
                        except SystemError, (errno, msg):
                            self.intf.messageWindow(_("Error"),
                                                    _("Error mounting ext2 filesystem on %s: %s") % (dev, msg))
                            continue
                        if os.access ('/mnt/sysimage/etc/fstab', os.R_OK):
                            rootparts.append (dev)
                        isys.umount('/mnt/sysimage')
                        os.remove ('/tmp/' + dev)
                    if size and type == 5:
                        dev = drive + str (i + 1)
                        isys.makeDevInode(dev, '/tmp/' + dev)
                        try:
                            isys.swapon ('/tmp/' + dev)
                        except SystemError, (error, msg):
                            self.log ("Error in swapon of %s: %s", dev, msg)
                        os.remove ('/tmp/' + dev)
            os.remove ('/tmp/' + drive)
        win.pop ()
        return rootparts

    def upgradeFindPackages (self, root):
        win = self.intf.waitWindow (_("Finding"),
                                    _("Finding packages to upgrade..."))
        self.getCompsList ()
	self.getHeaderList ()
        if self.setupFilesystems:
            isys.makeDevInode(root, '/tmp/' + root)
            isys.mount('/tmp/' + root, '/mnt/sysimage')
            self.mounts = self.readFstab ('/mnt/sysimage/etc/fstab')
            isys.umount('/mnt/sysimage')        
            self.mountFilesystems ()
        packages = rpm.findUpgradeSet (self.hdList.hdlist, self.instPath)
        self.umountFilesystems ()

        # unselect all packages
        for package in self.hdList.packages.values ():
            package.selected = 0

        # always upgrade all packages in Base package group
	self.comps['Base'].select(1)

        hasX = 0
        hasgmc = 0
        # turn on the packages in the upgrade set
        for package in packages:
            self.hdList[package[rpm.RPMTAG_NAME]].selected = 1
            if package[rpm.RPMTAG_NAME] == "XFree86":
                hasX = 1
            if package[rpm.RPMTAG_NAME] == "gmc":
                hasgmc = 1
                
        # yes, this is rude
        if hasX and not hasgmc:
            self.comps['GNOME'].select(1)
            
        # new package dependency fixup
        self.selectDeps (self.verifyDeps ())
        win.pop ()

    def rpmError (todo):
        todo.instLog.write (rpm.errorString () + "\n")

    def getClass(todo):
	return todo.instClass

    def setClass(todo, instClass):
	todo.instClass = instClass
	todo.hostname = todo.instClass.getHostname()
	todo.updateInstClassComps()
	( useShadow, useMd5, useNIS, nisDomain, nisBroadcast,
		      nisServer) = todo.instClass.getAuthentication()
        todo.auth.useShadow = useShadow
        todo.auth.useMD5 = useMd5
        todo.auth.useNIS = useNIS
        todo.auth.domain = nisDomain
        todo.auth.useBroadcast = nisBroadcast
        todo.auth.server = nisServer
	todo.timezone = instClass.getTimezoneInfo()
	todo.bootdisk = todo.instClass.getMakeBootdisk()
	todo.zeroMbr = todo.instClass.zeroMbr
	(where, linear, append) = todo.instClass.getLiloInformation()
	todo.liloDevice = where
	todo.liloLinear = linear
	todo.liloAppend = append

	for (mntpoint, (dev, fstype, reformat)) in todo.instClass.fstab:
	    todo.addMount(dev, mntpoint, fstype, reformat)

	if todo.ddruid:
	    todo.ddruid = None
	    todo.mounts = {}

	todo.users = []
	if todo.instClass.rootPassword:
	    todo.rootpassword.set(todo.instClass.rootPassword)
	if todo.instClass.language:
	    todo.language.setByAbbrev(todo.instClass.language)
	if todo.instClass.keyboard:
	    todo.keyboard.set(todo.instClass.keyboard)
            if todo.instClass.keyboard != "us":
                xkb = todo.keyboard.getXKB ()
                if xkb:
                    apply (todo.x.setKeyboard, xkb)

	(bootProto, ip, netmask, gateway, nameserver) = \
		todo.instClass.getNetwork()
	if bootProto:
	    todo.network.gateway = gateway
	    todo.network.primaryNS = nameserver

	    devices = todo.network.available ()
	    if (devices and bootProto):
		list = devices.keys ()
		list.sort()
		dev = devices[list[0]]
                dev.set (("bootproto", bootProto))
                if bootProto == "static":
                    if (ip):
                        dev.set (("ipaddr", ip))
                    if (netmask):
                        dev.set (("netmask", netmask))

	if (todo.instClass.mouse):
	    (type, device, emulateThreeButtons) = todo.instClass.mouse
	    todo.mouse.set(type, emulateThreeButtons, thedev = device)

	if (todo.instClass.x):
	    todo.x = todo.instClass.x

        if iutil.getArch () == "alpha":
            instClass.addToSkipList("bootdisk")
            instClass.addToSkipList("lilo")

        if todo.instClass.desktop:
            todo.desktop.set (todo.instClass.desktop)

    def getSkipPartitioning(self):
	return self.instClass.skipPartitioning

    def getPartitionWarningText(self):
	return self.instClass.clearPartText

    def manuallyPartition(self):
	self.instClass.skipPartitioning = 0
	self.instClass.clearPartText = None
	self.instClass.removeFromSkipList("partition")
	self.instClass.removeFromSkipList("format")

    # List of (accountName, fullName, password) tupes
    def setUserList(todo, users):
	todo.users = users

    def getUserList(todo):
	return todo.users

    def createAccounts(todo):
	if not todo.users: return

	for (account, name, password) in todo.users:
	    devnull = os.open("/dev/null", os.O_RDWR)

	    argv = [ "/usr/sbin/useradd", account ]
	    iutil.execWithRedirect(argv[0], argv, root = todo.instPath,
				   stdout = devnull)

	    argv = [ "/usr/bin/chfn", "-f", name, account]
	    iutil.execWithRedirect(argv[0], argv, root = todo.instPath,
				   stdout = devnull)
        
	    argv = [ "/usr/bin/passwd", "--stdin", account ]
	    p = os.pipe()
	    os.write(p[1], password + "\n")
	    iutil.execWithRedirect(argv[0], argv, root = todo.instPath, 
				   stdin = p[0], stdout = devnull)
	    os.close(p[0])
	    os.close(p[1])
	    os.close(devnull)

    def createCdrom(self):
	list = isys.cdromList()
	count = 0
	for device in list:
	    (device, descript) = device
	    cdname = "cdrom"
	    if (count):
		cdname = "%s%d" % (cdname, count)
	    count = count + 1

	    os.symlink(device, self.instPath + "/dev/" + cdname)
	    mntpoint = "/mnt/" + cdname
	    self.mounts[mntpoint] = (cdname, "iso9660", 0)

    def setDefaultRunlevel (self):
        inittab = open (self.instPath + '/etc/inittab', 'r')
        lines = inittab.readlines ()
        inittab.close ()
        inittab = open (self.instPath + '/etc/inittab', 'w')        
        for line in lines:
            if len (line) > 3 and line[:3] == "id:":
                fields = string.split (line, ':')
                fields[1] = str (self.initlevel)
                line = string.join (fields, ':')
            inittab.write (line)
        inittab.close ()
        
    def instCallback(self, what, amount, total, h, intf):
        if (what == rpm.RPMCALLBACK_TRANS_START):
            # step 6 is the bulk of the transaction set
            # processing time
            if amount == 6:
                self.progressWindow = \
                   self.intf.progressWindow (_("Processing"),
                                             _("Preparing to install..."),
                                             total)
        if (what == rpm.RPMCALLBACK_TRANS_PROGRESS):
            if self.progressWindow:
                self.progressWindow.set (amount)
                
        if (what == rpm.RPMCALLBACK_TRANS_STOP and self.progressWindow):
            self.progressWindow.pop ()

        if (what == rpm.RPMCALLBACK_INST_OPEN_FILE):
            intf.setPackage(h)
            intf.setPackageScale(0, 1)
            self.instLog.write (self.modeText % (h[rpm.RPMTAG_NAME],))
            self.instLog.flush ()
            fn = self.method.getFilename(h)
            self.rpmFD = os.open(fn, os.O_RDONLY)
            fn = self.method.unlinkFilename(fn)
            return self.rpmFD
        elif (what == rpm.RPMCALLBACK_INST_PROGRESS):
            intf.setPackageScale(amount, total)
        elif (what == rpm.RPMCALLBACK_INST_CLOSE_FILE):
            os.close (self.rpmFD)
            intf.completePackage(h)
        else:
            pass

    def copyExtraModules(self):
	kernelVersions = []
	
	if (self.hdList.has_key('kernel-smp') and 
	    self.hdList['kernel-smp'].selected):
	    version = (self.hdList['kernel-smp']['version'] + "-" +
		       self.hdList['kernel-smp']['release'] + "smp")
	    kernelVersions.append(version)

	version = (self.hdList['kernel']['version'] + "-" +
		   self.hdList['kernel']['release'])
	kernelVersions.append(version)

        for (path, subdir, name) in self.extraModules:
	    pattern = ""
	    names = ""
	    for n in kernelVersions:
		pattern = pattern + " " + n + "/" + name + ".o"
		names = names + " " + name + ".o"
	    command = ("cd %s/lib/modules; gunzip < %s/modules.cgz | " +
			"%s/bin/cpio  --quiet -iumd %s") % \
		(self.instPath, path, self.instPath, pattern)
	    self.log("running: '%s'" % (command, ))
	    os.system(command)

	    for n in kernelVersions:
		self.log("from %s/lib/modules/%s/%s.o" % (self.instPath, n, name))
		self.log("to %s/lib/modules/%s/%s/%s.o" % (self.instPath, n, 
							subdir, name))
		os.rename("%s/lib/modules/%s/%s.o" % (self.instPath, n, name),
			  "%s/lib/modules/%s/%s/%s.o" % (self.instPath, n, 
							subdir, name))

    def doInstall(self):
	# make sure we have the header list and comps file
	self.getHeaderList()
	self.getCompsList()

        arch = iutil.getArch ()

        if arch == "alpha":
            # if were're on alpha with ARC console, set the clock
            # so that our installed files won't be in the future
            if onMILO ():
                args = ("clock", "-A", "-s")
                try:
                    iutil.execWithRedirect('/usr/sbin/clock', args)
                except:
                    pass

	# this is NICE and LATE. It lets kickstart/server/workstation
	# installs detect this properly
	if (self.hdList.has_key('kernel-smp') and isys.smpAvailable()):
	    self.hdList['kernel-smp'].selected = 1

	# we *always* need a kernel installed
	if (self.hdList.has_key('kernel')):
	    self.hdList['kernel'].selected = 1

        if self.x.server:
            self.selectPackage ('XFree86-' + self.x.server)

        # make sure that all comps that include other comps are
        # selected (i.e. - recurse down the selected comps and turn
        # on the children
        if self.setupFilesystems:
            if not self.upgrade:
		if (self.ddruidAlreadySaved):
		    self.makeFilesystems (createSwap = 0)
		else:
		    self.ddruid.save ()
		    self.makeFilesystems (createSwap = (not self.swapCreated))
            else:
                (drives, raid) = self.ddruid.partitionList()

            self.mountFilesystems ()

        if self.upgrade:
            w = self.intf.waitWindow(_("Rebuilding"), 
                                     _("Rebuilding RPM database..."))
            rc = rpm.rebuilddb (self.instPath)
            w.pop ()
            if rc:
                self.intf.messageWindow (_("Error"),
                                    _("Rebuild of RPM "
                                      "database failed. You may be out of disk space?"));
                # XXX do something sane here.
                raise RuntimeError, "panic"

        self.method.targetFstab (self.mounts)

	if not self.installSystem: 
	    return

	for i in [ '/var', '/var/lib', '/var/lib/rpm', '/tmp', '/dev' ]:
	    try:
	        os.mkdir(self.instPath + i)
	    except os.error, (errno, msg):
                # self.intf.messageWindow("Error", "Error making directory %s: %s" % (i, msg))
                pass
        
	db = rpm.opendb(1, self.instPath)
	ts = rpm.TransactionSet(self.instPath, db)

        total = 0
	totalSize = 0

        if self.upgrade:
            how = "u"
        else:
            how = "i"
        
	for p in self.hdList.selected():
	    ts.add(p.h, p.h, how)
	    total = total + 1
	    totalSize = totalSize + p.h[rpm.RPMTAG_SIZE]

	ts.order()

        if self.upgrade:
            logname = '/tmp/upgrade.log'
        else:
            logname = '/tmp/install.log'
            
	self.instLog = open(self.instPath + logname, "w+")
	syslog = InstSyslog (self.instPath, self.instPath + logname)

	ts.scriptFd = self.instLog.fileno ()
	# the transaction set dup()s the file descriptor and will close the
	# dup'd when we go out of scope

	p = self.intf.packageProgressWindow(total, totalSize)

        if self.upgrade:
            self.modeText = _("Upgrading %s.\n")
        else:
            self.modeText = _("Installing %s.\n")

        rpm.errorSetCallback (self.rpmError)

        problems = ts.run(0, ~rpm.RPMPROB_FILTER_DISKSPACE,
                          self.instCallback, p)
        
        if problems:
            needed = {}
            size = 12
            for (descr, (type, mount, need)) in problems:
                idx = string.find (mount, "/mnt/sysimage")
                if idx != -1:
                    # 13 chars in /mnt/sysimage
                    mount = mount[13:]

                if needed.has_key (mount) and needed[mount] < need:
                    needed[mount] = need
                else:
                    needed[mount] = need
                    
            probs = _("You don't appear to have enough disk space to install "
                      "the packages you've selected. You need more space on the "
                      "following filesystems:\n\n")
            probs = probs + ("%-15s %s\n") % (_("Mount Point"), _("Space Needed"))
                    
            for (mount, need) in needed.items ():
                if need > (1024*1024):
                    need = (need + 1024 * 1024 - 1) / (1024 * 1024)
                    suffix = "M"
                else:
                    need = (need + 1023) / 1024,
                    suffix = "k"

                
                prob = "%-15s %d %c\n" % (mount, need, suffix)
                probs = probs + prob
                
            self.intf.messageWindow (_("Disk Space"), probs)

	    del ts
	    del db
	    self.instLog.close()
	    del syslog

            if self.setupFilesystems:
                self.umountFilesystems ()
            
            return 1

        # This should close the RPM database so that you can
        # do RPM ops in the chroot in a %post ks script
        del ts
        del db

        self.method.filesDone ()
        
        del p

        self.instLog.close ()

        w = self.intf.waitWindow(_("Post Install"), 
                                 _("Performing post install configuration..."))

        if not self.upgrade:
	    self.createCdrom()
	    self.copyExtraModules()
            self.writeFstab ()
            self.writeLanguage ()
            self.writeMouse ()
            self.writeKeyboard ()
            self.writeNetworkConfig ()
            self.writeRootPassword ()
            self.setupAuthentication ()
	    self.createAccounts ()
	    self.writeTimezone()
            self.writeDesktop ()
	    if (self.instClass.defaultRunlevel):
		self.initlevel = self.instClass.defaultRunlevel
		self.setDefaultRunlevel ()
            
            # pcmcia is supported only on i386 at the moment
            if arch == "i386":
                pcmcia.createPcmciaConfig(self.instPath + "/etc/sysconfig/pcmcia")
            self.copyConfModules ()
            if not self.x.skip and self.x.server:
		if self.x.server[0:3] == 'Sun':
                    try:
                        os.unlink(self.instPath + "/etc/X11/X")
                    except:
                        pass
		    script = open(self.instPath + "/etc/X11/X","w")
		    script.write("#!/bin/bash\n")
		    script.write("exec /usr/X11R6/bin/Xs%s -fp unix/:-1 $@\n" % self.x.server[1:])
		    script.close()
		    os.chmod(self.instPath + "/etc/X11/X", 0755)
		else:
		    os.symlink ("../../usr/X11R6/bin/XF86_" + self.x.server,
				self.instPath + "/etc/X11/X")
		self.x.write (self.instPath + "/etc/X11/XF86Config")
            self.setDefaultRunlevel ()
            argv = [ "/usr/sbin/kudzu", "-q" ]
	    devnull = os.open("/dev/null", os.O_RDWR)
	    iutil.execWithRedirect(argv[0], argv, root = self.instPath,
				   stdout = devnull)
        
        if arch == "sparc":
            self.silo.installSilo ()
        elif arch == "i386":
            self.installLilo ()
        elif arch == "alpha":
            self.milo.write ()
        else:
            raise RuntimeError, "What kind of machine is this, anyway?!"

	if self.instClass.postScript:
	    scriptRoot = "/"
	    if self.instClass.postInChroot:
		scriptRoot = self.instPath	

	    path = scriptRoot + "/tmp/ks-script"

	    f = open(path, "w")
	    f.write("#!/bin/sh\n\n")
	    f.write(self.instClass.postScript)
	    f.close()

	    if self.serial:
		messages = "/tmp/ks-script.log"
	    else:
		messages = "/dev/tty3"
	    iutil.execWithRedirect ("/bin/sh", ["/bin/sh", 
		    "/tmp/ks-script" ], stdout = messages,
		    stderr = messages, root = scriptRoot)
				    
	    #os.unlink(path)

        del syslog
        
        w.pop ()

