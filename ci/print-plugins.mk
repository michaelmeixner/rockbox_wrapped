# make -s -f Makefile -f ci/print-plugins.mk print-plugins
# Prints the plugins this player builds (used by build-all.sh).
print-plugins:
	@echo $(PLUGINS_SRC)
