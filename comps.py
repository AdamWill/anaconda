import sys
import rpm
import os
from string import *
import types
import urllib
from translate import _

XFreeServerPackages = { 'XFree86-3DLabs' : 1, 	'XFree86-8514' : 1,
			'XFree86-AGX' : 1, 	'XFree86-I128' : 1,
			'XFree86-Mach32' : 1, 	'XFree86-Mach64' : 1,
			'XFree86-Mach8' : 1, 	'XFree86-Mono' : 1,
			'XFree86-P9000' : 1, 	'XFree86-S3' : 1,
			'XFree86-S3V' : 1, 	'XFree86-SVGA' : 1,
			'XFree86-VGA16' : 1,    'XFree86-W32' : 1 }

# Package selection is complicated. Here are the rules:
#
# Calling package.select() forces the package on. No other rules apply.
# Calling package.unselect() forces the package off. No other rules apply.
#
# Else:
#
# Each package contains a list of selection chains. Each chain consists
# of a set of components. If all of the components in any chain are selected,
# the package is selected.
#
# Otherwise, the package is not selected.

CHECK_CHAIN	= 0
FORCE_SELECT	= 1
FORCE_UNSELECT	= 2

def D_(foo):
    return foo

class Package:
    def __getitem__(self, item):
	return self.h[item]

    def __repr__(self):
	return "%s" % self.name

    def select(self):
	self.state = FORCE_SELECT
	self.selected = 1

    def unselect(self):
	self.state = FORCE_UNSELECT
	self.selected = 0

    def isSelected(self):
	return self.selected

    def updateSelectionCache(self):
	if self.state == FORCE_SELECT or self.state == FORCE_UNSELECT:
	    return

	self.selected = 0
	for chain in self.chains:
	    on = 1
	    for comp in chain:
		if not comp.isSelected(justManual = 0):
		    on = 0
	    if on: 
		self.selected = 1

    def getState(self):
	return (self.state, self.selected)

    def setState(self, state):
	(self.state, self.selected) = state

    def addSelectionChain(self, chain):
	self.chains.append(chain)

    def __init__(self, header):
	self.h = header
	self.chains = []
	self.selected = 0
	self.state = CHECK_CHAIN
	self.name = header[rpm.RPMTAG_NAME]
	self.size = header[rpm.RPMTAG_SIZE]

class HeaderList:
    def selected(self):
	l = []
 	keys = self.packages.keys()
	keys.sort()
	for name in keys:
	    if self.packages[name].selected: l.append(self.packages[name])
	return l

    def has_key(self, item):
	return self.packages.has_key(item)

    def keys(self):
        return self.packages.keys()

    def values(self):
        return self.packages.values()

    def __getitem__(self, item):
	return self.packages[item]

    def list(self):
	return self.packages.values()

    def __init__(self, hdlist, compatPackages = None):
        self.hdlist = hdlist
	self.packages = {}
	newCompat = []
	for h in hdlist:
	    name = h[rpm.RPMTAG_NAME]
	    score1 = rpm.archscore(h['arch'])
	    if (score1):
		if self.packages.has_key(name):
		    score2 = rpm.archscore(self.packages[name].h['arch'])
		    if (score1 < score2):
			newCompat.append(self.packages[name])
			self.packages[name] = Package(h)
		    else:
			newCompat.append(Package(h))
		else:
		    self.packages[name] = Package(h)
        if hdlist and not self.packages:
            raise RuntimeError, ("the header list was read, but no packages "
                                 "matching architecture '%s' were found."
                                 % os.uname()[4])

	if compatPackages != None:
	    # don't use list + here, as it creates a new object
	    for p in newCompat:
		compatPackages.append(p)

class HeaderListFromFile (HeaderList):

    def __init__(self, path, compatPackages = None):
	hdlist = rpm.readHeaderListFromFile(path)
	HeaderList.__init__(self, hdlist, compatPackages = compatPackages)

class HeaderListFD (HeaderList):
    def __init__(self, fd):
	hdlist = rpm.readHeaderListFromFD (fd)
	HeaderList.__init__(self, hdlist)

# A component has a name, a selection state, a list of included components,
# and a list of packages whose selection depends in some way on this component 
# being selected. Selection and deselection recurses through included 
# components.
#
# When the component file is parsed, the selection chain rules for the
# packages are built up. Component selection is used by the packages to
# determine whether or not they are selected.
#
# The selection state consists of a manually selected flag and an included
# selection count. They are separate to make UI coding easier.

class Component:
    def __len__(self):
	return len(self.pkgs)

    def __getitem__(self, key):
	return self.pkgs[key]

    def __repr__(self):
	return "comp %s" % (self.name)

    def __keys__(self):
	return self.pkgs.keys()

    def includesPackage(self, pkg):
	return self.pkgDict.has_key(pkg)

    def select(self, forInclude = 0):
	if forInclude:
	    self.selectionCount = self.selectionCount + 1
	else:
	    self.manuallySelected = 1

	for pkg in self.pkgs:
	    pkg.updateSelectionCache()

	for comp in self.includes:
	    comp.select(forInclude = 1)

    def isSelected(self, justManual = 0):
	# don't admit to selection-by-inclusion
	if justManual:
	    return self.manuallySelected

	return self.manuallySelected or (self.selectionCount > 0)

    def unselect(self, forInclude = 0):
	if forInclude:
	    self.selectionCount = self.selectionCount - 1
	    if self.selectionCount < 0:
		self.selectionCount = 0
	else:
	    self.manuallySelected = 0

	for pkg in self.pkgs:
	    pkg.updateSelectionCache()

	for comp in self.includes:
	    comp.unselect(forInclude = 1)

    def addInclude(self, comp):
	self.includes.append(comp)

    def addPackage(self, p):
	self.pkgs.append(p)
	p.addSelectionChain([self])
	self.pkgDict[p] = 1

    def addConditionalPackage(self, condComponent, p):
	self.pkgs.append(p)
	p.addSelectionChain([self, condComponent])
	self.pkgDict[p] = 1

    def setDefault(self, default):
        self.default = default

    def setDefaultSelection(self):
	if self.default:
	    self.select()

    def getState(self):
	return (self.manuallySelected, self.selectionCount)

    def setState(self, state):
	(self.manuallySelected, self.selectionCount) = state

    def __init__(self, name, selected, hidden = 0):
	self.name = name
	self.hidden = hidden
	self.default = selected
	self.pkgs = []
	self.pkgDict = {}
	self.includes = []
	self.manuallySelected = 0
	self.selectionCount = 0

class ComponentSet:
    def __len__(self):
	return len(self.comps)

    def __getitem__(self, key):
	if (type(key) == types.IntType):
	    return self.comps[key]
	return self.compsDict[key]

    def getSelectionState(self):
	compsState = []
	for comp in self.comps:
	    compsState.append((comp, comp.getState()))

	pkgsState = []
	for pkg in self.packages.list():
	    pkgsState.append((pkg, pkg.getState()))

	return (compsState, pkgsState)

    def setSelectionState(self, pickle):
	(compsState, pkgsState) = pickle

        for (comp, state) in compsState:
	    comp.setState(state)

	for (pkg, state) in pkgsState:
	    pkg.setState(state)
	    
    def sizeStr(self):
	megs = self.size()
	if (megs >= 1000):
	    big = megs / 1000
	    little = megs % 1000
	    return "%d,%03dM" % (big, little)

	return "%dM" % (megs)

    def totalSize(self):
	total = 0
	for pkg in self.packages.list():
	    total = total + (pkg[rpm.RPMTAG_SIZE] / 1024)
	return total

    def size(self):
	size = 0
	for pkg in self.packages.list():
	    if pkg.isSelected(): size = size + (pkg[rpm.RPMTAG_SIZE] / 1024)

	return size / 1024

    def keys(self):
	return self.compsDict.keys()

    def exprMatch(self, expr, arch1, arch2, matchAllLang = 0):
	if os.environ.has_key('LANG'):
	    lang = os.environ['LANG']
	else:
	    lang = None

	if expr[0] != '(':
	    raise ValueError, "leading ( expected"
	expr = expr[1:]
	if expr[len(expr) - 1] != ')':
	    raise ValueError, "bad kickstart file [missing )]"
	expr = expr[:len(expr) - 1]

	exprList = split(expr, 'and')
	truth = 1
	for expr in exprList:
	    l = split(expr)

	    if l[0] == "lang":
		if len(l) != 2:
		    raise ValueError, "too many arguments for lang"
		if l[1] != lang:
		    newTruth = 0
		else:
		    newTruth = 1

		if matchAllLang:
		    newTruth = 1
	    elif l[0] == "arch":
		if len(l) != 2:
		    raise ValueError, "too many arguments for arch"
		newTruth = self.archMatch(l[1], arch1, arch2)
	    else:
		s = "unknown condition type %s" % (l[0],)
		raise ValueError, s

	    truth = truth and newTruth

	return truth

    # this checks to see if "item" is one of the archs
    def archMatch(self, item, arch1, arch2):
	if item == arch1 or (arch2 and item == arch2): 
	    return 1
	return 0

    def readCompsFile(self, filename, packages, arch = None,
		      matchAllLang = 0):
	if not arch:
	    import iutil
	    arch = iutil.getArch()

# always set since with can have i386 arch with i686 arch2, for example
#	arch2 = None
#	if arch == "sparc" and os.uname ()[4] == "sparc64":
#	    arch2 = "sparc64"
#
        arch2 = os.uname ()[4]
	file = urllib.urlopen(filename)
	lines = file.readlines()

	file.close()
	top = lines[0]
	lines = lines[1:]
	if (top != "3\n" and top != "4\n"):
	    raise TypeError, "comp file version 3 or 4 expected"
	
	comp = None
        conditional = None
	self.comps = []
	self.compsDict = {}
	for l in lines:
	    l = strip (l)
	    if (not l): continue

	    if (find(l, ":") > -1):
		(archList, l) = split(l, ":", 1)
		if archList[0] == '(':
		    l = strip(l)
		    if not self.exprMatch(archList, arch, arch2, matchAllLang):
			continue
		else:
		    while (l[0] == " "): l = l[1:]

		    skipIfFound = 0
		    if (archList[0] == '!'):
			skipIfFound = 1
			archList = archList[1:]
		    archList = split(archList)
		    found = 0
		    for n in archList:
			if self.archMatch(n, arch, arch2):	
			    found = 1
			    break
		    if ((found and skipIfFound) or 
			(not found and not skipIfFound)):
			continue

	    if (find(l, "?") > -1):
                (trash, cond) = split (l, '?', 1)
                (cond, trash) = split (cond, '{', 1)
                conditional = self.compsDict[strip (cond)]
                continue

	    if (comp == None):
		(default, l) = split(l, None, 1)
		hidden = 0
		if (l[0:6] == "--hide"):
		    hidden = 1
		    (foo, l) = split(l, None, 1)
                (l, trash) = split(l, '{', 1)
                l = strip (l)
                if l == "Base":
                    hidden = 1
		comp = Component(l, default == '1', hidden)
	    elif (l == "}"):
                if conditional:
                    conditional = None
                else:
                    self.comps.append(comp)
                    self.compsDict[comp.name] = comp
                    comp = None
	    else:
		if (l[0] == "@"):
		    (at, l) = split(l, None, 1)
		    comp.addInclude(self.compsDict[l])
		else:
                    if conditional:
			# Let both components involved in this conditional
			# know what's going on.
                        comp.addConditionalPackage (conditional, packages[l])
			conditional.addConditionalPackage (comp, packages[l])
                    else:
                        comp.addPackage(packages[l])

        everything = Component(D_("Everything"), 0, 0)
        for package in packages.keys ():
	    if (packages[package][rpm.RPMTAG_NAME] != 'kernel' and
                packages[package][rpm.RPMTAG_NAME] != 'kernel-BOOT' and
#                packages[package][rpm.RPMTAG_NAME] != 'kernel-enterprise' and
                packages[package][rpm.RPMTAG_NAME] != 'kernel-smp' and
		  not XFreeServerPackages.has_key(packages[package][rpm.RPMTAG_NAME])):
		everything.addPackage (packages[package])
        self.comps.append (everything)
        self.compsDict["Everything"] = everything

	for comp in self.comps:
	    comp.setDefaultSelection()
        
    def __repr__(self):
	s = ""
	for n in self.comps:
	    s = s + "{ " + n.name + " [";
	    for include in n.includes:
		s = s + " @" + include.name

	    for package in n:
		s = s + " " + str(package)
	    s = s + " ] } "

	return s

    def __init__(self, file, hdlist, arch = None, matchAllLang = 0):
	self.packages = hdlist
	self.readCompsFile(file, self.packages, arch, matchAllLang)
