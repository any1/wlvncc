#!/bin/sh
#
# This is an example script that displays a pop-up dialogue for user
# authentication.
# It can be enabled via the --auth-command option. E.g.
#     wlvncc --auth-command=$HOME/projects/wlvncc/scripts/auth-script.h my-vnc-server.local

ENTRY=$(zenity --password --username)

case $? in
	0)
		echo "$ENTRY" | tr '|' "\n"
		;;
	1)
		echo "Stop login." &>2
		;;
	-1)
		echo "An unexpected error has occurred." &>2
		;;
esac
