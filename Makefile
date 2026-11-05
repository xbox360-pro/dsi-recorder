# DSi Recorder - BlocksDS makefile
BLOCKSDS	?= /opt/blocksds/core

NAME		:= DSiRecorder
GAME_TITLE	:= DSi Recorder
GAME_SUBTITLE	:= Camera video recorder
GAME_AUTHOR	:= Leon

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile
