if XEPHYR
XEPHYR_SUBDIRS = ephyr
endif

if XARCANSERVER
XARCAN_SUBDIRS = arcan
endif

SERVER_SUBDIRS = 		\
	$(XEPHYR_SUBDIRS) \
	$(XARCAN_SUBDIRS)

SUBDIRS =			\
	src			\
	$(SERVER_SUBDIRS)

DIST_SUBDIRS = ephyr arcan src

relink:
	$(AM_V_at)for i in $(SERVER_SUBDIRS) ; do $(MAKE) -C $$i relink || exit 1 ; done
