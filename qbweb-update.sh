#!/bin/bash
# qbweb-update.sh - bring the QBone/UniBone software up to date from GitHub.
#
# Downloads the "control-panel" branch of n9tax/QUniBoneClassic-web, copies
# the files that changed into this directory (normally root's home), recompiles
# "demo" when its sources changed, and restarts the web control panel when it
# changed.
#
#   ./qbweb-update.sh             update, ask before copying
#   ./qbweb-update.sh -y          update without questions
#   ./qbweb-update.sh -n          only show what would change
#   ./qbweb-update.sh -a          also recompile everything ("compile.sh -a")
#   ./qbweb-update.sh -b <branch> -r <owner/repo>   other branch or repository
#
# Not touched: disk images, the stock scripts from files.retrocmp.com,
# qunibone-platform.env, your configurations in ~/.qbcontrol. Files that were
# deleted from the repository are not deleted here.
# Files that are overwritten are saved first in ~/.qbweb-backup/<date>/.
#
# First time, as root on the BeagleBone:
#   cd ~
#   wget https://raw.githubusercontent.com/n9tax/QUniBoneClassic-web/control-panel/qbweb-update.sh
#   chmod +x qbweb-update.sh
#   ./qbweb-update.sh

# Everything is inside main(): bash has read the whole script before it runs,
# so it may replace itself with a newer version while updating.
main() {
    local REPO=n9tax/QUniBoneClassic-web
    local BRANCH=control-panel
    local DRY=0 YES=0 FULL=0
    local opt
    OPTIND=1
    while getopts "r:b:nyah" opt; do
        case $opt in
            r) REPO=$OPTARG ;;
            b) BRANCH=$OPTARG ;;
            n) DRY=1 ;;
            y) YES=1 ;;
            a) FULL=1 ;;
            *) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; return 2 ;;
        esac
    done

    # The QUniBone tree is where this script lives (normally ~).
    local DEST
    DEST="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
    cd "$DEST" || return 1

    if [ ! -f qunibone-platform.env ] || [ ! -d 10.03_app_demo ]; then
        echo "$DEST does not look like a configured QUniBone tree"
        echo "(qunibone-platform.env or 10.03_app_demo missing). Put this script into"
        echo "the home directory that holds the QUniBone software and run it there."
        return 1
    fi
    if [ "$(id -u)" != "0" ] && [ $DRY = 0 ] && [ -z "$QBWEB_NO_ROOT_CHECK" ]; then
        echo "Please run as root (the QUniBone software and the control panel run as root)."
        return 1
    fi

    TMP="$(mktemp -d /tmp/qbweb-update.XXXXXX)" || return 1
    trap 'rm -rf "$TMP"' EXIT

    # --- download
    local URL="https://github.com/$REPO/archive/refs/heads/$BRANCH.tar.gz"
    echo "Downloading $REPO, branch $BRANCH ..."
    if command -v wget >/dev/null; then
        wget -q -O "$TMP/src.tar.gz" "$URL"
    else
        curl -fsSL -o "$TMP/src.tar.gz" "$URL"
    fi
    if [ $? -ne 0 ] || [ ! -s "$TMP/src.tar.gz" ]; then
        echo "Download failed: $URL"
        echo "Check the network connection, and the branch and repository names."
        return 1
    fi
    mkdir "$TMP/tree"
    if ! tar xzf "$TMP/src.tar.gz" -C "$TMP/tree" --strip-components=1; then
        echo "The download is not a valid archive."
        return 1
    fi

    # --- compare: only copy files whose content differs, so "make" rebuilds
    #     just what changed
    local f new=() changed=() skipped=()
    while IFS= read -r -d '' f; do
        f="${f#./}"
        if [ -L "$DEST/$f" ]; then
            skipped+=("$f")            # links made by qunibone-platform.sh
        elif [ ! -e "$DEST/$f" ]; then
            new+=("$f")
        elif ! cmp -s "$TMP/tree/$f" "$DEST/$f"; then
            changed+=("$f")
        fi
    done < <(cd "$TMP/tree" && find . -type f -print0 | sort -z)

    local total=$(( ${#new[@]} + ${#changed[@]} ))
    if [ $total -eq 0 ]; then
        echo "Already up to date."
    else
        echo
        echo "$total file(s) to update:"
        for f in "${changed[@]}"; do echo "  changed  $f"; done
        for f in "${new[@]}"; do echo "  new      $f"; done
    fi
    for f in "${skipped[@]}"; do echo "  skipped  $f (is a link here)"; done
    [ $DRY = 1 ] && return 0

    local demo_bin="10.03_app_demo/4_deploy/demo"
    local pending="$DEST/.qbweb-compile-pending"   # last compile failed
    if [ $total -eq 0 ] && [ $FULL = 0 ] && [ -x "$demo_bin" ] && [ ! -e "$pending" ]; then
        return 0
    fi

    if [ $total -gt 0 ] && [ $YES = 0 ]; then
        echo
        read -r -p "Copy these files into $DEST [y/N]? " || return 1
        [[ $REPLY =~ ^[Yy]$ ]] || { echo "Nothing changed."; return 1; }
    fi

    # --- back up and copy
    local backup="$DEST/.qbweb-backup/$(date +%Y-%m-%d_%H%M%S)"
    if [ ${#changed[@]} -gt 0 ]; then
        for f in "${changed[@]}"; do
            mkdir -p "$backup/$(dirname "$f")" && cp -p "$DEST/$f" "$backup/$f"
        done
        echo "Previous versions saved in $backup"
    fi
    for f in "${changed[@]}" "${new[@]}"; do
        mkdir -p "$DEST/$(dirname "$f")"
        # no -p: a new modification time makes "make" rebuild what depends on it
        cp -f "$TMP/tree/$f" "$DEST/$f" || { echo "Could not write $DEST/$f"; return 1; }
        [ -x "$TMP/tree/$f" ] && chmod +x "$DEST/$f"
    done
    find . -maxdepth 4 -name '*.sh' -type f -exec chmod +x '{}' \;
    chmod +x 10.05_control/qbcontrold 10.05_control/qbmenu 2>/dev/null

    # --- what has to happen now?
    local need_compile=$FULL panel_changed=0 panel_install=0
    for f in "${changed[@]}" "${new[@]}"; do
        case "$f" in
            10.01_*|10.02_devices/2_src/*|10.03_app_demo/2_src/*|10.04_*|90_common/*|91_3rd_party/*|compile*.sh|compile*.env)
                need_compile=1 ;;
            10.05_control/install.sh|10.05_control/qbcontrol.service)
                panel_changed=1; panel_install=1 ;;
            10.05_control/*)
                panel_changed=1 ;;
        esac
    done
    [ -x "$demo_bin" ] || need_compile=1
    [ -e "$pending" ] && need_compile=1

    # old conflicting sources, and the _q/_u links (as update-code.sh does)
    if [ -x ./cleanup.sh ]; then ./cleanup.sh >/dev/null; fi
    if [ -x ./qunibone-platform.sh ] && [ $total -gt 0 ]; then
        ./qunibone-platform.sh >/dev/null || echo "Warning: qunibone-platform.sh reported an error."
    fi

    local demo_running=0
    pgrep -x demo >/dev/null && demo_running=1

    # --- recompile demo
    local compiled=0 errors=0
    if [ $need_compile = 1 ]; then
        echo
        echo "Compiling demo (this takes a while on the BeagleBone) ..."
        local before=0
        [ -e "$demo_bin" ] && before=$(stat -c %Y "$demo_bin")
        if [ $FULL = 1 ]; then ./compile.sh -a; else ./compile.sh; fi
        cd "$DEST" || return 1
        if [ -x "$demo_bin" ] && [ "$(stat -c %Y "$demo_bin")" -gt "$before" ]; then
            compiled=1
            rm -f "$pending"
            echo "demo compiled."
        else
            echo
            echo "*** demo was not rebuilt. Look for errors in the compiler output above."
            echo "*** The previous demo program is still in place."
            errors=1
            touch "$pending"
        fi
    fi

    # --- control panel
    if [ $panel_changed = 1 ]; then
        echo
        if [ -f /etc/systemd/system/qbcontrol.service ]; then
            local restart=1
            if [ $demo_running = 1 ] && [ $YES = 0 ]; then
                read -r -p "Restart the control panel now? This stops the running emulation [y/N]? "
                [[ $REPLY =~ ^[Yy]$ ]] || restart=0
            fi
            if [ $restart = 0 ]; then
                echo "Control panel updated; it takes effect after: systemctl restart qbcontrol"
            elif [ $panel_install = 1 ]; then
                # keep the user's choice about the login menu
                local opts=""
                grep -q ">>> qbcontrol menu >>>" "$DEST/.profile" 2>/dev/null || opts="--no-menu"
                (cd 10.05_control && ./install.sh $opts)
            else
                systemctl restart qbcontrol && echo "Control panel restarted."
            fi
        else
            echo "The control panel is not installed yet. To install it:"
            echo "  cd $DEST/10.05_control && ./install.sh"
        fi
    fi

    echo
    if [ $compiled = 1 ] && [ $demo_running = 1 ] && pgrep -x demo >/dev/null; then
        echo "An emulation is still running the old demo. Restart it to use the new one."
    fi
    if [ $errors = 1 ]; then
        echo "demo did not compile (see above). The next run of this script tries again."
        return 1
    fi
    echo "Update complete."
    return 0
}

main "$@"
exit $?
