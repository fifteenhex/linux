# SPDX-License-Identifier: GPL-2.0
dtb-y += \
	mvme147.dtb \
	eltec-e17.dtb

obj-y += embedded_dtbs.o

# embedded_dtbs.S .incbin's the blobs, so make sure they are built first
$(obj)/embedded_dtbs.o: $(addprefix $(obj)/, $(dtb-y))
