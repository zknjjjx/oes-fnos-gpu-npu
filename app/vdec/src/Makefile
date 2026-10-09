obj-m := oes_codec_io.o oes_vdec_alias.o oes_reg_ops.o oes_canvas.o oes_v4l2fix.o oes_clkfix.o
KDIR ?= /lib/modules/$(shell uname -r)/build
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) KBUILD_MODPOST_WARN=1 modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
