AM_CPPFLAGS = 					\
	@KDRIVE_INCS@				\
	@KDRIVE_CFLAGS@                         \
	-I$(top_srcdir)/glamor     \
	-I$(top_srcdir)/dri3       \
	$(XARCANMODULES_CFLAGS)
	$(GLAMOR_CFLAGS)     \
	$(GBM_CFLAGS)

noinst_LTLIBRARIES = libarcan.la

bin_PROGRAMS = Xarcan

libarcan_la_SOURCES =	\
	arcan.c		\
	arcan.h

GLAMOR_LIB = $(top_builddir)/glamor/libglamor.la
Xarcan_LDADD = 						\
	libarcan.la					\
	$(XARCANMODULES_LIBS)                           \
	$(GLAMOR_LIB)      \
	@KDRIVE_MAIN_LIB@				\
	@KDRIVE_LIBS@

Xarcan_SOURCES =          \
	arcaninit.c

# if ... += to SOURCES


Xarcan_LDFLAGS = $(LD_EXPORT_SYMBOLS_FLAG)

Xarcan_DEPENDENCIES =                                   \
	libarcan.la					\
	@KDRIVE_LOCAL_LIBS@

relink:
	$(AM_V_at)rm -f $(bin_PROGRAMS) && $(MAKE) $(bin_PROGRAMS)
