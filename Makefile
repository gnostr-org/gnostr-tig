## Makefile for tig

all:

# Include setting from the configure script
-include config.make

prefix ?= $(HOME)
bindir ?= $(prefix)/bin
mandir ?= $(prefix)/man
datarootdir ?= $(prefix)/share
docdir ?= $(datarootdir)/doc
# DESTDIR=

# Get version either via git or from VERSION file. Allow either
# to be overwritten by setting DIST_VERSION on the command line.
ifneq (,$(wildcard .git))
GITDESC	= $(subst tig-,,$(shell git describe))
WTDIRTY	= $(if $(shell git diff-index HEAD 2>/dev/null),-dirty)
VERSION	= $(GITDESC)$(WTDIRTY)
else
VERSION	= $(shell test -f VERSION && cat VERSION || echo "unknown-version")
endif
ifdef DIST_VERSION
VERSION = $(DIST_VERSION)
endif

# Split the version "TAG-OFFSET-gSHA1-DIRTY" into "TAG OFFSET"
# and append 0 as a fallback offset for "exact" tagged versions.
RPM_VERLIST = $(filter-out g% dirty,$(subst -, ,$(VERSION))) 0
RPM_VERSION = $(word 1,$(RPM_VERLIST))
RPM_RELEASE = $(word 2,$(RPM_VERLIST))$(if $(WTDIRTY),.dirty)

LDLIBS ?= -lcurses
CFLAGS ?= -Wall -O2
DFLAGS	= -g -DDEBUG -Werror
PROGS	= tig
MANDOC	= tig.1 tigrc.5
HTMLDOC = tig.1.html tigrc.5.html manual.html README.html
ALLDOC	= $(MANDOC) $(HTMLDOC) manual.html-chunked manual.pdf
TARNAME	= tig-$(RPM_VERSION)-$(RPM_RELEASE)

override CFLAGS += '-DTIG_VERSION="$(VERSION)"'

AUTORECONF ?= autoreconf
ASCIIDOC ?= asciidoc
XMLTO ?= xmlto
DOCBOOK2PDF ?= docbook2pdf

all: $(PROGS)
all-debug: $(PROGS)
all-debug: CFLAGS += $(DFLAGS)
doc: $(ALLDOC)
doc-man: $(MANDOC)
doc-html: $(HTMLDOC)

install: all
	mkdir -p $(DESTDIR)$(bindir) && \
	for prog in $(PROGS); do \
		install -p -m 0755 $$prog $(DESTDIR)$(bindir); \
	done

install-doc-man: doc-man
	mkdir -p $(DESTDIR)$(mandir)/man1 \
		 $(DESTDIR)$(mandir)/man5
	for doc in $(MANDOC); do \
		case "$$doc" in \
		*.1) install -p -m 0644 $$doc $(DESTDIR)$(mandir)/man1 ;; \
		*.5) install -p -m 0644 $$doc $(DESTDIR)$(mandir)/man5 ;; \
		esac \
	done

install-doc-html: doc-html
	mkdir -p $(DESTDIR)$(docdir)/tig
	for doc in $(HTMLDOC); do \
		case "$$doc" in \
		*.html) install -p -m 0644 $$doc $(DESTDIR)$(docdir)/tig ;; \
		esac \
	done

install-doc: install-doc-man install-doc-html

clean:
	rm -rf manual.html-chunked $(TARNAME)
	rm -f $(PROGS) $(ALLDOC) core *.o *.xml *.toc
	rm -f *.spec tig-*.tar.gz tig-*.tar.gz.md5

spell-check:
	aspell --lang=en --check tig.1.txt tigrc.5.txt manual.txt

strip: $(PROGS)
	strip $(PROGS)

dist: tig.spec
	@mkdir -p $(TARNAME) && \
	cp tig.spec $(TARNAME) && \
	echo $(VERSION) > $(TARNAME)/VERSION
	git archive --format=tar --prefix=$(TARNAME)/ HEAD | \
	tar --delete $(TARNAME)/VERSION > $(TARNAME).tar && \
	tar rf $(TARNAME).tar $(TARNAME)/tig.spec $(TARNAME)/VERSION && \
	gzip -f -9 $(TARNAME).tar && \
	md5sum $(TARNAME).tar.gz > $(TARNAME).tar.gz.md5
	@rm -rf $(TARNAME)

rpm: dist
	rpmbuild -ta $(TARNAME).tar.gz

configure: configure.ac acinclude.m4
	$(AUTORECONF) -v

# Maintainer stuff
release-doc:
	git checkout release && \
	git merge master && \
	$(MAKE) clean doc-man doc-html && \
	git add -f $(MANDOC) $(HTMLDOC) && \
	git commit -m "Sync docs" && \
	git checkout master

release-dist: release-doc
	git checkout release && \
	$(MAKE) dist && \
	git checkout master

.PHONY: all all-debug doc doc-man doc-html install install-doc \
	install-doc-man install-doc-html clean spell-check dist rpm

tig.o: tig.c
tig: tig.o

tig.spec: contrib/tig.spec.in
	sed -e 's/@@VERSION@@/$(RPM_VERSION)/g' \
	    -e 's/@@RELEASE@@/$(RPM_RELEASE)/g' < $< > $@

manual.html: manual.toc
manual.toc: manual.txt
	sed -n '/^\[\[/,/\(---\|~~~\)/p' < $< | while read line; do \
		case "$$line" in \
		"-----"*)  echo ". <<$$ref>>"; ref= ;; \
		"~~~~~"*)  echo "- <<$$ref>>"; ref= ;; \
		"[["*"]]") ref="$$line" ;; \
		*)	   ref="$$ref, $$line" ;; \
		esac; done | sed 's/\[\[\(.*\)\]\]/\1/' > $@

README.html: README
	$(ASCIIDOC) -b xhtml11 -d article -a readme $<

%.pdf : %.xml
	$(DOCBOOK2PDF) $<

%.1.html : %.1.txt
	$(ASCIIDOC) -b xhtml11 -d manpage $<

%.1.xml : %.1.txt
	$(ASCIIDOC) -b docbook -d manpage -aversion=$(VERSION) $<

%.1 : %.1.xml
	$(XMLTO) -m manpage.xsl man $<

%.5.html : %.5.txt
	$(ASCIIDOC) -b xhtml11 -d manpage $<

%.5.xml : %.5.txt
	$(ASCIIDOC) -b docbook -d manpage -aversion=$(VERSION) $<

%.5 : %.5.xml
	$(XMLTO) -m manpage.xsl man $<

%.html : %.txt
	$(ASCIIDOC) -b xhtml11 -d article -n $<

%.xml : %.txt
	$(ASCIIDOC) -b docbook -d article $<

%.html-chunked : %.xml
	$(XMLTO) html -o $@ $<
