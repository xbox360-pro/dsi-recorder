# DSi Recorder - BlocksDS makefile
BLOCKSDS	?= /opt/blocksds/core

NAME		:= DSiRecorder
GAME_TITLE	:= DSi Recorder
GAME_SUBTITLE	:= Camera, photos & videos
GAME_AUTHOR	:= Leon
GAME_ICON	:= icon.gif

# DSiWare-style header with its own game code, so the .nds can also be
# installed to the DSi's internal memory (NAND) with a title manager.
GAME_CODE	:= KDRC
GAME_MAKER	:= 01

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile

# Same as the default ROM rule, plus the game code (title ID 00030004-KDRC)
$(ROM): $(ELF)
	@echo "  NDSTOOL $@"
	$(V)$(BLOCKSDS)/tools/ndstool/ndstool -c $@ \
		-7 $(ARM7ELF) -9 $(ELF) \
		-b $(GAME_ICON) "$(GAME_FULL_TITLE)" \
		-g $(GAME_CODE) $(GAME_MAKER) "DSIRECORDER" \
		-u 00030004 \
		$(NDSTOOL_ARGS)
