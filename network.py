#
# network.py - network configuration install data
#
# Matt Wilson <ewt@redhat.com>
# Erik Troan <ewt@redhat.com>
# Mike Fulbright <msf@redhat.com>
# Brent Fox <bfox@redhat.com>
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

import string
import isys
import socket
import os

from rhpl.log import log
from rhpl.translate import _, N_
from rhpl.simpleconfig import SimpleConfigFile

def inStrRange(v, s):
    if string.find(s, v) == -1:
	return 0
    else:
	return 1

def sanityCheckHostname(hostname):
    if len(hostname) < 1:
	return None

    if not inStrRange(hostname[0], string.ascii_letters):
	return _("Hostname must start with a valid character in the range "
		 "'a-z' or 'A-Z'")

    for i in range(1, len(hostname)):
	if not inStrRange(hostname[i], string.ascii_letters+string.digits+".-"):
	    return _("Hostnames can only contain the characters 'a-z', 'A-Z', '-', or '.'")

    return None
	    

def networkDeviceCheck(network, dispatch):
    devs = network.available()
    if not devs:
        dispatch.skipStep("network")

class NetworkDevice(SimpleConfigFile):
    def __str__(self):
        s = ""
        s = s + "DEVICE=" + self.info["DEVICE"] + "\n"
        keys = self.info.keys()
        keys.sort()
        keys.remove("DEVICE")

	# Don't let onboot be turned on unless we have config information
	# to go along with it
	if self.get('bootproto') != 'dhcp' and not self.get('ipaddr'):
	    forceOffOnBoot = 1
	else:
	    forceOffOnBoot = 0

	onBootWritten = 0
        for key in keys:
	    if key == 'ONBOOT' and forceOffOnBoot:
		s = s + key + "=" + 'no' + "\n"
	    else:
		s = s + key + "=" + self.info[key] + "\n"

	    if key == 'ONBOOT':
		onBootWritten = 1

	if not onBootWritten:
	    s = s + 'ONBOOT=no\n'

        return s

    def __init__(self, dev):
        self.info = { "DEVICE" : dev }

class Network:
    def __init__(self):
        self.netdevices = {}
        self.gateway = ""
        self.primaryNS = ""
        self.secondaryNS = ""
        self.ternaryNS = ""
        self.domains = []
	self.isConfigured = 0
        self.hostname = "localhost.localdomain"

	# this is only currently used in GUI
	# elsewhere we test if the hostname is localhost.localdomain
	# to see if its been override. Need some consolidation in future.
	self.overrideDHCPhostname = 0

        try:
            f = open("/tmp/netinfo", "r")
        except:
            pass
        else:
            lines = f.readlines()
	    f.close()
            info = {}
	    self.isConfigured = 1
            for line in lines:
                netinf = string.splitfields(line, '=')
                info [netinf[0]] = string.strip(netinf[1])
            self.netdevices [info["DEVICE"]] = NetworkDevice(info["DEVICE"])
            for key in ("IPADDR", "NETMASK", "BOOTPROTO", "ONBOOT", "REMIP", "MTU"):
                if info.has_key(key):
                    self.netdevices [info["DEVICE"]].set((key, info[key]))
            if info.has_key("GATEWAY"):
                self.gateway = info["GATEWAY"]
            if info.has_key("DOMAIN"):
                self.domains.append(info["DOMAIN"])
            if info.has_key("HOSTNAME"):
                self.hostname = info["HOSTNAME"]
            
	try:
	    f = open("/etc/resolv.conf", "r")
	except:
	    pass
	else:
	    lines = f.readlines()
	    f.close()
	    for line in lines:
		resolv = string.split(line)
		if resolv and resolv[0] == 'nameserver':
		    if self.primaryNS == "":
			self.primaryNS = resolv[1]
		    elif self.secondaryNS == "":
			self.secondaryNS = resolv[1]
		    elif self.ternaryNS == "":
			self.ternaryNS = resolv[1]

    def getDevice(self, device):
	return self.netdevices[device]

    def available(self):
        f = open("/proc/net/dev")
        lines = f.readlines()
        f.close()
        # skip first two lines, they are header
        lines = lines[2:]
        for line in lines:
            dev = string.strip(line[0:6])
            if dev != "lo" and not self.netdevices.has_key(dev):
                self.netdevices[dev] = NetworkDevice(dev)

        return self.netdevices

    def setHostname(self, hn):
	self.hostname = hn

    def setDNS(self, ns):
        self.primaryNS = ns

    def setGateway(self, gw):
        self.gateway = gw

    def lookupHostname(self):
	# can't look things up if they don't exist!
	if not self.hostname or self.hostname == "localhost.localdomain":
            return None
	if not self.primaryNS:
            return
	if not self.isConfigured:
	    for dev in self.netdevices.values():
		if dev.get('bootproto') == "dhcp":
		    self.primaryNS = isys.pumpNetDevice(dev.get('device'))
                    self.isConfigured = 1
                    break
		elif dev.get('ipaddr') and dev.get('netmask'):
                    try:
                        isys.configNetDevice(dev.get('device'),
                                             dev.get('ipaddr'),
                                             dev.get('netmask'),
                                             self.gateway)
                        self.isConfigured = 1
                        break
                    except SystemError:
                        log("failed to configure network device %s when "
                             "looking up host name", dev.get('device'))

	if not self.isConfigured:
            log("no network devices were availabe to look up host name")
            return None

	f = open("/etc/resolv.conf", "w")
	f.write("nameserver %s\n" % self.primaryNS)
	f.close()
	isys.resetResolv()
	isys.setResolvRetry(1)

	try:
	    ip = socket.gethostbyname(self.hostname)
	except:
	    return None

	return ip

    def nameservers(self):
        return (self.primaryNS, self.secondaryNS, self.ternaryNS)

    def writeKS(self, f):
	devNames = self.netdevices.keys()
	devNames.sort()
    
        if len(devNames) == 0:
            return

        for devName in devNames:
            dev = self.netdevices[devName]

            if dev.get('bootproto') == 'dhcp' or dev.get('ipaddr'):
                f.write("network --device %s" % dev.get('device'))
                if dev.get('bootproto') == 'dhcp':
                    f.write(" --bootproto dhcp")
                else:
                    f.write(" --bootproto static --ip %s --netmask %s --gateway %s" % 
                       (dev.get('ipaddr'), dev.get('netmask'), self.gateway))

                if dev.get('bootproto') != 'dhcp':
                    if self.primaryNS:
                        f.write(" --nameserver %s" % self.primaryNS)
                        
                        if (self.hostname and
                            self.hostname != "localhost.localdomain"):
                            f.write(" --hostname %s" % self.hostname)

                f.write("\n");

    def write(self, instPath):
        # /etc/sysconfig/network-scripts/ifcfg-*
        for dev in self.netdevices.values():
            device = dev.get("device")
	    fn = "%s/etc/sysconfig/network-scripts/ifcfg-%s" % (instPath,
                                                                device)
            f = open(fn, "w")
	    os.chmod(fn, 0644)
            f.write(str(dev))
            f.close()

        # /etc/sysconfig/network

        f = open(instPath + "/etc/sysconfig/network", "w")
        f.write("NETWORKING=yes\n"
                "HOSTNAME=")

        # use instclass hostname if set(kickstart) to override
        if self.hostname:
	    f.write(self.hostname + "\n")
	else:
	    f.write("localhost.localdomain\n")
	if self.gateway:
	    f.write("GATEWAY=%s\n" % (self.gateway,))
        f.close()

        # /etc/hosts
        f = open(instPath + "/etc/hosts", "w")
        localline = "127.0.0.1\t\t"

        log("self.hostname = %s", self.hostname)

	ip = self.lookupHostname()

	# If the hostname is not resolvable, tie it to 127.0.0.1
	if not ip and self.hostname != "localhost.localdomain":
	    localline = localline + self.hostname + " "
	    l = string.split(self.hostname, ".")
	    if len(l) > 1:
		localline = localline + l[0] + " "
                
	localline = localline + "localhost.localdomain localhost\n"
        f.write("# Do not remove the following line, or various programs\n")
        f.write("# that require network functionality will fail.\n")
        f.write(localline)

	if ip:
	    f.write("%s\t\t%s\n" % (ip, self.hostname))

	# If the hostname was not looked up, but typed in by the user,
	# domain might not be computed, so do it now.
	if self.domains == ["localdomain"] or not self.domains:
	    if '.' in self.hostname:
		# chop off everything before the leading '.'
		domain = self.hostname[(string.find(self.hostname, '.') + 1):]
		self.domains = [domain]

        # /etc/resolv.conf
        f = open(instPath + "/etc/resolv.conf", "w")

	if self.domains != ['localdomain'] and self.domains:
	    f.write("search %s\n" % (string.joinfields(self.domains, ' '),))

        for ns in self.nameservers():
            if ns:
                f.write("nameserver %s\n" % (ns,))

        f.close()

