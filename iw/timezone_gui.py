#
# timezone_gui.py: gui timezone selection.
#
# Copyright 2000-2002 Red Hat, Inc.
#
# This software may be freely redistributed under the terms of the GNU
# library public license.
#
# You should have received a copy of the GNU Library Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
#

import string
import iutil
import gtk
import gobject
from timezone_map_gui import TimezoneMap, ZoneTab
from iw_gui import *
from rhpl.translate import _

class TimezoneWindow(InstallWindow):
    def __init__(self, ics):
	InstallWindow.__init__(self, ics)

        ics.setTitle(_("Time Zone Selection"))
        ics.setNextEnabled(1)
        ics.readHTML("timezone")
        self.old_page = 0
        self.old_use_dst = 0

	self.timeZones = ((("-14", ""), ("Etc/GMT-14", "Etc/GMT-14")),
                          (("-13", ""), ("Etc/GMT-13", "Etc/GMT-13")),
                          (("-12", ""), ("Etc/GMT-12", "Etc/GMT-12")),
                          (("-11", ""), ("Etc/GMT-11", "Etc/GMT-11")),
                          (("-10", ""), ("Etc/GMT-10", "Etc/GMT-10")),
                          (("-09", ""), ("Etc/GMT-9", "Etc/GMT-9")),
                          (("-08", "US Pacific"),  ("Etc/GMT-8", "America/Los_Angeles")),
                          (("-07", "US Mountain"), ("Etc/GMT-7", "America/Denver")),
                          (("-06", "US Central"),  ("Etc/GMT-6", "America/Chicago")),
                          (("-05", "US Eastern"),  ("Etc/GMT-5", "America/New_York")),
                          (("-04", ""), ("Etc/GMT-4", "Etc/GMT-4")),
                          (("-03", ""), ("Etc/GMT-3", "Etc/GMT-3")),
                          (("-02", ""), ("Etc/GMT-2", "Etc/GMT-2")),
                          (("-01", ""), ("Etc/GMT-1", "Etc/GMT-1")),
                          (("",       ""), ("Etc/GMT", "Etc/GMT")),
                          (("+01", ""), ("Etc/GMT+1", "Etc/GMT+1")),
                          (("+02", ""), ("Etc/GMT+2", "Etc/GMT+2")),
                          (("+03", ""), ("Etc/GMT+3", "Etc/GMT+3")),
                          (("+04", ""), ("Etc/GMT+4", "Etc/GMT+4")),
                          (("+05", ""), ("Etc/GMT+5", "Etc/GMT+5")),
                          (("+06", ""), ("Etc/GMT+6", "Etc/GMT+6")),
                          (("+07", ""), ("Etc/GMT+7", "Etc/GMT+7")),
                          (("+08", ""), ("Etc/GMT+8", "Etc/GMT+8")),
                          (("+09", ""), ("Etc/GMT+9", "Etc/GMT+9")),
                          (("+10", ""), ("Etc/GMT+10", "Etc/GMT+10")),
                          (("+11", ""), ("Etc/GMT+11", "Etc/GMT+11")),
                          (("+12", ""), ("Etc/GMT+12", "Etc/GMT+12")))                    

    def getNext(self):
        newzone = self.tz.getCurrent().tz
        self.timezone.setTimezoneInfo(newzone, self.systemUTC.get_active())

        return None

    def copy_toggled(self, cb1, cb2):
        if cb1.get_data("toggling"): return
        
        cb2.set_data("toggling", 1)
        cb2.set_active(cb1.get_active ())
        cb2.set_data("toggling", 0)

    def view_change(self, widget, *args):
        if not self.tz.getCurrent():
            self.ics.setNextEnabled(gtk.FALSE)
        else:
            self.ics.setNextEnabled(gtk.TRUE)

    # TimezoneWindow tag="timezone"
    def getScreen(self, instLang, timezone):
	self.timezone = timezone

        try:
            f = open("/usr/share/anaconda/pixmaps/map480.png")
            f.close()
        except:
            path = "pixmaps/map480.png"
        else:
            path = "/usr/share/anaconda/pixmaps/map480.png"
        
        mainBox = gtk.VBox(gtk.FALSE, 5)

        zonetab = ZoneTab()
        self.tz = TimezoneMap(zonetab=zonetab, map=path)

	(self.default, asUTC, asArc) = self.timezone.getTimezoneInfo()

        self.old_page = timezone.utcOffset
        self.old_use_dst = timezone.dst
        self.langDefault = instLang.getDefaultTimeZone()

	if not self.default:
            self.default = self.langDefault
	    asUTC = 0

        if (string.find(self.default, "UTC") != -1):
            self.default = "America/New_York"

        self.tz.setCurrent(zonetab.findEntryByTZ(self.default))

        systemUTCCopy = gtk.CheckButton(_("System clock uses _UTC"))
        self.systemUTC = gtk.CheckButton(_("System clock uses _UTC"))

        systemUTCCopy.connect("toggled", self.copy_toggled, self.systemUTC)
        self.systemUTC.connect("toggled", self.copy_toggled, systemUTCCopy)

        self.systemUTC.set_active(asUTC)

        hbox = gtk.HBox(gtk.FALSE, 5)
#        pix = self.ics.readPixmap("timezone.png")
#        if pix:
#            a = gtk.Alignment()
#            a.add(pix)
#            a.set(1.0, 0.0, 0.0, 0.0)
#            hbox.pack_start(a, gtk.TRUE)
        
        mainBox.pack_start(self.tz, gtk.TRUE, gtk.TRUE)
        mainBox.set_border_width(5)

        align = gtk.Alignment(0.5, 0.5)
        align.add(self.systemUTC)
        hbox.pack_start(align, gtk.FALSE)
        mainBox.pack_start(hbox, gtk.FALSE)

        box = gtk.VBox(gtk.FALSE, 5)
        box.pack_start(mainBox)
        box.set_border_width(5)

        return box

