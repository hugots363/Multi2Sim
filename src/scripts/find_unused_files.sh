#!/bin/bash

find . \( -name '*.c' -or -name '*.h' \) -exec bash -c '\
	DIR=`dirname {}`; \
	FILE=`basename {}`; \
	if [ ! -f $DIR/Makefile.am ]; then exit 0; fi; \
	grep -q $FILE $DIR/Makefile.am; \
	if [ $? -ne 0 ]; then \
		echo {}; \
	fi; \
' \;

