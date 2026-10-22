#!/system/bin/sh
# KernelSU invokes this before stopping Android userspace/netd. Release our own
# queues/routes while the old image is still installed; service.sh later honors
# the existing autostart and enabled policy. Never use --stop (a user pause).
MODDIR=${0%/*}
"$MODDIR/AD_Killer_By_NFQUEUE" --prepare-reboot
