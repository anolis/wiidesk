#!/bin/sh
# Hardware trial helper. Run as root; never prints or passes a password in argv.
# Account creation is explicit and refuses to reuse an existing account.
set -eu
trial=/var/tmp/wiidesk-x11-session-20260916
account=wiidesk-session-test
export DISPLAY=:1
case ${1:-} in
prepare|prepare-account)
    if id "$account" >/dev/null 2>&1; then
        echo 'Refusing to reuse an existing test account' >&2
        exit 1
    fi
    umask 077
    useradd -m -s /bin/sh "$account"
    od -An -N18 -tx1 /dev/urandom | tr -d ' \n' > "$trial/test-password"
    { printf '%s:' "$account"; cat "$trial/test-password"; printf '\n'; } | chpasswd
    if [ "$1" = prepare ]; then /usr/bin/xdm -config /usr/local/lib/wiidesk-session/xdm-config; fi
    ;;
cleanup)
    usermod -L "$account"
    rm -f "$trial/test-password" "$trial/previous-auth"
    userdel "$account"
    # Leave the disposable home for inspecting test logs, never remove recursively.
    ;;
*)
    server=$(tr -d ' ' < /tmp/.X1-lock)
    XAUTHORITY=$(tr '\0' '\n' < "/proc/$server/cmdline" | awk 'nextauth {print; exit} $0 == "-auth" {nextauth=1}')
    export XAUTHORITY
    test -n "$XAUTHORITY"
    case $1 in
    wrong-login)
        xdotool key ctrl+u
        xdotool type --clearmodifiers "$account"
        xdotool key Return
        sleep 2
        xdotool type --clearmodifiers intentionally-wrong-password
        xdotool key Return
        ;;
    login)
        xdotool key ctrl+u
        xdotool type --clearmodifiers "$account"
        xdotool key Return
        sleep 2
        xdotool type --clearmodifiers --file "$trial/test-password"
        xdotool key Return
        ;;
    open-system) xdotool key --clearmodifiers alt+F1 Down Down Return ;;
    is-locked) xprop -root _WIIDESK_SESSION_CAPS | grep -q ', 1$' ;;
    is-unlocked) xprop -root _WIIDESK_SESSION_CAPS | grep -q '= 1, 1, 0, 0$' ;;
    vt-shortcuts)
        before=$(cat /sys/class/tty/tty0/active)
        xdotool key --clearmodifiers ctrl+alt+F7 ctrl+alt+BackSpace
        sleep 1
        test "$(cat /sys/class/tty/tty0/active)" = "$before"
        kill -0 "$server"
        echo 'PASS: XTEST VT-switch and server-zap shortcuts leave this X server active'
        ;;
    remember-auth)
        umask 077
        cp "$XAUTHORITY" "$trial/previous-auth"
        ;;
    reject-old-auth)
        xdpyinfo >/dev/null
        if XAUTHORITY="$trial/previous-auth" xdpyinfo >/dev/null 2>&1; then
            echo 'FAIL: previous session authorization still accepted' >&2
            exit 1
        fi
        echo 'PASS: previous session authorization rejected'
        ;;
    access-check)
        xdotool search --onlyvisible --name '^xlogin$' >/dev/null
        xhost | grep '^access control enabled'
        xdotool key --clearmodifiers ctrl+plus
        xhost | grep '^access control enabled'
        xdotool key ctrl+u
        ;;
    lock) xdotool key --clearmodifiers ctrl+alt+l ;;
    wrong-unlock)
        xprop -root _WIIDESK_SESSION_CAPS | grep -q ', 1$'
        xdotool key space
        sleep 5
        xdotool type --clearmodifiers intentionally-wrong-password
        xdotool key Return
        ;;
    unlock)
        xprop -root _WIIDESK_SESSION_CAPS | grep -q ', 1$'
        xdotool key space
        sleep 5
        xdotool key ctrl+u
        xdotool type --clearmodifiers --file "$trial/test-password"
        xdotool key Return
        ;;
    logout)
        xdotool key --clearmodifiers alt+F1 Down Down Down Down Down Down Return
        sleep 1
        xdotool key Down Return Return
        ;;
    inspect)
        xprop -root _WIIDESK_SESSION_CAPS _WIIDESK_SESSION_STATUS _NET_CLIENT_LIST
        xwininfo -root -tree
        ps -eo pid,user,rss,args | awk '/xdm|Xorg :1|xsecurelock|wiidesk-session\/wiidesk/'
        ;;
    *) exit 2 ;;
    esac
    ;;
esac
