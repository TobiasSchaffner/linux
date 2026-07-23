# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I $(src)		# needed for trace events

obj-$(CONFIG_RV) += rv.o
obj-$(CONFIG_RV_MON_WIP) += monitors/wip/wip.o
obj-$(CONFIG_RV_MON_WWNR) += monitors/wwnr/wwnr.o
obj-$(CONFIG_RV_MON_SCHED) += monitors/sched/sched.o
obj-$(CONFIG_RV_MON_SCO) += monitors/sco/sco.o
obj-$(CONFIG_RV_MON_SNROC) += monitors/snroc/snroc.o
obj-$(CONFIG_RV_MON_SCPD) += monitors/scpd/scpd.o
obj-$(CONFIG_RV_MON_SNEP) += monitors/snep/snep.o
obj-$(CONFIG_RV_MON_RTAPP) += monitors/rtapp/rtapp.o
obj-$(CONFIG_RV_MON_PAGEFAULT) += monitors/pagefault/pagefault.o
obj-$(CONFIG_RV_MON_SLEEP) += monitors/sleep/sleep.o
obj-$(CONFIG_RV_MON_STS) += monitors/sts/sts.o
obj-$(CONFIG_RV_MON_NRP) += monitors/nrp/nrp.o
obj-$(CONFIG_RV_MON_SSSW) += monitors/sssw/sssw.o
obj-$(CONFIG_RV_MON_OPID) += monitors/opid/opid.o
obj-$(CONFIG_RV_MON_STALL) += monitors/stall/stall.o
obj-$(CONFIG_RV_MON_DEADLINE) += monitors/deadline/deadline.o
obj-$(CONFIG_RV_MON_NOMISS) += monitors/nomiss/nomiss.o
# Add new monitors here
obj-$(CONFIG_RV_MON_EVL) += monitors/evl/evl.o
obj-$(CONFIG_RV_REACTORS) += rv_reactors.o
obj-$(CONFIG_RV_REACT_PRINTK) += reactor_printk.o
obj-$(CONFIG_RV_REACT_PANIC) += reactor_panic.o

# Per-edge dwell-stat RV: generate the per-monitor transition table
# from a GraphViz .dot topology with embedded metadata (event
# bindings + ``timed`` edge attributes). Generated headers live
# under include/generated/rv/ so they are tree-private and not
# checked into git (see .gitignore).
rvevl_root := $(srctree)/tools/verification
rvevl_rvgen := $(rvevl_root)/rvgen
rvevl_dot2c := $(rvevl_rvgen)/rvgen/dot2c.py
rvevl_automata := $(rvevl_rvgen)/rvgen/automata.py
rvevl_outdir := $(objtree)/include/generated/rv

# Composed monitor table: emit state/event/edge enums and the
# transition lookup tables from a single annotated .dot file.
quiet_cmd_rvevl_monitor = MONITOR $@
      cmd_rvevl_monitor = mkdir -p $(rvevl_outdir) && \
                         PYTHONPATH=$(rvevl_rvgen) python3 -c \
                         "from rvgen.dot2c import Dot2c; open('$@','w').write(Dot2c('$<').format_table_header())"

$(rvevl_outdir)/evl_table.h: \
		$(rvevl_root)/models/evl.dot \
		$(rvevl_dot2c) \
		$(rvevl_automata) \
		FORCE
	$(call if_changed,rvevl_monitor)

# Pin the composed monitor object to its generated header so a model
# edit triggers a rebuild.
$(obj)/monitors/evl/evl.o: $(rvevl_outdir)/evl_table.h
