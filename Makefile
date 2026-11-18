CC         ?= cc
CFLAGS     ?= -O2 -Wall -Wno-deprecated-declarations
FILTER_DIR  = /usr/libexec/cups/filter
QUEUE      ?= Ricoh_SP_150SUw
PPD         = Ricoh-SP150SUw-LHPL.ppd

all: rastertolhpl sp150ctl

rastertolhpl: rastertolhpl.c jbig.c jbig_ar.c jbig.h jbig_ar.h
	$(CC) $(CFLAGS) -I. -o $@ rastertolhpl.c jbig.c jbig_ar.c -lcups

sp150ctl: sp150ctl.c
	$(CC) $(CFLAGS) -o $@ sp150ctl.c -framework IOKit -framework CoreFoundation

# sudo make install  -> installs the filter and adds the USB print queue
install: rastertolhpl
	@[ "$$(id -u)" -eq 0 ] || { echo "run: sudo make install"; exit 1; }
	install -o root -g wheel -m 755 rastertolhpl $(FILTER_DIR)/rastertolhpl
	@uri=$$(lpinfo --include-schemes usb -v 2>/dev/null | awk '/usb:\/\/RICOH\/SP/ {print $$2; exit}'); \
	 [ -n "$$uri" ] || { echo "Ricoh SP 150 not found on USB - connected and switched on?"; exit 1; }; \
	 echo "Adding queue $(QUEUE) -> $$uri"; \
	 lpadmin -p $(QUEUE) -E -D "Ricoh SP 150SUw" -L "USB" -v "$$uri" -P $(PPD) -o printer-is-shared=false
	lpstat -p $(QUEUE) -v $(QUEUE)

uninstall:
	@[ "$$(id -u)" -eq 0 ] || { echo "run: sudo make uninstall"; exit 1; }
	-lpadmin -x $(QUEUE)
	rm -f $(FILTER_DIR)/rastertolhpl

testpage:
	python3 tools/make_testpdf.py testpage.pdf && lp -d $(QUEUE) testpage.pdf

clean:
	rm -f rastertolhpl sp150ctl testpage.pdf

.PHONY: all install uninstall testpage clean
