# Makefile for the kernel image.

u=/usr
CC=	exec cc
CFLAGS=	-O -D_MINIX -D_POSIX_SOURCE
MDEC=	/usr/mdec

# Specify the programs that are part of the system image.
PROGRAMS=	../kernel/kernel \
	../servers/pm/pm \
	../servers/vfs/vfs \
	../servers/rs/rs \
	../drivers/memory/memory \
	../drivers/log/log \
	../drivers/tty/tty \
	../servers/ds/ds \
	../servers/ss/ss \
	../servers/mfs/mfs \
	../servers/vm/vm \
	../servers/pfs/pfs \
	../servers/init/init

usage:	
	@echo " " >&2
	@echo "Master Makefile to create new MINIX configuration." >& 2
	@echo "Root privileges are required." >&2
	@echo " " >&2
	@echo "Usage:" >&2
	@echo "	make includes   # Install include files" >&2
	@echo "	make depend     # Generate dependency files" >&2
	@echo "	make libraries  # Make system libraries" >&2
	@echo "	make services   # Compile and install all services" >&2
	@echo "	make fresh      # Make clean, libraries, and services" >&2
	@echo "	make image      # Make needed services and create boot image" >&2
	@echo "	make install    # Make image, and install to hard disk" >&2
	@echo "	make hdboot     # Make image, and install to hard disk" >&2
	@echo "	make fdboot     # Make image, and install to floppy disk" >&2
	@echo "	make bootable   # Make hard disk bootable" >&2
	@echo "	make clean      # Remove all compiler results, except libs" >&2
	@echo " " >&2
	@echo "To create a fresh MINIX configuration, try:" >&2
	@echo "	make clean install      # new boot image" >&2
	@echo "	make fresh install      # new everything" >&2
	@echo " " >&2

# create a fresh configuration or system image
fresh: 
	cd ../lib && $(MAKE) clean
	$(MAKE) clean 
	$(MAKE) libraries services

all: services image

image:  includes
	cd ../kernel && $(MAKE) EXTRA_OPTS=$(EXTRA_OPTS)
	cd ../servers && $(MAKE) image
	cd ../drivers && $(MAKE) image
	installboot -image $@ $(PROGRAMS)

image_small:  includes
	cd ../kernel && $(MAKE) EXTRA_OPTS=$(EXTRA_OPTS)
	cd ../servers && $(MAKE) EXTRA_OPTS=-D_MINIX_SMALL=1 image
	cd ../drivers && $(MAKE) EXTRA_OPTS=$(EXTRA_OPTS) image
	installboot -image $@ $(PROGRAMS)

# rebuild the program or system libraries
includes:
	cd ../include && $(MAKE) install

depend: includes
	cd ../ && $(MAKE) depend

services: includes 
	cd ../kernel && $(MAKE) 
	cd ../servers && $(MAKE) install
	cd ../drivers && $(MAKE) install

libraries: includes
	cd ../lib && $(MAKE) clean
	cd ../lib && $(MAKE) all
	cd ../lib && $(MAKE) install


# make bootable and place system images
bootable:
	exec su root mkboot bootable

hdboot:	image
	exec sh mkboot $@
	@sync

fdboot:	image
	exec su root mkboot $@
	@sync

install: includes services hdboot

# clean up compile results
clean:
	cd ../kernel && $(MAKE) $@
	cd ../servers && $(MAKE) $@
	cd ../drivers && $(MAKE) $@
	rm -rf *.bak image image_small *.iso *.iso.gz cdfdimage rootimage src

