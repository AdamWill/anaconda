include Makefile.inc

VERSION = 7.1
RELEASE = 5
SNAPRELEASE = $(RELEASE)$(shell date "+.%Y%m%d%H%M")

ifneq (ia64, $(ARCH))
MINISLANG=minislang
STUBS=stubs
endif

SUBDIRSHD = balkan isys libfdisk collage $(MINISLANG) loader po text-help \
	    textw utils scripts bootdisk installclasses \
	    keymaps fonts gnome-map iw help pixmaps $(STUBS)
SUBDIRS = $(SUBDIRSHD)

ifeq (i386, $(ARCH))
SUBDIRS := ddcprobe edd $(SUBDIRS)
endif

# DESTDIR        - destination for install image for install purposes
DESTDIR = ../../../RedHat/instimage

CATALOGS = po/anaconda.pot

PYFILES = $(wildcard *.py)

all: subdirs _xkb.so xmouse.so $(CATALOGS) lang-table

_xkb.so: xkb.c
	gcc -Wall -o _xkb.o -O2 -fPIC -I/usr/include/python1.5 `gtk-config --cflags gtk` -c xkb.c 
	gcc -o _xkb.so -shared _xkb.o /usr/X11R6/lib/libxkbfile.a `gtk-config --libs gtk`

xmouse.so: xmouse.c
	gcc -Wall -o xmouse.o -fPIC -I/usr/X11R6/include -I/usr/include/python1.5 -I /usr/include/python1.5 -c xmouse.c 
	gcc -o xmouse.so -shared xmouse.o /usr/X11R6/lib/libXxf86misc.a -L/usr/X11R6/lib -lX11 -lXext

depend:
	rm -f *.o *.so *.pyc
	for d in $(SUBDIRS); do make -C $$d depend; done

clean:
	rm -f *.o *.so *.pyc
	for d in $(SUBDIRS); do make -C $$d clean; done

subdirs:
	for d in $(SUBDIRS); do make -C $$d; [ $$? = 0 ] || exit 1; done

install: 
	@if [ "$(DESTDIR)" = "" ]; then \
		echo " "; \
		echo "ERROR: A destdir is required"; \
		exit 1; \
	fi

	mkdir -p $(DESTDIR)/usr/bin
	mkdir -p $(DESTDIR)/usr/sbin
	mkdir -p $(DESTDIR)/etc/rc.d/init.d
	mkdir -p $(DESTDIR)/$(PYTHONLIBDIR)
	mkdir -p $(DESTDIR)/$(RUNTIMEDIR)

	cp -a reconfig.init $(DESTDIR)/etc/rc.d/init.d/reconfig
	install -m 755 anaconda $(DESTDIR)/usr/sbin/anaconda
	install -m 755 anaconda-stub $(DESTDIR)/$(RUNTIMEDIR)
	cp -var $(PYFILES) $(DESTDIR)/$(PYTHONLIBDIR)
	cp -a lang-table $(DESTDIR)/$(PYTHONLIBDIR)
	cp -a lang-table-kon $(DESTDIR)/$(PYTHONLIBDIR)
	./py-compile --basedir $(DESTDIR)/$(PYTHONLIBDIR) $(PYFILES)
	cp -a *.so $(DESTDIR)/$(PYTHONLIBDIR)
	strip $(DESTDIR)/$(PYTHONLIBDIR)/*.so
	cp -a raid*stub $(DESTDIR)/$(PYTHONLIBDIR)
	cp -a losetup-stub $(DESTDIR)/$(PYTHONLIBDIR)
	cp -a pump-stub $(DESTDIR)/$(PYTHONLIBDIR)
	for d in $(SUBDIRS); do make DESTDIR=`cd $(DESTDIR); pwd` -C $$d install; [ $$? = 0 ] || exit 1; done

archive: create-archive

src: create-archive
	@rpm -ts --nodeps anaconda-$(VERSION).tar.bz2

snapsrc: create-snapshot
	@rpm -ts --nodeps anaconda-$(VERSION).tar.bz2

create-snapshot:
	@rm -rf /tmp/anaconda
	@rm -rf /tmp/anaconda-$(VERSION)
	@tag=`cvs status Makefile | awk ' /Sticky Tag/ { print $$3 } '` 2> /dev/null; \
	[ x"$$tag" = x"(none)" ] && tag=HEAD; \
	echo "*** Pulling off $$tag!"; \
	cd /tmp ; cvs -Q -d $(CVSROOT) export -r $$tag anaconda || echo "Um... export aborted."
	@cd /tmp/anaconda ; rm isys/modutils/modutils.spec
	@cd /tmp/anaconda ; rm -rf comps
	@cd /tmp/anaconda ; sed -e "s/@@VERSION@@/$(VERSION)/g" -e "s/@@RELEASE@@/$(SNAPRELEASE)/g" < anaconda.spec.in > anaconda.spec
	@mv /tmp/anaconda /tmp/anaconda-$(VERSION)
	@cd /tmp ; tar --bzip2 -cSpf anaconda-$(VERSION).tar.bz2 anaconda-$(VERSION)
	@rm -rf /tmp/anaconda-$(VERSION)
	@cp /tmp/anaconda-$(VERSION).tar.bz2 .
	@rm -f /tmp/anaconda-$(VERSION).tar.bz2
	@echo ""
	@echo "The final archive is in anaconda-$(VERSION).tar.bz2"

create-archive:
	make SNAPRELEASE=$(RELEASE) create-snapshot
