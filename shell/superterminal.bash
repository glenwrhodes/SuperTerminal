# SuperTerminal bash integration.
#
# st starts bash with this file as its --rcfile (Linux, macOS, and inside WSL).
# It hooks Enter: each line is checked with bash itself (bash -n for syntax,
# type -t for every word; nothing runs), and lines that aren't runnable
# commands, or read like English, are sent to st to be translated. The result
# comes back as a real command on the prompt and runs in this same shell.
#
# Talks to st through `$SUPERTERMINAL_EXE --client $SUPERTERMINAL_PIPE <type>`.

# This file replaces ~/.bashrc as the rcfile, so load the user's first.
if [[ -f ~/.bashrc ]]; then . ~/.bashrc; fi

[[ $- == *i* && -n $SUPERTERMINAL_PIPE && -n $SUPERTERMINAL_EXE ]] || return 0
if ((BASH_VERSINFO[0] < 4)); then
    printf '\e[93mSuperTerminal needs bash 4 or newer for plain English (this is %s). Try --shell zsh.\e[0m\n' "$BASH_VERSION"
    return 0
fi

__st_exe=$SUPERTERMINAL_EXE
__st_pipe=$SUPERTERMINAL_PIPE
__st_suggestion=
__st_cont=
__st_go=1
__st_lastline=
__st_hist=()

__st_os=$(
    if [[ $(uname -s) == Darwin ]]; then
        printf 'macOS %s' "$(sw_vers -productVersion 2>/dev/null)"
    else
        . /etc/os-release 2>/dev/null
        printf '%s' "${PRETTY_NAME:-$(uname -s)}"
        grep -qi microsoft /proc/version 2>/dev/null && printf ' (WSL on Windows)'
    fi
)

# __st_call <type> [key value]... : one request to st; prints its reply.
__st_call() {
    local type=$1
    shift
    { while (($# >= 2)); do printf '%s\x1f%s\x1e' "$1" "$2"; shift 2; done; } |
        "$__st_exe" --client "$__st_pipe" "$type" 2>/dev/null
}

# "word<TAB>type" for each word, as bash sees it (alias, function, builtin, keyword, file, none).
__st_types() {
    local s=${1//[;|&()<>]/ } w t seen=' '
    local -a words
    read -ra words <<<"$s"
    for w in "${words[@]:0:40}"; do
        [[ $seen == *" $w "* ]] && continue
        seen+="$w "
        t=$(type -t -- "$w" 2>/dev/null)
        printf '%s\t%s\n' "$w" "${t:-none}"
    done
}

# Fast path: the first word is a command and nothing reads like English.
# Everything else goes to st, which decides properly.
__st_plain() {
    local line=$1 types=$2 first w
    [[ $line == '!!'* || $line == '??'* || $line == *'?' ]] && return 1
    read -r first _ <<<"$line"
    [[ $'\n'$types == *$'\n'"$first"$'\t'none* ]] && return 1
    local -a ws
    read -ra ws <<<"$line"
    local nocase=0
    shopt -q nocasematch && nocase=1
    shopt -s nocasematch
    local rc=0
    for w in "${ws[@]:1}"; do
        case $w in
        the | a | an | all | my | me | these | those | this | that | them | it | its | please | every | some | any | \
            what | which | how | why | who | is | are | can | could | would | should | than | there | here)
            rc=1
            break
            ;;
        esac
        [[ $first == dir && $w == /[a-z]* && ${#w} -le 4 ]] && rc=1 && break
    done
    ((nocase)) || shopt -u nocasematch
    return $rc
}

__st_listing() { CLICOLOR= LC_ALL=C command ls -lAh 2>/dev/null | head -n 81; }

__st_note() { # colour text
    local line
    while IFS= read -r line; do printf '\e[%sm  %s\e[0m\n' "$1" "$line"; done <<<"$2"
}

__st_exit() {
    printf '\n'
    builtin exit 0
}

__st_enter() {
    __st_go=1
    local line=$READLINE_LINE
    [[ -n $__st_cont || -z ${line//[[:space:]]/} ]] && return
    if [[ -n $__st_suggestion && $line == "$__st_suggestion" ]]; then
        __st_suggestion=
        return
    fi
    __st_suggestion=

    local syntax=ok err
    if ! err=$(bash -n <<<"$line" 2>&1); then
        if [[ $err == *"unexpected end of file"* || $err == *"unexpected EOF"* ]]; then syntax=incomplete; else syntax=error; fi
    fi
    local types
    types=$(__st_types "$line")
    if [[ $syntax == ok ]] && __st_plain "$line" "$types"; then return; fi
    # Unfinished input (open quote, if without fi): let bash continue - unless it reads like English.
    if [[ $syntax == incomplete ]] && __st_plain "$line" "$types"; then
        __st_cont=1
        return
    fi

    local reply action reason rest
    reply=$(__st_call route shell bash line "$line" syntax "$syntax" types "$types")
    if [[ -z $reply ]]; then
        [[ $syntax == incomplete ]] && __st_cont=1
        return
    fi
    action=${reply%%$'\n'*}
    rest=${reply#*$'\n'}
    reason=${rest%%$'\n'*}
    rest=${rest#*$'\n'}
    if [[ $action != ai ]]; then
        [[ $syntax == incomplete ]] && __st_cont=1
        if [[ -n $rest && $rest != "$line" ]]; then
            READLINE_LINE=$rest
            READLINE_POINT=${#rest}
        fi
        return
    fi
    __st_ask "$rest" "$reason" "$line"
}

__st_ask() {
    local request=$1 reason=$2 orig=$3
    printf '\n'
    local out pid
    out=$(mktemp "${TMPDIR:-/tmp}/st.XXXXXX") || return
    # In the background so Esc / Ctrl+C work while waiting; stderr hidden and
    # disowned so bash prints no job-control notices.
    {
        __st_call ask shell bash shellVersion "$BASH_VERSION" request "$request" reason "$reason" cwd "$PWD" \
            home "$HOME" os "$__st_os" listing "$(__st_listing)" history "$(printf '%s\n' "${__st_hist[@]}")" >"$out" &
        pid=$!
    } 2>/dev/null
    disown "$pid" 2>/dev/null
    local key dots=0
    while kill -0 "$pid" 2>/dev/null; do
        printf '\r\e[2K\e[90m  %s\e[0m' "$(printf '%*s' $((dots + 1)) '' | tr ' ' .)"
        dots=$(((dots + 1) % 3))
        if IFS= read -rsn1 -t 0.15 key; then
            case $key in
            $'\e')
                kill "$pid" 2>/dev/null
                printf '\r\e[2K'
                rm -f "$out"
                __st_go=0
                return
                ;;
            $'\x18')
                kill "$pid" 2>/dev/null
                rm -f "$out"
                __st_exit
                ;;
            esac
        fi
    done
    printf '\r\e[2K'

    local reply kind autorun note payload
    # Not $(<"$out"): inside bind -x that trips a bash 5.2 bug that breaks alias
    # expansion of the next command line (ls --color=auto became two commands).
    IFS= read -rd '' reply <"$out"
    rm -f "$out"
    kind=${reply%%$'\n'*}
    reply=${reply#*$'\n'}
    autorun=${reply%%$'\n'*}
    reply=${reply#*$'\n'}
    note=${reply%%$'\n'*}
    payload=${reply#*$'\n'}

    case $kind in
    command)
        if [[ -n $note ]]; then
            case $note in
            *deletes*) __st_note 91 "$note" ;;
            *changes*) __st_note 93 "$note" ;;
            *) __st_note 90 "$note" ;;
            esac
        fi
        history -s -- "$orig"
        READLINE_LINE=$payload
        READLINE_POINT=${#payload}
        if [[ $autorun == 1 ]]; then __st_go=1; else
            __st_go=0
            __st_suggestion=$payload
        fi
        ;;
    answer)
        __st_note 0 "$payload"
        history -s -- "$orig"
        READLINE_LINE=
        READLINE_POINT=0
        __st_go=0
        ;;
    passthrough)
        __st_go=1
        ;;
    *)
        __st_note 91 "${payload:-No reply.}"
        __st_go=0
        ;;
    esac
}

# Enter runs __st_enter, then accept-line only if it said so (__st_go).
__st_enter_then() {
    __st_enter
    if ((__st_go)); then
        __st_prompt_trap off # the command about to run gets normal Ctrl+C
        local km
        for km in emacs vi-insert vi-command; do bind -m "$km" '"\e[99992~": accept-line'; done
    else
        local km
        for km in emacs vi-insert vi-command; do bind -m "$km" '"\e[99992~": redraw-current-line'; done
    fi
}
for __st_km in emacs vi-insert vi-command; do
    bind -m "$__st_km" -x '"\e[99991~": __st_enter_then'
    bind -m "$__st_km" '"\e[99992~": accept-line'
    bind -m "$__st_km" '"\C-m": "\e[99991~\e[99992~"'
    bind -m "$__st_km" '"\C-j": "\e[99991~\e[99992~"'
done
unset __st_km

# Ctrl+C / Ctrl+X at the prompt exit. While a command runs, Ctrl+C keeps its
# normal meaning: the exiting INT trap is set only at the prompt (installed by
# PROMPT_COMMAND, removed by the Enter hook right before the command runs).
__st_user_int=$(trap -p INT)
__st_prompt_trap() {
    if [[ $1 == on ]]; then
        trap '__st_exit' INT
    elif [[ -n $__st_user_int ]]; then
        eval "$__st_user_int"
    else
        trap - INT
    fi
}
bind -x '"\C-x": __st_exit'
# Esc on its own clears the line (cancels a suggested command), as in PowerShell.
# Alt+key and arrow sequences still work: readline tells them apart by timing.
bind -m emacs '"\e": kill-whole-line'

__st_prompt() {
    local status=$? last
    last=$(HISTTIMEFORMAT= builtin history 1)
    [[ $last =~ ^[[:space:]]*[0-9]+[[:space:]]+(.*)$ ]] && last=${BASH_REMATCH[1]}
    if [[ -n $last && $last != "$__st_lastline" ]]; then
        __st_hist+=("$status"$'\t'"$last")
        ((${#__st_hist[@]} > 10)) && __st_hist=("${__st_hist[@]:1}")
        __st_lastline=$last
    fi
    __st_cont=
    __st_prompt_trap on
    return $status
}
PROMPT_COMMAND="__st_prompt${PROMPT_COMMAND:+; $PROMPT_COMMAND}"

st() {
    case ${1:-help} in
    model | effort) __st_call config key "$1" value "$2" ;;
    auto | autorun) __st_call config key autorun value "$2" ;;
    status | settings | config) __st_call config ;;
    *)
        printf '\e[96mSuperTerminal\e[0m: type commands as usual, or say what you want in plain English.\n\n'
        printf '  show me the biggest files here\n  which process is using port 3000?\n'
        printf '  dir /w                         \e[90m(PowerShell and cmd habits work too)\e[0m\n'
        printf '  fix it                         \e[90m(after a command fails)\e[0m\n\n'
        printf '  ?? <text>          treat the line as plain English, even if it looks like a command\n'
        printf '  !! <command>       run exactly as typed\n'
        printf '  Esc                cancel a suggested command, or a request in progress\n'
        printf '  Ctrl+C / Ctrl+X    exit SuperTerminal (Ctrl+C during a running command just stops it)\n\n'
        printf '  st status          settings\n'
        printf '  st auto on|off     run read-only answers straight away, or always wait for Enter\n\n'
        ;;
    esac
}

__st_hello=$(__st_call hello)
if [[ -n $__st_hello ]]; then
    printf '\e[96mSuperTerminal\e[0m %s  \e[90m·  bash %s\e[0m\n' "${__st_hello%%$'\n'*}" "${BASH_VERSION%%(*}"
    if [[ ${__st_hello#*$'\n'} == 1* ]]; then
        printf '\e[90mType commands as usual, or just say what you want. `st help` for more.\e[0m\n'
    else
        printf "\e[93mANTHROPIC_API_KEY isn't set, so plain-English requests are off.\e[0m\n"
    fi
fi
unset __st_hello
__st_prompt_trap on
