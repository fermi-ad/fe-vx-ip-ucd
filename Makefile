# When changing VID, make sure to change the version number namespace
# in `ip-ucd.h`.

VID = 1.0
PRODUCT = 1

SUPPORTED_VERSIONS = 64 69
SUPPORTED_VXWORKS_64_TARGETS = mv2434
SUPPORTED_VXWORKS_69_TARGETS = mv5500

MOD_TARGETS = ip-ucd.out
HEADER_TARGETS = ip-ucd.h
LIB_TARGETS = libip-ucd.a

include ${PRODUCTS_INCDIR}/frontend-latest.mk

ip-ucd.out : ip-ucd.o ${PRODUCTS_LIBDIR}/libvwpp-3.0.a
	${make-mod-munch}

libip-ucd.a : ip-ucd.o
	${make-lib}

test.out : test.o libip-ucd.a ${PRODUCTS_LIBDIR}/libvwpp-3.0.a
	${make-mod-munch}

ip-ucd.o test.o : ip-ucd.h
