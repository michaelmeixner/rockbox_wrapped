# Used by ci/build-all.sh alongside a configured Rockbox build's Makefile:
#   make -s -f Makefile -f ci/print-plugins.mk print-plugins
# Prints the plugin sources this player builds (SOURCES after the target's
# config is applied), so the script can tell whether Wrapped is included.
print-plugins:
	@echo $(PLUGINS_SRC)
