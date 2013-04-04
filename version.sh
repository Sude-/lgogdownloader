#!/bin/bash
version="LGOGDownloader `grep VERSION_NUMBER < main.cpp | head -n 1 | sed -e 's/.*\([0-9]\+\.[0-9]\+\).*/\1/'`"
if [ -e .git/HEAD ]; then
	if git status | grep -q 'modified:'; then
		version="${version}M"
	fi
	version="$version git `git rev-parse --short HEAD`"
fi
echo "$version"
