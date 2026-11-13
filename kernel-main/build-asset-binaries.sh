# Build ELF assets

# The libc every program links (built once, rebuilt only when it changes)

(cd ../libc      && ./build.sh)

# Libraries programs load at run time (sys/lib)

(cd ../programs/stblib    && ./build.sh)
(cd ../programs/tlslib    && ./build.sh)

(cd ../programs/dwm       && ./build.sh)
(cd ../programs/explorer  && ./build.sh)
(cd ../programs/prop      && ./build.sh)
(cd ../programs/fileop    && ./build.sh)
(cd ../programs/console   && ./build.sh)
(cd ../programs/format    && ./build.sh)
(cd ../programs/loadlib   && ./build.sh)
(cd ../programs/runlib    && ./build.sh)

# Services

(cd ../programs/netsrv    && ./build.sh)
(cd ../programs/audiosrv  && ./build.sh)

# Settings

(cd ../programs/devmgr   && ./build.sh)
(cd ../programs/desktop  && ./build.sh)
(cd ../programs/datetime && ./build.sh)
(cd ../programs/regedit  && ./build.sh)

# Programs / Features

(cd ../programs/paint    && ./build.sh)
(cd ../programs/hexedit  && ./build.sh)
(cd ../programs/charmap  && ./build.sh)
(cd ../programs/calc     && ./build.sh)
(cd ../programs/notepad  && ./build.sh)
(cd ../programs/wget     && ./build.sh)
(cd ../programs/htmlview && ./build.sh)
(cd ../programs/httpd    && ./build.sh)
(cd ../programs/mediaplay && ./build.sh)
(cd ../programs/play      && ./build.sh)
(cd ../programs/rec       && ./build.sh)

# Games

(cd ../programs/minesweeper  && ./build.sh)
